module;

#include <memory>
#include <vector>

// Vorrat an Texturen für transiente Ressourcen des Render-Graphs. Texturen mit gleicher
// Beschreibung werden über Frames hinweg wiederverwendet, statt jeden Frame neu angefordert, und
// seit R3.5 auch innerhalb eines Frames, wenn ihre Lebensdauern sich nicht überschneiden (TD-16).
export module ngine.render:pool;

import ngine.core;
import ngine.rhi;

export namespace ngine::render {

class TexturePool {
public:
    // Unbenutzte Texturen werden nach so vielen Frames zerstört.
    static constexpr u32 max_idle_frames = 8;

    explicit TexturePool(rhi::Device& device) : device_{&device} {}

    // Eine freie Textur mit dieser Größe und diesem Format, deren Verwendungen alle von desc
    // einschließen (gleiche Verwendungen bevorzugt), sonst eine neue. Bis release() oder
    // end_frame() gehört sie dem Aufrufer. Ihr Inhalt ist undefiniert. previous bekommt den
    // Zustand, in dem ein früherer Pass desselben Frames sie zurückgegeben hat (sonst Undefined):
    // Die erste Barriere muss auf dessen Zugriffe warten.
    [[nodiscard]] rhi::Texture& acquire(const rhi::TextureDesc& desc,
                                        rhi::ResourceState* previous = nullptr);

    // Gibt eine Textur vor end_frame() zurück, die zuletzt im Zustand state benutzt wurde. Ein
    // späterer acquire() im selben Frame kann sie bekommen.
    void release(const rhi::Texture& texture, rhi::ResourceState state);

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
        // In diesem Frame benutzt und mit diesem Zustand zurückgegeben.
        bool used_in_frame = false;
        rhi::ResourceState state = rhi::ResourceState::Undefined;
        u32 idle_frames = 0;
    };

    rhi::Device* device_;
    std::vector<Entry> entries_;
};

} // namespace ngine::render
