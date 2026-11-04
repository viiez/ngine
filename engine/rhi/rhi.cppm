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

enum class Format : u8 {
    Unknown,
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA8Srgb,
    BGRA8Srgb,
    RGBA16Float,
    R32Float,
    D32Float,
};
enum class ShaderStage : u8 { Vertex, Fragment, Compute };
enum class DeviceType : u8 { Other, Integrated, Discrete, Virtual, Cpu };

[[nodiscard]] constexpr u32 bytes_per_pixel(Format format) noexcept {
    switch (format) {
    case Format::Unknown: return 0;
    case Format::RGBA16Float: return 8;
    default: return 4;
    }
}

[[nodiscard]] constexpr bool is_depth(Format format) noexcept {
    return format == Format::D32Float;
}

// Größe der Push Constants, die jede Pipeline anbietet (Mindestwert, den Vulkan garantiert).
inline constexpr u32 max_push_constant_size = 128;

// Index im bindlosen Descriptor-Satz, den eine Ressource ohne passende Verwendung hat.
inline constexpr u32 invalid_index = ~u32{0};

// Wie eine Ressource gerade benutzt wird. Barrieren beschreiben Übergänge zwischen zwei
// Zuständen; das Backend leitet daraus Pipeline-Stufen, Zugriffe und Layouts ab.
enum class ResourceState : u8 {
    // Inhalt wird nicht gebraucht. Nur als Vorher-Zustand erlaubt.
    Undefined,
    ColorTarget,
    DepthTarget,
    // Tiefenpuffer, der getestet, aber nicht geschrieben wird. Zum Lesen im Shader: ShaderRead.
    DepthRead,
    // Lesen in einem beliebigen Shader: Texturen über den bindlosen Satz, Puffer über ihre Adresse.
    ShaderRead,
    // Lesen und Schreiben in einem beliebigen Shader (Storage-Texturen und -Puffer).
    ShaderWrite,
    IndexRead,
    TransferSrc,
    TransferDst,
    // Bereit für die Präsentation (nur Swapchain-Bilder).
    Present,
};

[[nodiscard]] constexpr bool is_write(ResourceState state) noexcept {
    return state == ResourceState::ColorTarget || state == ResourceState::DepthTarget ||
           state == ResourceState::ShaderWrite || state == ResourceState::TransferDst;
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
    // Höchste anisotrope Filterung der Sampler; 1 heißt: keine.
    f32 max_anisotropy = 1.0f;
};

class Resource {
public:
    Resource() = default;
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    virtual ~Resource() = default;
};

enum class TextureUsage : u8 {
    // Farbziel bzw. Tiefenpuffer beim Zeichnen.
    RenderTarget = 1,
    // Im Shader über sampled_index() lesbar.
    Sampled = 2,
    // Im Shader über storage_index() les- und schreibbar.
    Storage = 4,
    TransferSrc = 8,
    TransferDst = 16,
};

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has(TextureUsage set, TextureUsage flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0;
}

// Anzahl der Mip-Stufen bis hinunter zu 1x1.
[[nodiscard]] constexpr u32 full_mip_count(Extent2D extent) noexcept {
    u32 size = extent.width > extent.height ? extent.width : extent.height;
    u32 levels = 1;
    while (size > 1) {
        size /= 2;
        ++levels;
    }
    return levels;
}

struct TextureDesc {
    Extent2D extent;
    Format format = Format::RGBA8Unorm;
    TextureUsage usage = TextureUsage::RenderTarget | TextureUsage::TransferSrc;
    // Mehr als eine Stufe nur für Texturen zum Lesen (ohne RenderTarget und Storage); die Stufen
    // füllt Device::upload().
    u32 mip_levels = 1;
};

// 2D-Textur. Eine neue Textur ist im Zustand Undefined; Barrieren gelten für alle Mip-Stufen. Mit
// Sampled oder Storage hat sie einen festen Index im bindlosen Descriptor-Satz, über den Shader sie
// erreichen.
class Texture : public Resource {
public:
    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual Format format() const = 0;
    [[nodiscard]] virtual TextureUsage usage() const = 0;
    [[nodiscard]] virtual u32 mip_levels() const = 0;
    // Index im Feld der lesbaren Texturen (zu lesen im Zustand ShaderRead), sonst invalid_index.
    [[nodiscard]] virtual u32 sampled_index() const = 0;
    // Index im Feld der Storage-Texturen (im Zustand ShaderWrite), sonst invalid_index.
    [[nodiscard]] virtual u32 storage_index() const = 0;
};

enum class Filter : u8 { Nearest, Linear };
enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge };

struct SamplerDesc {
    Filter mag_filter = Filter::Linear;
    Filter min_filter = Filter::Linear;
    // Zwischen den Mip-Stufen.
    Filter mip_filter = Filter::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    // Gewünschte anisotrope Filterung; begrenzt auf Capabilities::max_anisotropy.
    f32 anisotropy = 1.0f;
};

// Sampler im bindlosen Descriptor-Satz.
class Sampler : public Resource {
public:
    [[nodiscard]] virtual u32 index() const = 0;
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

enum class MemoryUsage : u8 {
    // Schneller Speicher der GPU; beschrieben wird nur über Device::upload().
    Gpu,
    // Speicher, den die CPU direkt über Buffer::write() beschreibt. Mit Resizable BAR liegt er auf
    // der GPU, sonst im Systemspeicher.
    CpuWrite,
};

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Storage;
    MemoryUsage memory = MemoryUsage::CpuWrite;
};

// GPU-Puffer. Shader lesen ihn über device_address().
class Buffer : public Resource {
public:
    [[nodiscard]] virtual u64 size() const = 0;
    [[nodiscard]] virtual u64 device_address() const = 0;
    [[nodiscard]] virtual MemoryUsage memory() const = 0;
    // Nur für MemoryUsage::CpuWrite. Wer ihn ändert, während die GPU ihn noch liest, muss selbst
    // warten oder pro Frame im Flug einen eigenen Puffer nehmen (Frame::index).
    virtual void write(u64 offset, std::span<const std::byte> data) = 0;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const u32> spirv;
    std::string entry_point = "main";
};

class Shader : public Resource {};

enum class CullMode : u8 { None, Back };

// Wann ein Fragment den Tiefentest besteht. Die Kamera nutzt umgekehrte Tiefe (nah 1, fern 0)
// mit Greater; LessOrEqual passt zu klassischer Tiefe.
enum class CompareOp : u8 { LessOrEqual, Greater, GreaterOrEqual };

enum class BlendMode : u8 {
    // Farbe ersetzt das Ziel.
    None,
    // Klassisches Überblenden: src * a + dst * (1 - a).
    Alpha,
};

// Dreiecke gegen den Uhrzeigersinn sind Vorderseiten. Geometrie kommt nicht über Vertex-Attribute,
// sondern liest der Shader selbst aus Puffern (ADR 0022). Alle Pipelines teilen ein Layout: den
// bindlosen Descriptor-Satz (Satz 0, siehe bindless.slang) und max_push_constant_size Bytes Push
// Constants für alle Stufen.
struct GraphicsPipelineDesc {
    const Shader* vertex = nullptr;
    const Shader* fragment = nullptr;
    Format color_format = Format::Unknown;
    // Mit Tiefenformat prüft die Pipeline Tiefe mit depth_compare und schreibt sie, wenn
    // depth_write gesetzt ist.
    Format depth_format = Format::Unknown;
    CompareOp depth_compare = CompareOp::LessOrEqual;
    bool depth_write = true;
    CullMode cull_mode = CullMode::None;
    BlendMode blend = BlendMode::None;
};

struct ComputePipelineDesc {
    const Shader* compute = nullptr;
};

class Pipeline : public Resource {};

struct TextureBarrier {
    const Texture* texture = nullptr;
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
};

struct BufferBarrier {
    const Buffer* buffer = nullptr;
    ResourceState before = ResourceState::Undefined;
    ResourceState after = ResourceState::Undefined;
};

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

    // Übergänge zwischen Zuständen. Die CommandList merkt sich keine Zustände; wer aufzeichnet,
    // kennt sie (meist der Render-Graph). Auch zwischen zwei schreibenden Nutzungen im selben
    // Zustand ist eine Barriere nötig.
    virtual void barrier(std::span<const TextureBarrier> textures,
                         std::span<const BufferBarrier> buffers = {}) = 0;
    void barrier(const Texture& texture, ResourceState before, ResourceState after) {
        const TextureBarrier one{.texture = &texture, .before = before, .after = after};
        barrier(std::span{&one, 1});
    }

    // Beginnt das Zeichnen. Farbziel und Tiefenpuffer müssen in den Zuständen ColorTarget bzw.
    // DepthTarget sein. Viewport und Scissor decken das ganze Farbziel ab; die y-Achse zeigt
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
    // Nur mit gebundener Compute-Pipeline, außerhalb von begin_rendering().
    virtual void dispatch(u32 groups_x, u32 groups_y = 1, u32 groups_z = 1) = 0;
};

struct Frame {
    CommandList& commands;
    // Zu Beginn im Zustand Undefined; vor end_frame() muss es im Zustand Present sein.
    Texture& backbuffer;
    // Platz dieses Frames unter den Frames im Flug (0 bis frames_in_flight() - 1). Ressourcen, die
    // der Frame mit demselben Index benutzt hat, sind jetzt wieder frei.
    u32 index = 0;
};

// Ein Device darf nur von einem Thread zugleich benutzt werden. Ressourcen dürfen jederzeit
// zerstört werden, auch wenn ein Frame im Flug sie noch benutzt: Das Device gibt sie erst frei,
// wenn die GPU damit fertig ist.
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
    [[nodiscard]] virtual Result<std::unique_ptr<Pipeline>>
    create_compute_pipeline(const ComputePipelineDesc& desc) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Buffer>> create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Sampler>>
    create_sampler(const SamplerDesc& desc) = 0;

    // Kopiert Daten über Staging-Speicher in einen Puffer, ohne auf die GPU zu warten. Die Kopie
    // läuft vor der nächsten Abgabe (end_frame() oder execute()) und ist für alles danach
    // sichtbar; GPU-Arbeit davor ist mit dem Puffer fertig, bevor die Kopie ihn überschreibt.
    virtual void upload(Buffer& buffer, u64 offset, std::span<const std::byte> data) = 0;
    // Wie oben für die erste Mip-Stufe einer Textur mit TransferDst (Zeilen ohne Abstand). Die
    // weiteren Stufen rechnet die GPU daraus (linear verkleinert). Danach ist die Textur im
    // Zustand ShaderRead.
    virtual void upload(Texture& texture, std::span<const std::byte> data) = 0;

    // Bildschirmausgabe, nur mit Surface. begin_frame() liefert nichts, solange gerade
    // nicht gezeichnet werden kann (z. B. minimiertes Fenster).
    [[nodiscard]] virtual Format swapchain_format() const = 0;
    [[nodiscard]] virtual u32 frames_in_flight() const = 0;
    [[nodiscard]] virtual std::optional<Frame> begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual void resize(Extent2D extent) = 0;

    // Führt Befehle sofort aus und wartet, bis die GPU fertig ist. Für Werkzeuge und Tests;
    // Frames laufen über begin_frame() und end_frame().
    virtual void execute(const std::function<void(CommandList&)>& record) = 0;
    // Liest eine Textur mit TransferSrc zurück, Zeile für Zeile ohne Abstand. state ist ihr
    // Zustand vorher und nachher.
    [[nodiscard]] virtual std::vector<u8> read_texture(Texture& texture, ResourceState state) = 0;

    virtual void wait_idle() = 0;

    // Anzahl der Fehler, die die Debug-Schicht bisher gemeldet hat.
    [[nodiscard]] virtual u32 validation_errors() const = 0;
};

} // namespace ngine::rhi
