// Skinning auf der GPU (R4.6, ANIM-01, ADR 0050): verformt die Eckpunkte eines Netzes mit einer
// Palette aus 3×4-Matrizen und schreibt sie in einen eigenen Bereich des Geometriepuffers. Danach
// ist das Netz für Culling, Schatten, Vorwärts-Pass und ID-Puffer ein gewöhnliches Netz.
// Lineares Skinning mit vier Einflüssen.

#include "scene.slang"

// Wie asset::SkinVertex: vier Gelenke (u16) und vier Gewichte (normierte u16), 16 Bytes.
struct SkinVertex
{
    uint joints01;
    uint joints23;
    uint weights01;
    uint weights23;
};

// Wie animation::JointMatrix: Zeilen einer affinen 3×4-Matrix.
struct JointMatrix
{
    float4 rows[3];
};

// Wie render::detail::SkinningPush.
struct SkinningPush
{
    Vertex* source;
    SkinVertex* skin;
    Vertex* target;
    JointMatrix* palette;
    uint vertex_count;
    uint joint_count;
    uint2 reserved;
};

[[vk::push_constant]]
ConstantBuffer<SkinningPush> push;

// transform_point() kommt aus scene.slang.
float3 skin_direction(float4 rows[3], float3 d)
{
    return float3(dot(rows[0].xyz, d), dot(rows[1].xyz, d), dot(rows[2].xyz, d));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 id : SV_DispatchThreadID)
{
    let index = id.x;
    if (index >= push.vertex_count)
    {
        return;
    }
    let vertex = push.source[index];
    let skin = push.skin[index];
    let joints = uint4(skin.joints01 & 0xFFFF, skin.joints01 >> 16, skin.joints23 & 0xFFFF,
                       skin.joints23 >> 16);
    let weights = float4(float(skin.weights01 & 0xFFFF), float(skin.weights01 >> 16),
                         float(skin.weights23 & 0xFFFF), float(skin.weights23 >> 16)) /
                  65535.0;
    // Gewichtete Summe der Matrizen, dann einmal anwenden.
    float4 rows[3] = { float4(0.0), float4(0.0), float4(0.0) };
    for (uint i = 0; i < 4; ++i)
    {
        if (weights[i] > 0.0 && joints[i] < push.joint_count)
        {
            let m = push.palette[joints[i]];
            rows[0] += m.rows[0] * weights[i];
            rows[1] += m.rows[1] * weights[i];
            rows[2] += m.rows[2] * weights[i];
        }
    }
    let position = transform_point(rows, float3(vertex.px, vertex.py, vertex.pz));
    // Normalen mit der linearen Matrix: genug bei gleichmäßiger Skalierung der Gelenke.
    let normal = normalize(skin_direction(rows, float3(vertex.nx, vertex.ny, vertex.nz)));
    let tangent = normalize(skin_direction(rows, float3(vertex.tx, vertex.ty, vertex.tz)));
    Vertex result = vertex;
    result.px = position.x;
    result.py = position.y;
    result.pz = position.z;
    result.nx = normal.x;
    result.ny = normal.y;
    result.nz = normal.z;
    result.tx = tangent.x;
    result.ty = tangent.y;
    result.tz = tangent.z;
    push.target[index] = result;
}
