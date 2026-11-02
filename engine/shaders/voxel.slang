// Voxel-Terrain für den Durchstich D1. Die Geometrie kommt über Vertex Pulling: Der Shader liest
// die Eckpunkte selbst über die Adresse des Puffers (ADR 0022), es gibt keine Vertex-Attribute.

struct Vertex
{
    float3 position;
    uint material;
    float3 normal;
    float reserved;
};

struct PushConstants
{
    // Zeilen der Matrix von Gitter- in Clip-Koordinaten.
    float4 view_projection[4];
    // xyz: Richtung zum Licht (normiert), w: Anteil des Umgebungslichts.
    float4 light;
    // xyz: Kamera in Gitterkoordinaten, w: Dichte des Nebels.
    float4 camera;
    Vertex* vertices;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 world : POSITION0;
    float3 normal : NORMAL0;
    // Farbe pro Eckpunkt, interpoliert: So verlaufen Materialgrenzen weich über das Dreieck,
    // statt an Dreieckskanten zu springen. Ersetzt in R3 durch Triplanar-Texturen (VOX-07).
    float3 albedo : COLOR0;
};

float3 material_color(uint material, float3 normal)
{
    let grass = float3(0.20, 0.42, 0.10);
    let rock = float3(0.32, 0.30, 0.28);
    let earth = float3(0.36, 0.22, 0.12);
    switch (material)
    {
    case 1:
        // Gras rutscht an steilen Stellen ab, darunter kommt Fels zum Vorschein.
        return lerp(rock, grass, smoothstep(0.55, 0.8, normal.y));
    case 2:
        return rock;
    case 3:
        return earth;
    default:
        return float3(1.0, 0.0, 1.0);
    }
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id : SV_VertexID)
{
    let v = push.vertices[vertex_id];
    let p = float4(v.position, 1.0);
    VertexOutput output;
    output.position = float4(dot(push.view_projection[0], p), dot(push.view_projection[1], p),
                             dot(push.view_projection[2], p), dot(push.view_projection[3], p));
    output.world = v.position;
    output.normal = v.normal;
    output.albedo = material_color(v.material, v.normal);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    let n = normalize(input.normal);
    let albedo = input.albedo;
    let diffuse = max(dot(n, push.light.xyz), 0.0);
    // Halbkugel-Licht: oben heller Himmel, unten dunkler Boden.
    let ambient = push.light.w * lerp(0.5, 1.0, n.y * 0.5 + 0.5);
    var color = albedo * (diffuse + ambient);

    let sky = float3(0.45, 0.60, 0.80);
    let distance = length(input.world - push.camera.xyz);
    let fog = 1.0 - exp(-distance * push.camera.w);
    color = lerp(color, sky, saturate(fog));
    return float4(color, 1.0);
}
