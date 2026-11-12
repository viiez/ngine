module;

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// Innenleben des Renderers (TD-36, R3.2): RendererCore (Zustand hinter Renderer::Impl) und die
// Daten der Draws, geteilt von renderer.cpp (Ablauf eines Frames), pipelines.cpp (Shader und
// Pipelines), passes.cpp (Schatten, Vorwärts, Tone Mapping, Auswahl), terrain_pass.cpp (Terrain
// und seine Materialien) und driven.cpp (Instanzen, Gruppen, Prüfen der Meshlets, Hi-Z; R3.5). Eine
// interne Partition (wie :console in ngine.core): Nur die Implementierungen importieren sie, wer
// ngine.render importiert, sieht nichts davon. Renderer::Impl selbst wird erst in renderer.cpp
// vervollständigt (MSVC, siehe dort).
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
import :culling;
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

// --- GPU-getriebenes Zeichnen (R3.5, ADR 0043) -----------------------------------------------

// Kategorien der Gruppen von Meshlets. Jede hat eigene Pipelines: Vorwärts, Schatten, ID-Puffer.
// Durchsichtige Netze gehören zu keiner; sie zeichnet die CPU sortiert (BlendDraw).
enum class Category : u8 { Terrain, Opaque, Masked, OpaqueDoubleSided, MaskedDoubleSided };
inline constexpr u32 category_count = 5;

// Push Constants aller Stufen des GPU-getriebenen Zeichnens, wie driven.slang sie liest.
struct DrivenPush {
    u64 frame = 0;
    u64 instances = 0;
    u64 groups = 0;
    u64 view = 0;
    u64 materials = 0;
    u64 terrain_materials = 0;
    u64 commands = 0;
    u64 counters = 0;
    u64 stats = 0;
    u32 first_group = 0;
    u32 command_base = 0;
    u32 counter_base = 0;
    // Nur Schatten: die Kaskade, in die gezeichnet wird.
    u32 cascade = 0;
    u32 reserved[4] = {};
};
static_assert(sizeof(DrivenPush) == 104);
static_assert(sizeof(DrivenPush) <= rhi::max_push_constant_size);

struct HizPush {
    u32 source = 0;
    u32 target = 0;
    u32 source_size[2] = {};
    u32 target_size[2] = {};
    u32 reserved[2] = {};
};

// Wer Meshlets prüft und zeichnet: jede Aufgabe mit eigener Sicht, eigenen Befehlen und Zählern.
enum class Job : u8 { Forward, ForwardLate, Shadow0, Shadow1, Shadow2, Shadow3, Pick };
inline constexpr u32 job_count = 7;

// Blöcke der Statistik (je stat_words Zähler): Kamera, Schatten (alle Kaskaden), ID-Puffer. Die
// Zähler eines Blocks wie in cull.slang: geprüft, Stumpf, Kegel, verdeckt, gezeichnet, Dreiecke.
inline constexpr u32 stat_block_count = 3;
inline constexpr u32 stat_words = 8;
inline constexpr u64 stat_bytes = u64{stat_block_count} * stat_words * sizeof(u32);

// Gruppen einer Kategorie, deren Indizes in einem Block der Geometrie liegen: im Rückfallpfad ein
// indirekter Draw mit Zähler, Befehle ab command_start.
struct Region {
    Category category = Category::Terrain;
    u32 block = 0;
    const rhi::Buffer* index_buffer = nullptr;
    u32 command_start = 0;
    u32 max_commands = 0;
};

// Gruppen einer Kategorie (im Mesh-Shader-Pfad ein Aufruf von draw_mesh_tasks()).
struct GroupRange {
    u32 first = 0;
    u32 count = 0;
};

// Ein durchsichtiges Teilnetz, gezeichnet von der CPU mit seiner Instanz.
struct BlendDraw {
    u32 instance = 0;
    const rhi::Buffer* index_buffer = nullptr;
    u32 first_index = 0;
    u32 index_count = 0;
    usize pipeline = 0;
    f32 distance = 0.0f;
};

// Pipelines je Kategorie für eine Art von Pass.
using CategoryPipelines = std::array<const rhi::Pipeline*, category_count>;

// Puffer eines Frame-Platzes für das GPU-getriebene Zeichnen.
struct DrivenBuffers {
    std::unique_ptr<rhi::Buffer> instances;
    std::unique_ptr<rhi::Buffer> groups;
    std::unique_ptr<rhi::Buffer> views;
    // GPU: Befehle des Rückfallpfads; Statistik und Zähler der Befehle.
    std::unique_ptr<rhi::Buffer> commands;
    std::unique_ptr<rhi::Buffer> counters;
    // CPU: Kopie der Statistik.
    std::unique_ptr<rhi::Buffer> readback;
    bool readback_pending = false;
};

// Eine Tiefenpyramide (Hi-Z): je Stufe eine Textur.
struct HizSet {
    std::vector<std::unique_ptr<rhi::Texture>> levels;
    // Inhalt gültig (die Texturen sind im Zustand ShaderRead).
    bool valid = false;
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
    u64 terrain_materials = 0;
    // Puffer des GPU-getriebenen Zeichnens in diesem Frame.
    DrivenBuffers* driven = nullptr;
    bool mesh_shaders = false;
    // Hi-Z: dieser Frame baut hiz[hiz_current], phase zwei läuft.
    bool occlusion = false;
    u32 hiz_current = 0;
    rhi::Extent2D hiz_extent;
    u32 hiz_levels = 0;
};

// Lader für ColorTexture und DataTexture (textures.cpp), aus register_loaders().
void register_texture_loaders(asset::ResourceManager& resources, Renderer& renderer);

} // namespace ngine::render::detail

namespace ngine::render {

struct RendererCore {
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
    std::unique_ptr<rhi::Shader> cull_compute;
    std::unique_ptr<rhi::Shader> hiz_compute;
    // Nur mit der Stufe Mesh Shader (R3.5).
    std::unique_ptr<rhi::Shader> cull_task;
    std::unique_ptr<rhi::Shader> pbr_mesh;
    std::unique_ptr<rhi::Shader> shadow_mesh;
    std::unique_ptr<rhi::Shader> id_mesh;
    std::unique_ptr<rhi::Shader> terrain_mesh;
    std::unique_ptr<rhi::Shader> terrain_shadow_mesh;
    std::unique_ptr<rhi::Shader> terrain_id_mesh;
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
    std::unique_ptr<rhi::Pipeline> cull;
    std::unique_ptr<rhi::Pipeline> hiz;
    // Mesh-Shader-Pfad: dieselben Pipelines mit Task- und Mesh-Shader statt Vertex-Shader.
    std::array<std::unique_ptr<rhi::Pipeline>, 2> pbr_meshlets;
    std::unique_ptr<rhi::Pipeline> shadow_opaque_meshlets;
    std::unique_ptr<rhi::Pipeline> shadow_masked_meshlets;
    std::array<std::unique_ptr<rhi::Pipeline>, 2> id_meshlets;
    std::unique_ptr<rhi::Pipeline> terrain_meshlets;
    std::unique_ptr<rhi::Pipeline> terrain_shadow_meshlets;
    std::unique_ptr<rhi::Pipeline> terrain_id_meshlets;
    std::unique_ptr<rhi::Sampler> shadow_sampler;
    // Je ein Puffer pro Frame im Flug, reihum benutzt: Frame-Daten, Lichter, Debug-Linien,
    // Materialien des Terrains, Puffer des GPU-getriebenen Zeichnens.
    std::vector<std::unique_ptr<rhi::Buffer>> frames;
    std::vector<std::unique_ptr<rhi::Buffer>> lights;
    std::vector<std::unique_ptr<rhi::Buffer>> lines;
    std::vector<std::unique_ptr<rhi::Buffer>> terrain_material_buffers;
    std::vector<detail::DrivenBuffers> driven;
    usize next_frame = 0;

    // Instanzen und Gruppen dieses Frames (R3.5).
    std::vector<GpuInstance> instances;
    // Je Instanz: Kategorie und Block der Indizes; für durchsichtige Instanzen ungenutzt.
    struct InstanceKey {
        detail::Category category = detail::Category::Terrain;
        u32 block = 0;
        const rhi::Buffer* index_buffer = nullptr;
        bool blend = false;
    };
    std::vector<InstanceKey> instance_keys;
    std::vector<std::array<u32, 4>> groups;
    std::vector<detail::Region> regions;
    std::array<detail::GroupRange, detail::category_count> category_groups{};
    // Befehle je Aufgabe im Rückfallpfad (Summe der max_commands aller Bereiche).
    u32 commands_per_job = 0;
    std::vector<detail::BlendDraw> blend_draws;
    // Hi-Z im Wechsel: eines baut dieser Frame, das andere stammt vom letzten.
    std::array<detail::HizSet, 2> hiz_sets;
    rhi::Extent2D hiz_size;
    u32 hiz_next = 0;
    // Matrix und Ursprung des letzten Frames, dessen Hi-Z gültig ist.
    math::Mat4 previous_view_projection;
    math::DVec3 previous_origin;
    bool previous_valid = false;
    // Zuletzt von der GPU gelesene Statistik der Kamera (einige Frames alt).
    std::array<u32, detail::stat_words> gpu_stats{};
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

    explicit RendererCore(rhi::Device& device_)
        : device{&device_}, geometry{device_}, meshes{geometry}, terrain_meshes{geometry},
          materials{device_} {}

    // Modell zu einer Kennung, wenn geladen und hochgeladen; beginnt sonst das Laden.
    const GpuModel* model(asset::AssetId id);
    const GpuMesh* mesh(const asset::AssetRef& ref);
    // Material eines Verweises; nichts, wenn der Verweis leer ist oder das Modell fehlt.
    std::optional<u32> material(const asset::AssetRef& ref);

    // --- pipelines.cpp ---------------------------------------------------------------------

    // Shader des Renderers: Quelldatei, Einstieg und eingebettetes SPIR-V; mesh: nur mit der Stufe
    // Mesh Shader.
    struct ShaderSpec {
        std::string_view file;
        rhi::ShaderDesc (*embedded)();
        std::unique_ptr<rhi::Shader> RendererCore::*slot;
        bool mesh = false;
    };
    static std::span<const ShaderSpec> shader_specs();
    // Die GPU kann Mesh Shader (Capabilities::features).
    [[nodiscard]] bool mesh_capable() const;
    // Shader aus dem eingebetteten SPIR-V.
    rhi::Result<void> create_shaders();
    rhi::Result<void> reload_shaders(const ShaderSource& source);
    // Alle Pipelines aus den aktuellen Shadern.
    rhi::Result<void> create_pipelines();
    rhi::Result<void> create_shadow_pipelines(f32 constant, f32 slope);
    // Pipelines je Kategorie für Vorwärts, Schatten und ID-Puffer.
    [[nodiscard]] detail::CategoryPipelines forward_pipelines(bool mesh_shaders) const;
    [[nodiscard]] detail::CategoryPipelines shadow_pipelines(bool mesh_shaders) const;
    [[nodiscard]] detail::CategoryPipelines pick_pipelines(bool mesh_shaders) const;

    // --- renderer.cpp ----------------------------------------------------------------------

    // Puffer für Debug-Linien dieses Frames, mindestens count Linien groß.
    rhi::Result<rhi::Buffer*> line_buffer(usize slot, usize count);
    // Sammelt die Teilnetze aller MeshRenderer als Instanzen.
    void collect_meshes(world::World& world, math::DVec3 origin);

    // --- terrain_pass.cpp ------------------------------------------------------------------

    // Lädt geänderte Netze aller Terrains hoch und sammelt ihre Chunks als Instanzen.
    void collect_terrain(world::World& world, math::DVec3 origin);
    // Schreibt die Materialtabelle des Terrains in den Puffer dieses Frames; Texturen, die noch
    // laden, fehlen bis dahin (Farbe allein). Liefert die Adresse.
    u64 write_terrain_materials(usize slot, bool any_terrain);

    // --- driven.cpp ------------------------------------------------------------------------

    // Bildet aus den Instanzen Gruppen zu je 32 Meshlets, sortiert nach Kategorie und Block,
    // dazu die Bereiche der Befehle; sortiert durchsichtige Teilnetze von hinten nach vorn.
    void build_groups();
    // Schreibt Instanzen, Gruppen und Sichten in die Puffer des Platzes und legt die Puffer für
    // Befehle und Zähler an. Liefert false, wenn das nicht ging (dann wird nichts gezeichnet).
    bool write_driven(usize slot, std::span<const GpuView, detail::job_count> views,
                      bool mesh_shaders);
    // Liest die Statistik des Platzes, wenn die GPU sie geschrieben hat.
    void read_driven_stats(usize slot);
    // Hi-Z-Sätze für die Größe dieses Frames (neu, wenn sie sich ändert).
    bool ensure_hiz(rhi::Extent2D extent);
    // Push Constants einer Aufgabe.
    [[nodiscard]] detail::DrivenPush driven_push(const detail::FrameInfo& frame, detail::Job job,
                                                 u32 cascade) const;
    // Graph: Zähler leeren, Prüfen im Rückfallpfad (Compute), Hi-Z, Statistik lesen.
    void add_clear_pass(RenderGraph& graph, const detail::FrameInfo& frame, BufferHandle counters);
    void add_cull_pass(RenderGraph& graph, std::string_view name, const detail::FrameInfo& frame,
                       std::span<const detail::Job> jobs, BufferHandle commands,
                       BufferHandle counters, std::span<const TextureHandle> hiz_reads);
    void add_hiz_passes(RenderGraph& graph, const detail::FrameInfo& frame, TextureHandle depth,
                        std::span<const TextureHandle> levels);
    void add_stats_pass(RenderGraph& graph, const detail::FrameInfo& frame, BufferHandle counters);
    // Zeichnet die Gruppen aller Kategorien für eine Aufgabe: im Mesh-Shader-Pfad mit
    // draw_mesh_tasks(), sonst mit draw_indexed_indirect_count() je Bereich.
    void draw_job(rhi::CommandList& commands, const detail::FrameInfo& frame, detail::Job job,
                  const detail::CategoryPipelines& pipelines, u32 cascade,
                  const rhi::Buffer* command_buffer, const rhi::Buffer* counter_buffer);
    // Wie viele Draw-Aufrufe draw_job() macht.
    [[nodiscard]] u32 job_draw_calls(bool mesh_shaders) const;

    // --- passes.cpp ------------------------------------------------------------------------

    // Handles der Puffer des GPU-getriebenen Zeichnens im Graph dieses Frames.
    struct DrivenHandles {
        BufferHandle commands;
        BufferHandle counters;
        std::vector<TextureHandle> hiz_previous;
        std::vector<TextureHandle> hiz_current;
    };
    // Eine Schattenkarte pro Kaskade; liefert ihre Texturen.
    std::array<TextureHandle, cascade_count> add_shadow_passes(RenderGraph& graph,
                                                               const detail::FrameInfo& frame,
                                                               const DrivenHandles& handles);
    // Vorwärts in ein HDR-Ziel in zwei Phasen mit Hi-Z dazwischen: Terrain, Meshes, durchsichtige
    // Meshes, Debug-Linien. Liefert das HDR-Ziel.
    TextureHandle add_forward_passes(RenderGraph& graph, const detail::FrameInfo& frame,
                                     const std::array<TextureHandle, cascade_count>& shadow_maps,
                                     const DrivenHandles& handles);
    void add_tonemap_pass(RenderGraph& graph, const detail::FrameInfo& frame, TextureHandle hdr,
                          TextureHandle target);
    // ID-Puffer mit allen Instanzen dieses Frames und Kopie eines Pixels in pick_buffers[slot].
    void add_pick_passes(RenderGraph& graph, usize slot, detail::PickRequest request,
                         const detail::FrameInfo& frame, const DrivenHandles& handles);
};

} // namespace ngine::render
