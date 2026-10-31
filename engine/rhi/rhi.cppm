module;

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Render Hardware Interface: schmale, API-unabhängige Schnittstelle zwischen Renderer
// und Grafik-API (ADR 0004, ADR 0013). Backends wie ngine.rhi.vulkan implementieren sie.
// Alle Ressourcen müssen vor ihrem Device zerstört werden.
export module ngine.rhi;

import ngine.core;

export namespace ngine::rhi {

template <typename T>
using Result = std::expected<T, std::string>;

enum class Format : u8 { Unknown, RGBA8Unorm, BGRA8Unorm, RGBA8Srgb, BGRA8Srgb };
enum class ShaderStage : u8 { Vertex, Fragment };
enum class DeviceType : u8 { Other, Integrated, Discrete, Virtual, Cpu };

[[nodiscard]] constexpr u32 bytes_per_pixel(Format format) noexcept {
    return format == Format::Unknown ? 0 : 4;
}

struct Extent2D {
    u32 width = 0;
    u32 height = 0;
};

struct Color {
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    f32 a = 1.0f;
};

// Verbindung zu einem Fenster, ohne dass die RHI die Plattformschicht kennt.
// create_surface bekommt die API-Instanz und liefert die API-spezifische Surface
// als Zahl (bei Vulkan eine VkSurfaceKHR), 0 bei Fehler.
struct SurfaceDesc {
    std::vector<const char*> instance_extensions;
    std::function<u64(void* instance)> create_surface;
    Extent2D extent;
};

struct DeviceDesc {
    std::string application_name = "NGine";
    // Debug-Schicht der API (bei Vulkan die Validation Layers), falls installiert.
    bool enable_validation = false;
    bool vsync = true;
    // Ohne Surface arbeitet das Device nur offscreen (Tests, Werkzeuge).
    std::optional<SurfaceDesc> surface;
};

// Was die GPU kann. Grundlage der Feature-Stufen: Die Basis läuft überall,
// optionale Features werden nur genutzt, wo sie vorhanden sind.
struct Capabilities {
    std::string device_name;
    std::string api_version;
    DeviceType device_type = DeviceType::Other;
    bool mesh_shaders = false;
    bool ray_tracing = false;
};

class Resource {
public:
    Resource() = default;
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    virtual ~Resource() = default;
};

struct TextureDesc {
    Extent2D extent;
    Format format = Format::RGBA8Unorm;
};

// Bisher nur als Render-Ziel nutzbar, das sich auslesen lässt.
class Texture : public Resource {
public:
    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual Format format() const = 0;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const u32> spirv;
    std::string entry_point = "main";
};

class Shader : public Resource {};

struct GraphicsPipelineDesc {
    const Shader* vertex = nullptr;
    const Shader* fragment = nullptr;
    Format color_format = Format::Unknown;
};

class Pipeline : public Resource {};

class CommandList {
public:
    CommandList() = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    virtual ~CommandList() = default;

    // Beginnt das Zeichnen in target. Viewport und Scissor decken das ganze Ziel ab;
    // die y-Achse zeigt nach oben. Ohne clear bleibt der alte Inhalt erhalten.
    virtual void begin_rendering(Texture& target, std::optional<Color> clear) = 0;
    virtual void end_rendering() = 0;
    virtual void bind_pipeline(const Pipeline& pipeline) = 0;
    virtual void draw(u32 vertex_count, u32 instance_count = 1) = 0;
};

struct Frame {
    CommandList& commands;
    Texture& backbuffer;
};

class Device {
public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    virtual ~Device() = default;

    [[nodiscard]] virtual const Capabilities& capabilities() const = 0;

    [[nodiscard]] virtual Result<std::unique_ptr<Texture>>
    create_texture(const TextureDesc& desc) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Shader>> create_shader(const ShaderDesc& desc) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Pipeline>>
    create_pipeline(const GraphicsPipelineDesc& desc) = 0;

    // Bildschirmausgabe, nur mit Surface. begin_frame() liefert nichts, solange gerade
    // nicht gezeichnet werden kann (z. B. minimiertes Fenster).
    [[nodiscard]] virtual Format swapchain_format() const = 0;
    [[nodiscard]] virtual std::optional<Frame> begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual void resize(Extent2D extent) = 0;

    // Führt Befehle sofort aus und wartet, bis die GPU fertig ist.
    virtual void execute(const std::function<void(CommandList&)>& record) = 0;
    // Liest eine Textur zurück, Zeile für Zeile ohne Abstand.
    [[nodiscard]] virtual std::vector<u8> read_texture(Texture& texture) = 0;

    virtual void wait_idle() = 0;

    // Anzahl der Fehler, die die Debug-Schicht bisher gemeldet hat.
    [[nodiscard]] virtual u32 validation_errors() const = 0;
};

} // namespace ngine::rhi
