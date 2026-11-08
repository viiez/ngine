// Beleuchtung der Szenen-Shader (pbr.slang, terrain.slang): Sonne mit Schatten aus Kaskaden,
// Liste weiterer Lichter, Umgebung aus Himmel und Boden (ADR 0028). Wer die Datei einbindet,
// braucht vorher eine globale Variable push mit einem Feld frame (Frame*).

static const float pi = 3.14159265359;

// GGX/Trowbridge-Reitz mit alpha = roughness².
float distribution_ggx(float n_dot_h, float alpha)
{
    let a2 = alpha * alpha;
    let d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (pi * d * d);
}

// Höhenkorrelierte Smith-Sichtbarkeit; enthält schon den Nenner 4 (n·l)(n·v).
float visibility_smith(float n_dot_l, float n_dot_v, float alpha)
{
    let a2 = alpha * alpha;
    let v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    let l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    let sum = v + l;
    return sum > 0.0 ? 0.5 / sum : 0.0;
}

float3 fresnel_schlick(float3 f0, float v_dot_h)
{
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

float3 hemisphere(float3 direction)
{
    return lerp(push.frame.ground_color.rgb, push.frame.sky_color.rgb, direction.y * 0.5 + 0.5);
}


// Licht aus Richtung l mit Strahldichte 1: Cook-Torrance (GGX, Smith, Schlick) und Lambert.
float3 direct(float3 n, float3 v, float3 l, float n_dot_v, float3 f0, float3 diffuse_color,
              float alpha)
{
    let n_dot_l = saturate(dot(n, l));
    if (n_dot_l <= 0.0)
    {
        return float3(0.0);
    }
    let h = normalize(v + l);
    let n_dot_h = saturate(dot(n, h));
    let v_dot_h = saturate(dot(v, h));
    let f = fresnel_schlick(f0, v_dot_h);
    let specular = f * distribution_ggx(n_dot_h, alpha) * visibility_smith(n_dot_l, n_dot_v, alpha);
    let diffuse = (1.0 - f) * diffuse_color / pi;
    return (diffuse + specular) * n_dot_l;
}

// Bilinear gefilterter Vergleich mit der Schattenkarte über Gather: 1 beleuchtet, 0 im Schatten.
float compare_bilinear(uint map, float2 uv, float depth)
{
    let frame = push.frame;
    let texels = bindless_textures[NonUniformResourceIndex(map)].Gather(
        bindless_samplers[frame.options.w], uv);
    // Gather liefert w = oben links, z = oben rechts, x = unten links, y = unten rechts.
    let lit = float4(depth <= texels.x ? 1.0 : 0.0, depth <= texels.y ? 1.0 : 0.0,
                     depth <= texels.z ? 1.0 : 0.0, depth <= texels.w ? 1.0 : 0.0);
    let f = frac(uv * frame.shadow_params.z - 0.5);
    let top = lerp(lit.w, lit.z, f.x);
    let bottom = lerp(lit.x, lit.y, f.x);
    return lerp(top, bottom, f.y);
}

// Schatten aus einer Kaskade: PCF über 3 × 3 bilinear gefilterte Proben. Der Punkt wird entlang
// der Normale um einige Texel der Kaskade verschoben, damit flache Flächen sich nicht selbst
// beschatten.
float cascade_shadow(uint cascade, float3 world, float3 normal, float n_dot_l)
{
    let frame = push.frame;
    let offset = frame.cascade_texel[cascade] * frame.shadow_params.x * (1.0 - 0.5 * n_dot_l);
    let p = float4(world + normal * offset, 1.0);
    let base = cascade * 3;
    let s = float3(dot(frame.cascade_rows[base], p), dot(frame.cascade_rows[base + 1], p),
                   dot(frame.cascade_rows[base + 2], p));
    let uv = float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || s.z >= 1.0)
    {
        return 1.0;
    }
    let map = frame.shadow_maps[cascade];
    let step = frame.shadow_params.y;
    var sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            sum += compare_bilinear(map, uv + float2(x, y) * step, s.z);
        }
    }
    return sum / 9.0;
}

// Schatten der Sonne: Kaskade nach Entfernung entlang der Blickrichtung, am Ende jeder Kaskade
// weicher Übergang in die nächste, hinter der letzten ausgeblendet.
float sun_shadow(float3 world, float3 normal, float3 l, float view_depth, out uint cascade)
{
    let frame = push.frame;
    cascade = cascade_count;
    if (frame.options.y == 0 || view_depth >= frame.cascade_far[cascade_count - 1])
    {
        return 1.0;
    }
    cascade = 0;
    while (cascade < cascade_count - 1 && view_depth >= frame.cascade_far[cascade])
    {
        ++cascade;
    }
    let n_dot_l = saturate(dot(normal, l));
    var shadow = cascade_shadow(cascade, world, normal, n_dot_l);
    let blend_start = frame.cascade_blend[cascade];
    if (view_depth > blend_start)
    {
        let t = saturate((view_depth - blend_start) /
                         max(frame.cascade_far[cascade] - blend_start, 1e-4));
        let next = cascade + 1 < cascade_count ? cascade_shadow(cascade + 1, world, normal, n_dot_l)
                                                : 1.0;
        shadow = lerp(shadow, next, t);
    }
    return shadow;
}

// Licht am Punkt world (relativ zur Kamera) einer Oberfläche mit Normale n (nach Normalenkarte)
// und geometrischer Normale geometric, Grundfarbe base und Metallic-Roughness-Werten.
float3 shade(float3 world, float3 n, float3 geometric, float3 base, float metallic, float roughness,
             float occlusion)
{
    let alpha = roughness * roughness;
    let v = normalize(-world);
    let n_dot_v = max(dot(n, v), 1e-4);
    let f0 = lerp(float3(0.04), base, metallic);
    let diffuse_color = base * (1.0 - metallic);
    let frame = push.frame;
    var color = float3(0.0);

    // Sonne mit Schatten aus den Kaskaden.
    let sun = frame.sun_direction.xyz;
    let view_depth = dot(world, frame.camera_forward.xyz);
    var cascade = cascade_count;
    let shadow = sun_shadow(world, geometric, sun, view_depth, cascade);
    color += direct(n, v, sun, n_dot_v, f0, diffuse_color, alpha) * frame.sun_color.rgb * shadow;

    // Weitere Lichter aus der Liste, ohne Schatten.
    for (uint i = 0; i < frame.options.x; ++i)
    {
        let light = frame.lights[i];
        let type = uint(light.color_type.w + 0.5);
        var l = -light.direction.xyz;
        var attenuation = 1.0;
        if (type != 0)
        {
            let to_light = light.position_range.xyz - world;
            let d2 = max(dot(to_light, to_light), 1e-8);
            l = to_light * rsqrt(d2);
            attenuation = 1.0 / max(d2, 1e-4);
            let range = light.position_range.w;
            if (range > 0.0)
            {
                // Weiches Ende an der Reichweite wie in KHR_lights_punctual.
                let ratio2 = d2 / (range * range);
                let window = saturate(1.0 - ratio2 * ratio2);
                attenuation *= window * window;
            }
            if (type == 2)
            {
                let cone = saturate(dot(light.direction.xyz, -l) * light.spot.x + light.spot.y);
                attenuation *= cone * cone;
            }
        }
        if (attenuation > 0.0)
        {
            color += direct(n, v, l, n_dot_v, f0, diffuse_color, alpha) * light.color_type.rgb *
                     attenuation;
        }
    }

    // Umgebung aus Himmel und Boden: diffus entlang der Normale, glänzend entlang der Spiegelung.
    // Ersatz für bildbasiertes Licht, das in R3 mit dem Himmel kommt.
    let r = reflect(-v, n);
    let f_ambient = f0 + (max(float3(1.0 - roughness), f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    let glossy = lerp(hemisphere(r), hemisphere(n), roughness);
    color += (diffuse_color * hemisphere(n) + f_ambient * glossy) * occlusion;
    if (frame.options.z != 0 && cascade < cascade_count)
    {
        // Kaskaden zur Prüfung einfärben: rot, grün, blau, gelb.
        static const float3 tints[4] = {
            float3(1.0, 0.35, 0.35), float3(0.35, 1.0, 0.35), float3(0.35, 0.5, 1.0),
            float3(1.0, 1.0, 0.35),
        };
        color = color * tints[cascade] + tints[cascade] * 0.02;
    }
    return color;
}
