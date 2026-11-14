module;

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.render:renderer;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import :components;
import :debug_draw;
import :geometry;
import :graph;
import :lights;
import :materials;
import :meshes;
import :shadows;
import :terrain;

export namespace ngine::render {

// Umgebungslicht aus Himmel- und Bodenfarbe, Ersatz für bildbasiertes Licht (R3). Sonne und
// andere Lichter sind seit R1.4 Komponenten der Welt (DirectionalLight, PointLight, SpotLight).
struct Lighting {
    math::Vec3 sky_color{0.32f, 0.42f, 0.58f};
    math::Vec3 ground_color{0.12f, 0.1f, 0.08f};
};

// Daten des Frames im GPU-Puffer, wie scene.slang sie liest.
struct GpuFrame {
    math::Vec4 view_projection[4];
    math::Vec4 sun_direction;
    math::Vec4 sun_color;
    math::Vec4 sky_color;
    math::Vec4 ground_color;
    math::Vec4 camera_forward;
    math::Vec4 cascade_far;
    math::Vec4 cascade_blend;
    math::Vec4 cascade_texel;
    math::Vec4 cascade_rows[cascade_count * 3];
    u32 shadow_maps[4] = {};
    // Zahl der Lichter, Schatten an, Kaskaden einfärben, Sampler der Schattenkarten.
    u32 options[4] = {};
    math::Vec4 shadow_params;
    u64 lights = 0;
    u64 reserved = 0;
};
static_assert(sizeof(GpuFrame) == 448);

// Kamera, wie sie der Renderer in einem Frame benutzt.
struct CameraView {
    math::DVec3 position;
    // Spalten: rechts, oben, nach hinten (die Kamera blickt entlang -z).
    math::Mat3 rotation;
    Camera camera;
};

// Erste aktive Kamera der Welt (in der Reihenfolge der Abfrage).
[[nodiscard]] std::optional<CameraView> find_camera(world::World& world);

// Matrix von kamerarelativen Weltkoordinaten in Clip-Koordinaten: umgekehrte Tiefe, unendlich
// ferner Horizont.
[[nodiscard]] math::Mat4 view_projection(const CameraView& view, f32 aspect);

// Ergebnis einer Auswahl per Klick (Renderer::request_pick()).
struct PickResult {
    u32 x = 0;
    u32 y = 0;
    // Entity der Welt unter dem Pixel, 0 wenn dort nichts gezeichnet wurde.
    u64 entity = 0;
};

struct RenderStats {
    // Netze (Instanzen), die der Vorwärts-Pass bekommt, und ihre Dreiecke, bevor die GPU prüft;
    // die Zahl der Draw-Aufrufe steht in forward_draws.
    u32 draws = 0;
    u32 triangles = 0;
    // Draw-Aufrufe in die Schattenkarten, über alle Kaskaden.
    u32 shadow_draws = 0;
    // Draw-Aufrufe der Vorwärts-Passes (beide Phasen und Durchsichtiges, ohne Debug-Linien).
    u32 forward_draws = 0;
    u32 lights = 0;
    u32 debug_lines = 0;
    // MeshRenderer, deren Modell noch lädt (oder nicht geladen werden konnte).
    u32 waiting = 0;
    // Gezeichnete Chunks aller Terrains (in draws und triangles mitgezählt) und Chunks, deren Netz
    // in diesem Frame hochgeladen wurde.
    u32 terrain_chunks = 0;
    u32 terrain_uploads = 0;
    // GPU-getriebenes Zeichnen (R3.5): Pfad mit Mesh Shadern, Instanzen im Instanzpuffer (auch
    // Schatten und durchsichtige) und Meshlets aller Gruppen der Kamera.
    bool mesh_shaders = false;
    u32 instances = 0;
    u32 meshlets = 0;
    // Was die GPU für die Kamera gezählt hat, einige Frames alt: gezeichnete Meshlets und ihre
    // Dreiecke, aussortiert am Stumpf, am Normalenkegel und durch Verdeckung (Hi-Z).
    u32 meshlets_drawn = 0;
    u32 triangles_drawn = 0;
    u32 culled_frustum = 0;
    u32 culled_backface = 0;
    u32 culled_occluded = 0;
};

// Liefert SPIR-V für einen Einstiegspunkt einer Shader-Datei (z. B. „pbr.slang“), für Hot Reload
// der Shader (PLAT-06). Der Renderer kennt keinen Shader-Compiler; die Anwendung reicht ihn
// hinein.
using ShaderSource = std::function<rhi::Result<std::vector<u32>>(
    std::string_view file, std::string_view entry, rhi::ShaderStage stage)>;

class GpuModel;

// Zeichnet die Welt (REND-07, REND-08, REND-09, REND-11, REND-15): von der ersten aktiven Kamera
// aus alle MeshRenderer und Terrains (seit R3.1, world::TerrainState), vorwärts mit PBR in ein
// HDR-Ziel, danach Tone Mapping in das Ziel.
// Vorher zeichnen vier Pässe die Schattenkarten der Sonne. Undurchsichtige Materialien kommen vor
// durchsichtigen; durchsichtige werden von hinten nach vorn sortiert und werfen keine Schatten.
// Seit R3.5 GPU-getrieben (REND-10, REND-13, ADR 0043): Jedes Netz ist eine Instanz aus Meshlets,
// die die GPU gegen Stumpf, Normalenkegel und Hi-Z prüft, mit Mesh Shadern oder indirekten Draws.
class Renderer {
public:
    // target_format: Format des Ziels, in das add_passes() zeichnet (meist der Backbuffer).
    [[nodiscard]] static rhi::Result<std::unique_ptr<Renderer>> create(rhi::Device& device,
                                                                       rhi::Format target_format);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] MeshStore& meshes() noexcept;
    // Gemeinsame Puffer für die Geometrie aller Meshes und Terrains (TD-23).
    [[nodiscard]] GeometryBuffer& geometry() noexcept;
    [[nodiscard]] MaterialStore& materials() noexcept;
    [[nodiscard]] rhi::Device& device() noexcept;

    // Ressourcen-Manager, aus dem der Renderer die Modelle der MeshRenderer lädt (R2.4). Ohne ihn
    // zeichnet er keine Meshes. Muss den Renderer überleben oder vorher zurückgesetzt werden.
    void set_resources(asset::ResourceManager* resources) noexcept;
    [[nodiscard]] asset::ResourceManager* resources() noexcept;
    // Mesh, auf das ref zeigt, wenn sein Modell geladen ist; beginnt sonst das Laden.
    [[nodiscard]] const GpuMesh* find_mesh(const asset::AssetRef& ref);
    // Modell zu einer Kennung, wenn geladen; beginnt sonst das Laden.
    [[nodiscard]] const GpuModel* find_model(asset::AssetId id);

    // Materialtabelle des Terrains (R3.2), Index = Materialnummer der Voxel. Anfangs
    // default_terrain_materials() ohne Texturen; Texturen lädt der Renderer über den
    // Ressourcen-Manager, sobald ein Terrain gezeichnet wird.
    void set_terrain_materials(std::vector<TerrainMaterial> materials);
    [[nodiscard]] const std::vector<TerrainMaterial>& terrain_materials() const noexcept;

    // Übersetzt die Shader des Renderers neu aus ihren Quellen und baut die Pipelines neu (Hot
    // Reload). Scheitert ein Shader, bleibt alles beim Alten.
    rhi::Result<void> reload_shaders(const ShaderSource& source);

    Lighting lighting;
    ShadowSettings shadows;
    // Färbt die Kaskaden der Schatten zur Prüfung ein.
    bool show_cascades = false;
    // Mesh Shader nutzen, wenn die GPU sie kann; sonst indirekte Draws (Rückfallpfad).
    bool use_mesh_shaders = true;
    // Verdeckte Meshlets über das Hi-Z aussortieren (zwei Phasen im Vorwärts-Pass).
    bool occlusion_culling = true;
    // Kamera statt der ersten aktiven Kamera der Welt, z. B. die Kamera des Editors, die nicht
    // zur Szene gehört (R2.2).
    std::optional<CameraView> camera_override;

    // Linien für diesen Frame; add_passes() zeichnet und leert sie.
    [[nodiscard]] DebugDraw& debug() noexcept;

    // Fügt Vorwärts-Pass und Tone Mapping hinzu. target muss das Format aus create() haben und
    // endet im Zustand ColorTarget, den Rest regelt der Graph. WorldTransform muss aktuell sein
    // (world::update_transforms()). Ohne aktive Kamera zeigt das Bild nur den Himmel.
    void add_passes(RenderGraph& graph, world::World& world, TextureHandle target);

    [[nodiscard]] const RenderStats& stats() const noexcept;

    // Auswahl per Klick (R2.2): Das nächste add_passes() zeichnet zusätzlich einen ID-Puffer
    // (Entity pro Pixel in R32Uint) und liest das Pixel (x, y) des Ziels zurück (oben links 0, 0).
    // Eine neue Anfrage ersetzt eine, die noch nicht gezeichnet ist.
    void request_pick(u32 x, u32 y);
    // Ergebnis, sobald die GPU fertig ist, also einige add_passes() später; sonst nichts. world
    // muss die Welt sein, die gezeichnet wurde.
    [[nodiscard]] std::optional<PickResult> take_pick(const world::World& world);

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Eine Textur im MaterialStore des Renderers, geladen aus einer Bilddatei über den
// Ressourcen-Manager (R3.2). Gibt sie beim Zerstören frei; der Renderer muss sie überleben.
class GpuTexture {
public:
    GpuTexture(Renderer& renderer, u32 texture, rhi::UploadTicket ticket = {});
    ~GpuTexture();
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    // Bindloser Index der Textur.
    [[nodiscard]] u32 index() const noexcept {
        return texture_;
    }
    // Uploads im Hintergrund; benutzt werden darf die Textur erst, wenn das Ticket erledigt ist.
    [[nodiscard]] rhi::UploadTicket ticket() const noexcept {
        return ticket_;
    }

private:
    void release() noexcept;

    Renderer* renderer_ = nullptr;
    u32 texture_ = rhi::invalid_index;
    rhi::UploadTicket ticket_;
};

// Bild als Textur im sRGB-Format (Grundfarben).
class ColorTexture : public GpuTexture {
public:
    using GpuTexture::GpuTexture;
};

// Bild als lineare Textur (Normalen, Masken).
class DataTexture : public GpuTexture {
public:
    using GpuTexture::GpuTexture;
};

// Meshes, Materialien und Texturen eines Modells im Renderer: Index im MeshStore pro asset::Mesh,
// Index im MaterialStore pro asset::Material, bindlose Indizes der hochgeladenen Bilder.
struct ModelInstance {
    std::vector<u32> meshes;
    std::vector<u32> materials;
    std::vector<u32> textures;
};

// Ein Modell auf der GPU, wie der Ressourcen-Manager es hält (ASSET-04): seine Meshes,
// Materialien und Texturen in den Speichern des Renderers und sein Aufbau (Knoten, Namen,
// Materialien) ohne Eckpunkte und Pixel. Gibt beim Zerstören alles wieder frei; der Renderer muss
// es überleben.
class GpuModel {
public:
    GpuModel(Renderer& renderer, ModelInstance instance, asset::Model layout,
             rhi::UploadTicket ticket = {});
    ~GpuModel();
    GpuModel(GpuModel&& other) noexcept;
    GpuModel& operator=(GpuModel&& other) noexcept;
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    [[nodiscard]] const ModelInstance& instance() const noexcept {
        return instance_;
    }
    // Knoten, Meshes (ohne Eckpunkte), Materialien; Bilder ohne Pixel.
    [[nodiscard]] const asset::Model& layout() const noexcept {
        return layout_;
    }
    // Uploads im Hintergrund (TD-17); der Renderer zeichnet das Modell erst, wenn es erledigt ist.
    [[nodiscard]] rhi::UploadTicket ticket() const noexcept {
        return ticket_;
    }

private:
    void release() noexcept;

    Renderer* renderer_ = nullptr;
    ModelInstance instance_;
    asset::Model layout_;
    rhi::UploadTicket ticket_;
};

// Fertige Texturen eines Modells je Bild und Farbraum: Mip-Ketten in RGBA8 (prepare_textures())
// oder aus KTX2 umgewandelt (gekochte Modelle, R2.5), auch in BC7.
struct ModelTextures {
    std::map<std::pair<u32, bool>, asset::TextureData> textures;
};

// Rechnet die Mip-Ketten aller Texturen, die Materialien des Modells benutzen, in RGBA8. Braucht
// keine GPU und läuft deshalb im Hintergrund beim Laden (TD-17).
[[nodiscard]] ModelTextures prepare_textures(const asset::Model& model);

// Texturen eines gekochten Modells in target (im Hintergrund): BC7 aus der fertigen Fassung, sonst
// aus der KTX2-Datei umgewandelt. Nimmt die Daten aus cooked.
[[nodiscard]] asset::Result<ModelTextures> decode_textures(asset::CookedModel& cooked,
                                                           asset::TextureFormat target);

// Lädt Bilder, Materialien und Meshes eines Modells hoch. Bilder, die nur als Grundfarbe oder
// Emission dienen, werden sRGB, alle anderen linear. Mit textures laufen die Uploads im
// Hintergrund (GpuModel::ticket()); fehlt eine Textur darin, rechnet die GPU die Stufen aus den
// Pixeln des Modells wie ohne textures.
[[nodiscard]] rhi::Result<GpuModel> upload_model(Renderer& renderer, const asset::Model& model,
                                                 const ModelTextures* textures = nullptr);

// Woher der Lader für glTF-Modelle liest (R2.5, ADR 0036).
struct ModelLoading {
    // Gekochte Modelle aus diesem Cache, wenn sie aktuell sind. nullptr: immer aus den Quellen.
    const asset::AssetCache* cache = nullptr;
    // Kocht veraltete Modelle und legt sie in den Cache (Editor). Ohne kommt ein veraltetes Modell
    // aus der Quelle, mit einfachen Tangenten und Mip-Stufen auf der CPU (Spiel, Server).
    asset::ModelCooker cook;
    // Kocht Einzeltexturen (z. B. des Terrains) zu KTX2 und legt sie in den Cache (R4.3). Ohne
    // kommt eine veraltete Textur aus der Bilddatei mit Mip-Stufen auf der CPU.
    asset::ImageCooker cook_image;
};

// Meldet beim Ressourcen-Manager, wie GpuModel aus glTF-Dateien entstehen: lesen (aus dem Cache,
// gekocht oder aus der Quelle) im Hintergrund, hochladen in update(). Ein neu geladenes Modell
// ersetzt das alte erst, wenn seine Uploads fertig sind (TD-30). Seit R3.2 auch ColorTexture und
// DataTexture aus Bilddateien (Mip-Stufen im Hintergrund, Upload in update()). renderer und
// loading.cache müssen den Manager überleben.
void register_loaders(asset::ResourceManager& resources, Renderer& renderer,
                      ModelLoading loading = {});

// Wie oft der Lader aus welcher Quelle gelesen hat, seit dem Start (Messung, Tests).
struct ModelLoadCounts {
    u32 cached = 0;
    u32 cooked = 0;
    u32 source = 0;
};
[[nodiscard]] ModelLoadCounts model_load_counts() noexcept;

// Legt für die Knoten der Szene Entities an (Name, Transform, MeshRenderer mit Verweisen auf das
// Modell model_id, Hierarchie) unter einer neuen Wurzel-Entity und liefert sie.
world::Entity spawn_model(world::World& world, const asset::Model& layout, asset::AssetId model_id,
                          std::string_view name);

} // namespace ngine::render
