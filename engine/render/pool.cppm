module;

#include <memory>
#include <vector>

// Vorrat an Texturen für transiente Ressourcen des Render-Graphs. Texturen mit gleicher
// Beschreibung werden über Frames hinweg wiederverwendet, statt jeden Frame neu angefordert.
export module ngine.render:pool;

import ngine.core;
import ngine.rhi;

export namespace ngine::render {

class TexturePool {
public:
    // Unbenutzte Texturen werden nach so vielen Frames zerstört.
    static constexpr u32 max_idle_frames = 8;

    explicit TexturePool(rhi::Device& device) : device_{&device} {}

    // Eine freie Textur mit genau dieser Beschreibung, sonst eine neue. Bis end_frame() gehört
    // sie dem Aufrufer. Ihr Inhalt ist undefiniert.
    [[nodiscard]] rhi::Texture& acquire(const rhi::TextureDesc& desc);

    // Gibt alle Texturen für den nächsten Frame frei und zerstört lange unbenutzte. Die RHI hält
    // zerstörte Texturen am Leben, bis die GPU fertig ist.
    void end_frame();

    // Anzahl der Texturen im Vorrat, benutzt oder frei.
    [[nodiscard]] usize size() const noexcept {
        return entries_.size();
    }

private:
    struct Entry {
        std::unique_ptr<rhi::Texture> texture;
        rhi::TextureDesc desc;
        bool in_use = false;
        u32 idle_frames = 0;
    };

    rhi::Device* device_;
    std::vector<Entry> entries_;
};

} // namespace ngine::render
