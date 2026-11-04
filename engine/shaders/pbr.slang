// Vorwärts-Pass mit PBR-Materialien nach glTF Metallic-Roughness (REND-07, R1.3). Geometrie kommt
// über Vertex Pulling (ADR 0022), Materialien als Feld in einem Puffer mit bindlosen Texturindizes
// (ADR 0027). Gerechnet wird relativ zur Kamera (ADR 0012): Die Kamera steht im Ursprung.

#include "bindless.slang"

static const uint no_texture = 0xFFFFFFFF;
static const float pi = 3.14159265359;

// Wie asset::Vertex: 12 Floats ohne Lücken, unabhängig von den Ausrichtungsregeln.
struct Vertex
{
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;
    float u, v;
};

// Wie render::GpuMaterial (96 Bytes).
struct Material
{
    float4 base_color;
    // xyz: Emission, w: Schwelle der Alpha-Maske
    float4 emissive_cutoff;
    // x: Metallic, y: Roughness, z: Stärke der Normalenkarte, w: Stärke der Okklusion
    float4 params;
    // Texturen: Grundfarbe, Metallic-Roughness, Normalen, Okklusion
    uint4 textures;
    // Sampler in derselben Reihenfolge
    uint4 samplers;
    // x: Emissionstextur, y: ihr Sampler, z: Alpha-Modus (0 undurchsichtig, 1 Maske, 2 Blend)
    uint4 extra;
};

// Wie render::GpuFrame.
struct Frame
{
    // Zeilen der Matrix von kamerarelativen Welt- in Clip-Koordinaten
    float4 view_projection[4];
    // xyz: Richtung zur Sonne (normiert), w: unbenutzt
    float4 sun_direction;
    // rgb: Farbe mal Stärke der Sonne
    float4 sun_color;
    float4 sky_color;
    float4 ground_color;
};

struct PushConstants
{
    // Zeilen der affinen Matrix von Objekt- in kamerarelative Weltkoordinaten
    float4 model[3];
    Vertex* vertices;
    Frame* frame;
    Material* materials;
    uint material;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    float3 normal : NORMAL0;
    float4 tangent : TANGENT0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let p = float4(v.px, v.py, v.pz, 1.0);
    let world = float3(dot(push.model[0], p), dot(push.model[1], p), dot(push.model[2], p));
    let w = float4(world, 1.0);

    // Spalten des linearen Teils; Normalen transformiert die Adjunkte (Kofaktoren), die auch bei
    // ungleicher Skalierung senkrecht bleibt.
    let c0 = float3(push.model[0].x, push.model[1].x, push.model[2].x);
    let c1 = float3(push.model[0].y, push.model[1].y, push.model[2].y);
    let c2 = float3(push.model[0].z, push.model[1].z, push.model[2].z);
    let n = float3(v.nx, v.ny, v.nz);
    let t = float3(v.tx, v.ty, v.tz);

    VertexOutput output;
    let vp = push.frame.view_projection;
    output.position = float4(dot(vp[0], w), dot(vp[1], w), dot(vp[2], w), dot(vp[3], w));
    output.world = world;
    output.normal = cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z;
    output.tangent = float4(c0 * t.x + c1 * t.y + c2 * t.z, v.tw);
    output.uv = float2(v.u, v.v);
    return output;
}

float4 sample_or(uint texture, uint sampler, float2 uv, float4 fallback)
{
    return texture == no_texture ? fallback : sample_texture(texture, sampler, uv);
}

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

[shader("fragment")]
float4 fragment_main(VertexOutput input, bool front : SV_IsFrontFace) : SV_Target
{
    let m = push.materials[push.material];
    let uv = input.uv;

    var base = m.base_color * sample_or(m.textures.x, m.samplers.x, uv, float4(1.0));
    if (m.extra.z == 1 && base.a < m.emissive_cutoff.w)
    {
        discard;
    }
    let mr = sample_or(m.textures.y, m.samplers.y, uv, float4(1.0));
    let metallic = saturate(m.params.x * mr.b);
    let roughness = clamp(m.params.y * mr.g, 0.03, 1.0);
    let alpha = roughness * roughness;

    var n = normalize(input.normal);
    if (!front)
    {
        n = -n; // Rückseite doppelseitiger Materialien
    }
    if (m.textures.z != no_texture)
    {
        var t = input.tangent.xyz - n * dot(n, input.tangent.xyz);
        if (dot(t, t) > 1e-12)
        {
            t = normalize(t);
            let b = cross(n, t) * (input.tangent.w < 0.0 ? -1.0 : 1.0);
            var local = sample_texture(m.textures.z, m.samplers.z, uv).xyz * 2.0 - 1.0;
            local.xy *= m.params.z;
            n = normalize(t * local.x + b * local.y + n * local.z);
        }
    }
    let occlusion = lerp(1.0, sample_or(m.textures.w, m.samplers.w, uv, float4(1.0)).r, m.params.w);
    let emissive = m.emissive_cutoff.xyz * sample_or(m.extra.x, m.extra.y, uv, float4(1.0)).rgb;

    let v = normalize(-input.world);
    let l = push.frame.sun_direction.xyz;
    let h = normalize(v + l);
    let n_dot_v = max(dot(n, v), 1e-4);
    let n_dot_l = saturate(dot(n, l));
    let n_dot_h = saturate(dot(n, h));
    let v_dot_h = saturate(dot(v, h));

    let f0 = lerp(float3(0.04), base.rgb, metallic);
    let diffuse_color = base.rgb * (1.0 - metallic);
    let f = fresnel_schlick(f0, v_dot_h);
    let specular = f * distribution_ggx(n_dot_h, alpha) * visibility_smith(n_dot_l, n_dot_v, alpha);
    let diffuse = (1.0 - f) * diffuse_color / pi;
    var color = (diffuse + specular) * push.frame.sun_color.rgb * n_dot_l;

    // Umgebung aus Himmel und Boden: diffus entlang der Normale, glänzend entlang der Spiegelung.
    // Ersatz für bildbasiertes Licht, das in R3 mit dem Himmel kommt.
    let r = reflect(-v, n);
    let f_ambient = f0 + (max(float3(1.0 - roughness), f0) - f0) * pow(1.0 - n_dot_v, 5.0);
    let glossy = lerp(hemisphere(r), hemisphere(n), roughness);
    color += (diffuse_color * hemisphere(n) + f_ambient * glossy) * occlusion;
    color += emissive;
    return float4(color, base.a);
}
