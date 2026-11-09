module;

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// Innenleben des Renderers (TD-36, R3.2): Renderer::Impl und die Daten der Draws, geteilt von
// renderer.cpp (Ablauf eines Frames), pipelines.cpp (Shader und Pipelines), passes.cpp (Schatten,
// Vorwärts, Tone Mapping, Auswahl) und terrain_pass.cpp (Terrain und seine Materialien). Eine
// interne Partition (wie :console in ngine.core): Nur die Implementierungen importieren sie, wer
// ngine.render importiert, sieht nichts davon. Als Schnittstellen-Partition brach MSVC beim Lesen
// des Moduls mit einem internen Fehler ab (C1001).
module ngine.render:renderer_detail;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import :debug_draw;
import :geometry;
import :graph;
import :materials;
import :meshes;
import :renderer;
import :shadows;
import :terrain;

namespace ngine::render::detail {

inline constexpr rhi::Format hdr_format = rhi::Format::RGBA16Float;
inline constexpr rhi::Format depth_format = rhi::Format::D32Float;
inline constexpr rhi::Format id_format = rhi::Format::R32Uint;

struct TonemapPush {
    u32 hdr = 0;
    f32 exposure = 1.0f;
    u32 encode_srgb = 0;
};

// Ein Teilnetz eines MeshRenderers im Vorwärts-Pass und im ID-Puffer.
struct Draw {
    std::array<math::Vec4, 3> model;
    const GpuMesh* mesh = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    usize pipeline = 0;
    // Untere 32 Bit der Entity für den ID-Puffer (World::entity_from_index()).
    u32 id = 0;
};

// Ein Teilnetz in einer Schattenkarte.
struct ShadowDraw {
    std::array<math::Vec4, 3> model;
    const GpuMesh* mesh = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    bool masked = false;
};

// Push Constants des ID-Puffers, wie id.slang sie liest.
struct IdPush {
    math::Vec4 model[3];
    u64 vertices = 0;
    u64 frame = 0;
    u32 id = 0;
    u32 reserved = 0;
};
static_assert(sizeof(IdPush) <= rhi::max_push_constant_size);

struct DebugPush {
    u64 lines = 0;
    u64 frame = 0;
    f32 viewport[2] = {};
    f32 width = 1.0f;
    u32 reserved = 0;
};

struct PickRequest {
    u32 x = 0;
    u32 y = 0;
};

// Ein Chunk eines Terrains, wie ihn Vorwärts-Pass, Schatten und ID-Puffer zeichnen.
struct TerrainDraw {
    std::array<math::Vec4, 3> model;
    // Ecke des Chunks relativ zum Terrain in Metern (xyz) und Meter pro Voxel (w), für die
    // Texturen (R3.2).
    math::Vec4 offset;
    u64 vertices = 0;
    const rhi::Buffer* index_buffer = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    u32 id = 0;
    // Materialpalette des Chunks, vier Materialien je Wort (voxel::Mesh::palette).
    std::array<u32, 2> palette{};
};

// Was die Passes eines Frames brauchen, festgelegt in Renderer::add_passes().
struct FrameInfo {
    rhi::Buffer* frame_buffer = nullptr;
    u64 frame_address = 0;
    bool shadows_on = false;
    u32 shadow_resolution = 0;
    rhi::Extent2D extent;
    u64 lines_address = 0;
    math::Vec3 sky_color;
    f32 exposure = 1.0f;
};

// Lader für ColorTexture und DataTexture (textures.cpp), aus register_loaders().
void register_texture_loaders(asset::ResourceManager& resources, Renderer& renderer);

} // namespace ngine::render::detail

namespace ngine::render {

struct Renderer::Impl {
    rhi::Device* device = nullptr;
    rhi::Format target_format = rhi::Format::Unknown;
    GeometryBuffer geometry;
    MeshStore meshes;
    TerrainMeshes terrain_meshes;
    MaterialStore materials;
    DebugDraw debug;
    std::unique_ptr<rhi::Shader> pbr_vertex;
    std::unique_ptr<rhi::Shader> pbr_fragment;
    std::unique_ptr<rhi::Shader> shadow_vertex;
    std::unique_ptr<rhi::Shader> shadow_fragment;
    std::unique_ptr<rhi::Shader> debug_vertex;
    std::unique_ptr<rhi::Shader> debug_fragment;
    std::unique_ptr<rhi::Shader> fullscreen_vertex;
    std::unique_ptr<rhi::Shader> tonemap_fragment;
    std::unique_ptr<rhi::Shader> id_vertex;
    std::unique_ptr<rhi::Shader> id_fragment;
    std::unique_ptr<rhi::Shader> terrain_vertex;
    std::unique_ptr<rhi::Shader> terrain_fragment;
    std::unique_ptr<rhi::Shader> terrain_shadow_vertex;
    std::unique_ptr<rhi::Shader> terrain_id_vertex;
    std::unique_ptr<rhi::Shader> terrain_id_fragment;
    // Terrain: Vorwärts, Schatten (neu erzeugt mit dem Tiefenversatz), ID-Puffer.
    std::unique_ptr<rhi::Pipeline> terrain;
    std::unique_ptr<rhi::Pipeline> terrain_shadow;
    std::unique_ptr<rhi::Pipeline> terrain_id;
    // ID-Puffer mit und ohne Aussortieren von Rückseiten (wie die PBR-Varianten).
    std::array<std::unique_ptr<rhi::Pipeline>, 2> id_pipelines;
    // Pro Frame im Flug: Puffer für das zurückgelesene Pixel und die Anfrage, die er beantwortet.
    std::vector<std::unique_ptr<rhi::Buffer>> pick_buffers;
    std::vector<std::optional<detail::PickRequest>> pick_pending;
    std::optional<detail::PickRequest> pick_requested;
    std::optional<detail::PickRequest> pick_answered;
    u32 pick_answer = 0;
    // Undurchsichtig, undurchsichtig doppelseitig, durchsichtig, durchsichtig doppelseitig.
    std::array<std::unique_ptr<rhi::Pipeline>, 4> pbr;
    // Schatten ohne und mit Alpha-Maske; neu erzeugt, wenn sich der Tiefenversatz ändert.
    std::unique_ptr<rhi::Pipeline> shadow_opaque;
    std::unique_ptr<rhi::Pipeline> shadow_masked;
    f32 shadow_bias_constant = 0.0f;
    f32 shadow_bias_slope = 0.0f;
    std::unique_ptr<rhi::Pipeline> debug_lines;
    std::unique_ptr<rhi::Pipeline> tonemap;
    std::unique_ptr<rhi::Sampler> shadow_sampler;
    // Je ein Puffer pro Frame im Flug, reihum benutzt: Frame-Daten, Lichter, Debug-Linien,
    // Materialien des Terrains.
    std::vector<std::unique_ptr<rhi::Buffer>> frames;
    std::vector<std::unique_ptr<rhi::Buffer>> lights;
    std::vector<std::unique_ptr<rhi::Buffer>> lines;
    std::vector<std::unique_ptr<rhi::Buffer>> terrain_material_buffers;
    usize next_frame = 0;
    std::vector<detail::Draw> draws;
    std::array<std::vector<detail::ShadowDraw>, cascade_count> shadow_draws;
    std::vector<detail::TerrainDraw> terrain_draws;
    // Indizes in terrain_draws je Kaskade.
    std::array<std::vector<u32>, cascade_count> terrain_shadow_draws;
    u32 line_count = 0;
    RenderStats stats;
    // Modelle, die MeshRenderer brauchen (R2.4). Ein Eintrag hält das Handle, solange ihn ein
    // Frame benutzt; danach gibt der Ressourcen-Manager das Modell nach einer Weile frei.
    struct ModelUse {
        asset::Handle<GpuModel> handle;
        u64 last_used = 0;
    };
    asset::ResourceManager* resources = nullptr;
    std::unordered_map<asset::AssetId, ModelUse> models;
    u64 frame_number = 0;

    // Materialien des Terrains (R3.2): Tabelle, Texturen aus dem Ressourcen-Manager und ihr
    // Sampler.
    std::vector<TerrainMaterial> terrain_materials = default_terrain_materials();
    std::unordered_map<asset::AssetId, asset::Handle<ColorTexture>> color_textures;
    std::unordered_map<asset::AssetId, asset::Handle<DataTexture>> data_textures;
    u32 terrain_sampler = rhi::invalid_index;

    explicit Impl(rhi::Device& device_)
        : device{&device_}, geometry{device_}, meshes{geometry}, terrain_meshes{geometry},
          materials{device_} {}

    // Modell zu einer Kennung, wenn geladen und hochgeladen; beginnt sonst das Laden.
    const GpuModel* model(asset::AssetId id);
    const GpuMesh* mesh(const asset::AssetRef& ref);
    // Material eines Verweises; nichts, wenn der Verweis leer ist oder das Modell fehlt.
    std::optional<u32> material(const asset::AssetRef& ref);

    // --- pipelines.cpp ---------------------------------------------------------------------

    // Shader des Renderers: Quelldatei, Einstieg und eingebettetes SPIR-V.
    struct ShaderSpec {
        std::string_view file;
        rhi::ShaderDesc (*embedded)();
        std::unique_ptr<rhi::Shader> Impl::*slot;
    };
    static std::span<const ShaderSpec> shader_specs();
    // Shader aus dem eingebetteten SPIR-V.
    rhi::Result<void> create_shaders();
    rhi::Result<void> reload_shaders(const ShaderSource& source);
    // Alle Pipelines aus den aktuellen Shadern.
    rhi::Result<void> create_pipelines();
    rhi::Result<void> create_shadow_pipelines(f32 constant, f32 slope);

    // --- renderer.cpp ----------------------------------------------------------------------

    // Puffer für Debug-Linien dieses Frames, mindestens count Linien groß.
    rhi::Result<rhi::Buffer*> line_buffer(usize slot, usize count);
    // Sammelt die Teilnetze aller MeshRenderer als Draws, undurchsichtige zuerst, dazu die Werfer
    // jeder Kaskade.
    void collect_meshes(world::World& world, math::DVec3 origin, bool shadows_on,
                        const std::array<Cascade, cascade_count>& cascades);

    // --- terrain_pass.cpp ------------------------------------------------------------------

    // Lädt geänderte Netze aller Terrains hoch und sammelt ihre Chunks als Draws, dazu die Werfer
    // jeder Kaskade.
    void collect_terrain(world::World& world, math::DVec3 origin, bool shadows_on,
                         const std::array<Cascade, cascade_count>& cascades);
    // Schreibt die Materialtabelle des Terrains in den Puffer dieses Frames; Texturen, die noch
    // laden, fehlen bis dahin (Farbe allein). Liefert die Adresse.
    u64 write_terrain_materials(usize slot);
    // Zeichnet Chunks des Terrains mit der gebundenen Pipeline.
    static void draw_terrain(rhi::CommandList& commands, std::span<const detail::TerrainDraw> draws,
                             u64 frame_address, u64 materials_address, u32 cascade);

    // --- passes.cpp ------------------------------------------------------------------------

    // Eine Schattenkarte pro Kaskade; liefert ihre Texturen.
    std::array<TextureHandle, cascade_count> add_shadow_passes(RenderGraph& graph,
                                                               const detail::FrameInfo& frame,
                                                               u64 terrain_material_address);
    // Vorwärts in ein HDR-Ziel: Terrain, Meshes, Debug-Linien. Liefert das HDR-Ziel.
    TextureHandle add_forward_pass(RenderGraph& graph, const detail::FrameInfo& frame,
                                   const std::array<TextureHandle, cascade_count>& shadow_maps,
                                   u64 terrain_material_address);
    void add_tonemap_pass(RenderGraph& graph, const detail::FrameInfo& frame, TextureHandle hdr,
                          TextureHandle target);
    // ID-Puffer mit allen Draws dieses Frames und Kopie eines Pixels in pick_buffers[slot].
    void add_pick_passes(RenderGraph& graph, usize slot, detail::PickRequest request,
                         const detail::FrameInfo& frame, u64 terrain_material_address);
};

} // namespace ngine::render
