module;

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

export module ngine.render:renderer;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.world;
import :components;
import :graph;
import :materials;
import :meshes;

export namespace ngine::render {

// Licht in R1.3: eine Sonne und ein Umgebungslicht aus Himmel- und Bodenfarbe. Punktlichter und
// Schatten folgen in R1.4, bildbasiertes Licht mit dem Himmel in R3.
struct Lighting {
    // Richtung zur Sonne in Weltkoordinaten (wird normiert).
    math::Vec3 sun_direction{0.4f, 0.8f, 0.45f};
    math::Vec3 sun_color{1.0f, 0.96f, 0.9f};
    f32 sun_intensity = 3.0f;
    math::Vec3 sky_color{0.32f, 0.42f, 0.58f};
    math::Vec3 ground_color{0.12f, 0.1f, 0.08f};
};

// Daten des Frames im GPU-Puffer, wie pbr.slang sie liest.
struct GpuFrame {
    math::Vec4 view_projection[4];
    math::Vec4 sun_direction;
    math::Vec4 sun_color;
    math::Vec4 sky_color;
    math::Vec4 ground_color;
};
static_assert(sizeof(GpuFrame) == 128);

// Push Constants eines Draws, wie pbr.slang sie liest.
struct DrawPush {
    math::Vec4 model[3];
    u64 vertices = 0;
    u64 frame = 0;
    u64 materials = 0;
    u32 material = 0;
    u32 reserved = 0;
};
static_assert(sizeof(DrawPush) <= rhi::max_push_constant_size);

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

struct RenderStats {
    u32 draws = 0;
    u32 triangles = 0;
};

// Zeichnet die Welt (REND-07, REND-08, REND-11): von der ersten aktiven Kamera aus alle
// MeshRenderer, vorwärts mit PBR in ein HDR-Ziel, danach Tone Mapping in das Ziel. Undurchsichtige
// Materialien kommen vor durchsichtigen; durchsichtige werden nicht sortiert (TD-20).
class Renderer {
public:
    // target_format: Format des Ziels, in das add_passes() zeichnet (meist der Backbuffer).
    [[nodiscard]] static rhi::Result<std::unique_ptr<Renderer>> create(rhi::Device& device,
                                                                       rhi::Format target_format);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] MeshStore& meshes() noexcept;
    [[nodiscard]] MaterialStore& materials() noexcept;

    Lighting lighting;

    // Fügt Vorwärts-Pass und Tone Mapping hinzu. target muss das Format aus create() haben und
    // endet im Zustand ColorTarget, den Rest regelt der Graph. WorldTransform muss aktuell sein
    // (world::update_transforms()). Ohne aktive Kamera zeigt das Bild nur den Himmel.
    void add_passes(RenderGraph& graph, world::World& world, TextureHandle target);

    [[nodiscard]] const RenderStats& stats() const noexcept;

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Meshes und Materialien eines Modells im Renderer: Index im MeshStore pro asset::Mesh, Index im
// MaterialStore pro asset::Material.
struct ModelInstance {
    std::vector<u32> meshes;
    std::vector<u32> materials;
};

// Lädt Bilder, Materialien und Meshes eines Modells hoch. Bilder, die nur als Grundfarbe oder
// Emission dienen, werden sRGB, alle anderen linear.
[[nodiscard]] rhi::Result<ModelInstance> upload_model(Renderer& renderer,
                                                      const asset::Model& model);

// Legt für die Knoten der Szene Entities an (Name, Transform, MeshRenderer, Hierarchie) unter einer
// neuen Wurzel-Entity und liefert sie.
world::Entity spawn_model(world::World& world, const asset::Model& model,
                          const ModelInstance& instance, std::string_view name);

} // namespace ngine::render
