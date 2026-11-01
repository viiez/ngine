module;

#include <cstddef>
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

enum class Format : u8 { Unknown, RGBA8Unorm, BGRA8Unorm, RGBA8Srgb, BGRA8Srgb, D32Float };
enum class ShaderStage : u8 { Vertex, Fragment };
enum class DeviceType : u8 { Other, Integrated, Discrete, Virtual, Cpu };

[[nodiscard]] constexpr u32 bytes_per_pixel(Format format) noexcept {
    return format == Format::Unknown ? 0 : 4;
}

[[nodiscard]] constexpr bool is_depth(Format format) noexcept {
    return format == Format::D32Float;
}

// Größe der Push Constants, die jede Pipeline anbietet (Mindestwert, den Vulkan garantiert).
inline constexpr u32 max_push_constant_size = 128;

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

// Optionale Fähigkeiten der GPU (ADR 0017). Fehlt eine, nimmt der Renderer einen
// Rückfallpfad; die Engine bleibt in jedem Fall lauffähig.
struct Features {
    // Task- und Mesh-Shader für GPU-getriebene Geometrie.
    bool mesh_shader = false;
    // Beschleunigungsstrukturen und Ray Queries.
    bool ray_tracing = false;
};

struct DeviceDesc {
    std::string application_name = "NGine";
    // Debug-Schicht der API (bei Vulkan die Validation Layers), falls installiert.
    bool enable_validation = false;
    bool vsync = true;
    // Ohne Surface arbeitet das Device nur offscreen (Tests, Werkzeuge).
    std::optional<SurfaceDesc> surface;
    // Optionale Stufen, die nicht genutzt werden, auch wenn die GPU sie kann. So lassen sich
    // die Rückfallpfade auf starker Hardware testen.
    Features disabled_features;
};

// Beschreibung der gewählten GPU und der optionalen Stufen, die das Device eingeschaltet hat.
struct Capabilities {
    std::string device_name;
    std::string api_version;
    DeviceType device_type = DeviceType::Other;
    Features features;
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

// Bisher nur als Render-Ziel nutzbar: Farbformate lassen sich auslesen, Tiefenformate dienen als
// Tiefenpuffer.
class Texture : public Resource {
public:
    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual Format format() const = 0;
};

enum class BufferUsage : u8 {
    // Nur über die Adresse im Shader lesbar (Vertex Pulling, ADR 0022).
    Storage = 1,
    // Zusätzlich als Indexpuffer mit 32-Bit-Indizes.
    Index = 2,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has(BufferUsage set, BufferUsage flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0;
}

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Storage;
};

// GPU-Puffer, den die CPU direkt beschreibt (TD-11: noch ohne Staging in reinen GPU-Speicher).
// Shader lesen ihn über device_address(). Wer ihn ändert, während die GPU ihn noch liest, muss
// selbst warten oder pro Frame im Flug einen eigenen Puffer nehmen (Frame::index).
class Buffer : public Resource {
public:
    [[nodiscard]] virtual u64 size() const = 0;
    [[nodiscard]] virtual u64 device_address() const = 0;
    virtual void write(u64 offset, std::span<const std::byte> data) = 0;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const u32> spirv;
    std::string entry_point = "main";
};

class Shader : public Resource {};

enum class CullMode : u8 { None, Back };

// Dreiecke gegen den Uhrzeigersinn sind Vorderseiten. Geometrie kommt nicht über Vertex-Attribute,
// sondern liest der Shader selbst aus Puffern (ADR 0022). Jede Pipeline hat
// max_push_constant_size Bytes Push Constants für alle Stufen.
struct GraphicsPipelineDesc {
    const Shader* vertex = nullptr;
    const Shader* fragment = nullptr;
    Format color_format = Format::Unknown;
    // Mit Tiefenformat prüft und schreibt die Pipeline Tiefe (kleiner gleich gewinnt).
    Format depth_format = Format::Unknown;
    CullMode cull_mode = CullMode::None;
};

class Pipeline : public Resource {};

struct RenderingDesc {
    Texture* color = nullptr;
    // Ohne Löschfarbe bleibt der alte Inhalt erhalten.
    std::optional<Color> clear_color;
    // Optionaler Tiefenpuffer; ohne clear_depth bleibt sein Inhalt erhalten.
    Texture* depth = nullptr;
    std::optional<f32> clear_depth = 1.0f;
};

class CommandList {
public:
    CommandList() = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    virtual ~CommandList() = default;

    // Beginnt das Zeichnen. Viewport und Scissor decken das ganze Farbziel ab; die y-Achse zeigt
    // nach oben, die Tiefe läuft von 0 (nah) bis 1 (fern).
    virtual void begin_rendering(const RenderingDesc& desc) = 0;
    void begin_rendering(Texture& target, std::optional<Color> clear) {
        begin_rendering(RenderingDesc{.color = &target, .clear_color = clear});
    }
    virtual void end_rendering() = 0;
    virtual void bind_pipeline(const Pipeline& pipeline) = 0;
    // Höchstens max_push_constant_size Bytes; gilt für die gebundene Pipeline.
    virtual void push_constants(std::span<const std::byte> data) = 0;
    virtual void bind_index_buffer(const Buffer& buffer) = 0;
    virtual void draw(u32 vertex_count, u32 instance_count = 1) = 0;
    virtual void draw_indexed(u32 index_count, u32 instance_count = 1, u32 first_index = 0) = 0;
};

struct Frame {
    CommandList& commands;
    Texture& backbuffer;
    // Platz dieses Frames unter den Frames im Flug (0 bis frames_in_flight() - 1). Ressourcen, die
    // der Frame mit demselben Index benutzt hat, sind jetzt wieder frei.
    u32 index = 0;
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
    [[nodiscard]] virtual Result<std::unique_ptr<Buffer>> create_buffer(const BufferDesc& desc) = 0;

    // Bildschirmausgabe, nur mit Surface. begin_frame() liefert nichts, solange gerade
    // nicht gezeichnet werden kann (z. B. minimiertes Fenster).
    [[nodiscard]] virtual Format swapchain_format() const = 0;
    [[nodiscard]] virtual u32 frames_in_flight() const = 0;
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
