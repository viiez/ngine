module;

#include <memory>
#include <optional>
#include <span>

// Zeichnet die Editor-UI als Pass im Render-Graph, nur mit der RHI (ADR 0029): Eckpunkte über
// Vertex Pulling, Texturen bindlos, ein Scissor pro Draw. Kein Vulkan-Backend von ImGui, damit
// die Regel „nur die RHI kennt Vulkan“ gilt und der Pass mit jedem RHI-Backend läuft.
export module ngine.editor_ui:pass;

import ngine.core;
import ngine.rhi;
import ngine.render;
import :draw;

export namespace ngine::editor_ui {

// Kennung, unter der die UI eine Textur der RHI zeigt (image()). Die Textur muss Sampled haben und
// beim Zeichnen der UI im Zustand ShaderRead sein; ist sie im Render-Graph, gehört ihr Handle in
// die gelesenen Texturen von add_pass().
[[nodiscard]] inline TextureId texture_id(const rhi::Texture& texture) {
    return texture.sampled_index() == rhi::invalid_index ? no_texture
                                                         : TextureId{texture.sampled_index()} + 1;
}

// Höchstens so viele Frames im Flug: Puffer der Zeichenliste reihum.
class UiRenderer final : public TextureHost {
public:
    // target_format: Format der Ziele, in die add_pass() zeichnet.
    [[nodiscard]] static rhi::Result<std::unique_ptr<UiRenderer>> create(rhi::Device& device,
                                                                         rhi::Format target_format);
    ~UiRenderer() override;

    // TextureHost: Texturen der UI (Schriftatlas) in der RHI.
    [[nodiscard]] TextureId create_texture(u32 width, u32 height,
                                           std::span<const u8> rgba) override;
    void destroy_texture(TextureId texture) override;

    // Zeichnet die Zeichenliste in target (Format aus create(), endet im Zustand ColorTarget).
    // Mit clear wird das Ziel vorher gelöscht, sonst über den Inhalt gezeichnet. reads sind die
    // Texturen des Graphs, die die UI über image() zeigt; der Graph bringt sie nach ShaderRead.
    // Die Zeichenliste wird sofort kopiert.
    void add_pass(render::RenderGraph& graph, const UiDrawData& draw_data,
                  render::TextureHandle target, std::span<const render::TextureHandle> reads = {},
                  std::optional<rhi::Color> clear = std::nullopt);

    // Draws des letzten add_pass().
    [[nodiscard]] u32 draw_count() const noexcept;

private:
    struct Impl;
    explicit UiRenderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::editor_ui
