// Der bindlose Descriptor-Satz, den alle Pipelines teilen (ADR 0026). Shader erreichen Texturen
// und Sampler über Indizes, die die RHI vergibt (Texture::sampled_index(), storage_index(),
// Sampler::index()); die Indizes kommen meist über Push Constants oder Puffer.
//
// Satz 0, Binding 0: Texturen zum Lesen (Zustand ShaderRead)
//         Binding 1: Sampler
//         Binding 2: Storage-Texturen (Zustand ShaderWrite)

[[vk::binding(0, 0)]]
Texture2D<float4> bindless_textures[];

[[vk::binding(1, 0)]]
SamplerState bindless_samplers[];

[[vk::binding(2, 0)]]
RWTexture2D<float4> bindless_storage[];

// Ein Index kann innerhalb eines Draws von Pixel zu Pixel wechseln; NonUniformResourceIndex sagt
// das dem Compiler, damit es auf jeder GPU richtig gelesen wird.
float4 sample_texture(uint texture, uint sampler, float2 uv)
{
    return bindless_textures[NonUniformResourceIndex(texture)].Sample(
        bindless_samplers[NonUniformResourceIndex(sampler)], uv);
}

RWTexture2D<float4> storage_texture(uint index)
{
    return bindless_storage[NonUniformResourceIndex(index)];
}
