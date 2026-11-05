// Größe des Viewports (EDIT-02): Das Bild der Szene folgt der Größe seines Fensters mit einem Frame
// Verzug. Die Zeichenliste eines Frames zeigt auf die Textur, die beim Aufbau der UI galt; erst
// danach darf eine neue entstehen. Ohne GPU prüfbar.
export module ngine.editor_ui:viewport;

import ngine.core;

export namespace ngine::editor_ui {

struct Extent {
    u32 width = 0;
    u32 height = 0;
    friend constexpr bool operator==(Extent, Extent) noexcept = default;
};

class ViewportSize {
public:
    // Kleinste und größte Seitenlänge des Bildes in Pixeln.
    static constexpr u32 min_side = 16;
    static constexpr u32 max_side = 8192;

    explicit ViewportSize(Extent initial = {1280, 720}) : current_{clamp(initial)} {}

    // Größe, die das Fenster in diesem Frame anbietet, in Pixeln (auch Bruchteile oder 0, wenn
    // es eingeklappt ist; dann bleibt die alte Größe).
    void request(f32 width, f32 height) {
        if (width < 1.0f || height < 1.0f) {
            return;
        }
        requested_ = clamp({static_cast<u32>(width), static_cast<u32>(height)});
    }

    // Zu Beginn eines Frames, bevor die UI aufgebaut wird: die neue Größe, wenn sich die Anfrage
    // des letzten Frames geändert hat; sie gilt danach als aktuell.
    [[nodiscard]] bool take_change(Extent& out) {
        if (requested_ == current_) {
            return false;
        }
        current_ = requested_;
        out = current_;
        return true;
    }

    [[nodiscard]] Extent current() const noexcept {
        return current_;
    }

private:
    static constexpr Extent clamp(Extent e) {
        const auto side = [](u32 v) {
            return v < min_side ? min_side : (v > max_side ? max_side : v);
        };
        return {side(e.width), side(e.height)};
    }

    Extent current_;
    Extent requested_ = current_;
};

} // namespace ngine::editor_ui
