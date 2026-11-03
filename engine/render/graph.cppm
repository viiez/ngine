module;

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Render-Graph (REND-05, ADR 0026). Jeder Frame deklariert seine Passes und welche Ressourcen sie
// in welchem Zustand lesen oder schreiben. compile() plant daraus ohne GPU: verwirft Passes, deren
// Ergebnis niemand braucht, berechnet die Barrieren und die Lebensdauer transienter Texturen.
// execute() zeichnet den Plan in eine CommandList auf.
//
// Grenzen dieser Fassung: eine Queue, Reihenfolge wie deklariert, transiente Ressourcen nur als
// Texturen und ohne geteilten Speicher (Aliasing).
export module ngine.render:graph;

import ngine.core;
import ngine.rhi;
import :pool;

export namespace ngine::render {

struct TextureHandle {
    u32 index = rhi::invalid_index;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != rhi::invalid_index;
    }
    friend constexpr bool operator==(TextureHandle, TextureHandle) noexcept = default;
};

struct BufferHandle {
    u32 index = rhi::invalid_index;
    [[nodiscard]] constexpr bool valid() const noexcept {
        return index != rhi::invalid_index;
    }
    friend constexpr bool operator==(BufferHandle, BufferHandle) noexcept = default;
};

// Beschreibung einer transienten Textur. Die Verwendungen (Render-Ziel, Sampled, Storage …) leitet
// der Graph aus den Zuständen ab, in denen Passes sie benutzen.
struct TransientTextureDesc {
    rhi::Extent2D extent;
    rhi::Format format = rhi::Format::RGBA8Unorm;
};

enum class ResourceKind : u8 { Texture, Buffer };

struct ResourceRef {
    ResourceKind kind = ResourceKind::Texture;
    u32 index = rhi::invalid_index;
    friend constexpr bool operator==(ResourceRef, ResourceRef) noexcept = default;
};

struct PlannedBarrier {
    ResourceRef resource;
    rhi::ResourceState before = rhi::ResourceState::Undefined;
    rhi::ResourceState after = rhi::ResourceState::Undefined;
    friend constexpr bool operator==(const PlannedBarrier&, const PlannedBarrier&) = default;
};

struct PlannedPass {
    // Index des Passes in der Reihenfolge von add_pass().
    u32 pass = 0;
    // Barrieren direkt vor dem Pass.
    std::vector<PlannedBarrier> barriers;
};

// Erster und letzter Pass (Indizes wie bei add_pass()), der eine transiente Textur benutzt.
struct Lifetime {
    u32 first = rhi::invalid_index;
    u32 last = rhi::invalid_index;
    [[nodiscard]] constexpr bool used() const noexcept {
        return first != rhi::invalid_index;
    }
};

// Ergebnis von compile(), ohne GPU prüfbar.
struct Plan {
    // Ausgeführte Passes in Reihenfolge, mit ihren Barrieren.
    std::vector<PlannedPass> passes;
    // Barrieren nach dem letzten Pass: Endzustände importierter Ressourcen.
    std::vector<PlannedBarrier> final_barriers;
    // Verworfene Passes.
    std::vector<u32> culled;
    // Pro Textur (Index wie TextureHandle); für importierte Texturen ungenutzt.
    std::vector<Lifetime> lifetimes;
    // Pro Textur: abgeleitete Verwendungen.
    std::vector<rhi::TextureUsage> usages;
};

class RenderGraph;

// Erklärt in der Setup-Funktion eines Passes, was er benutzt. Pro Pass hat jede Ressource genau
// einen Zustand.
class PassBuilder {
public:
    void read(TextureHandle texture, rhi::ResourceState state);
    void write(TextureHandle texture, rhi::ResourceState state);
    void read(BufferHandle buffer, rhi::ResourceState state);
    void write(BufferHandle buffer, rhi::ResourceState state);
    // Der Pass wirkt nach außen (z. B. Rückmeldung an die CPU) und wird nie verworfen.
    void side_effect();

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, u32 pass) : graph_{&graph}, pass_{pass} {}
    void use(ResourceRef resource, rhi::ResourceState state, bool write);

    RenderGraph* graph_;
    u32 pass_;
};

// Zugriff auf die echten Ressourcen während execute().
class PassContext {
public:
    rhi::CommandList& commands;

    [[nodiscard]] rhi::Texture& texture(TextureHandle handle) const;
    [[nodiscard]] rhi::Buffer& buffer(BufferHandle handle) const;

private:
    friend class RenderGraph;
    PassContext(rhi::CommandList& commands, const RenderGraph& graph)
        : commands{commands}, graph_{&graph} {}
    const RenderGraph* graph_;
};

class RenderGraph {
public:
    using Setup = std::function<void(PassBuilder&)>;
    using Execute = std::function<void(PassContext&)>;

    // Textur, die außerhalb des Graphs lebt (Backbuffer, dauerhafte Ziele). initial ist ihr
    // Zustand vor dem ersten Pass, final der Zustand, den sie am Ende haben muss; mit Undefined
    // als final bleibt sie im Zustand ihrer letzten Benutzung.
    TextureHandle import_texture(std::string_view name, rhi::Texture& texture,
                                 rhi::ResourceState initial, rhi::ResourceState final);
    // Textur, die nur innerhalb dieses Frames lebt; sie kommt aus dem TexturePool.
    TextureHandle create_texture(std::string_view name, const TransientTextureDesc& desc);
    BufferHandle import_buffer(std::string_view name, rhi::Buffer& buffer,
                               rhi::ResourceState initial, rhi::ResourceState final);

    // setup läuft sofort, execute erst in execute(), und nur, wenn der Pass nicht verworfen wird.
    void add_pass(std::string_view name, const Setup& setup, Execute execute);

    // Plant den Graph. Fehler: Zugriff mit falscher Art (z. B. write() mit einem lesenden
    // Zustand), widersprüchliche Zustände in einem Pass, Lesen einer transienten Textur, bevor
    // ein Pass sie geschrieben hat.
    [[nodiscard]] rhi::Result<void> compile();
    // Nur nach erfolgreichem compile().
    [[nodiscard]] const Plan& plan() const noexcept {
        return plan_;
    }

    // Plant bei Bedarf und zeichnet alle Passes mit ihren Barrieren auf. Transiente Texturen
    // kommen aus pool; der Aufrufer ruft pool.end_frame(), wenn der Frame aufgezeichnet ist.
    void execute(rhi::CommandList& commands, TexturePool& pool);

    // Leert den Graph für den nächsten Frame.
    void reset();

    [[nodiscard]] std::string_view pass_name(u32 pass) const {
        return passes_[pass].name;
    }
    [[nodiscard]] std::string_view texture_name(TextureHandle texture) const {
        return textures_[texture.index].name;
    }
    // Größe und Format einer Textur, importiert oder transient.
    [[nodiscard]] const TransientTextureDesc& texture_desc(TextureHandle texture) const {
        return textures_[texture.index].desc;
    }
    [[nodiscard]] usize pass_count() const noexcept {
        return passes_.size();
    }

private:
    friend class PassBuilder;
    friend class PassContext;

    struct TextureEntry {
        std::string name;
        rhi::Texture* texture = nullptr;
        bool imported = false;
        TransientTextureDesc desc;
        rhi::ResourceState initial = rhi::ResourceState::Undefined;
        rhi::ResourceState final = rhi::ResourceState::Undefined;
    };
    struct BufferEntry {
        std::string name;
        rhi::Buffer* buffer = nullptr;
        rhi::ResourceState initial = rhi::ResourceState::Undefined;
        rhi::ResourceState final = rhi::ResourceState::Undefined;
    };
    struct Access {
        ResourceRef resource;
        rhi::ResourceState state;
        bool write;
    };
    struct Pass {
        std::string name;
        Execute execute;
        std::vector<Access> accesses;
        bool side_effect = false;
    };

    [[nodiscard]] std::string resource_name(ResourceRef resource) const;
    [[nodiscard]] bool imported(ResourceRef resource) const;
    [[nodiscard]] rhi::ResourceState initial_state(ResourceRef resource) const;
    [[nodiscard]] rhi::ResourceState final_state(ResourceRef resource) const;
    void record_barriers(rhi::CommandList& commands, const std::vector<PlannedBarrier>& barriers);

    std::vector<TextureEntry> textures_;
    std::vector<BufferEntry> buffers_;
    std::vector<Pass> passes_;
    std::vector<std::string> errors_;
    Plan plan_;
    bool compiled_ = false;
};

} // namespace ngine::render
