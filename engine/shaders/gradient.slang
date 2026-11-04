// Testmuster als Compute-Shader: schreibt einen Farbverlauf in eine Storage-Textur. Rot wächst
// nach rechts, Grün nach unten, Blau ist fest.

#include "bindless.slang"

struct PushConstants
{
    uint target;
    uint width;
    uint height;
    float blue;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.width || id.y >= push.height)
    {
        return;
    }
    let uv = (float2(id.xy) + 0.5) / float2(push.width, push.height);
    storage_texture(push.target)[id.xy] = float4(uv.x, uv.y, push.blue, 1.0);
}
