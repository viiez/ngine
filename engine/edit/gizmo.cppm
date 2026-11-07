module;

#include <optional>
#include <string>
#include <vector>

export module ngine.edit:gizmo;

import ngine.core;
import ngine.math;
import ngine.world;
import :document;

// Gizmos zum Verschieben, Drehen und Skalieren (EDIT-05, ADR 0031): Treffertests über Strahlen
// gegen Achsen, Ringe und eine Kugel in der Mitte, Ziehen mit Raster, lokal oder global. Die Logik
// kennt weder UI noch GPU; der Editor projiziert die Form ins Bild und zeichnet sie, Eingaben
// kommen als Mausposition in Pixeln des Viewports.
export namespace ngine::edit {

// Kamera, wie sie der Viewport zeigt: Strahl unter einem Pixel und Projektion eines Punktes,
// passend zur Projektion des Renderers (Perspektive, Pixel oben links 0, 0).
struct ViewCamera {
    math::DVec3 position;
    // Spalten: rechts, oben, nach hinten (die Kamera blickt entlang -z).
    math::Mat3 rotation;
    f32 fov_y = 1.0f;
    f32 near_plane = 0.05f;
    // Größe des Viewports in Pixeln.
    f32 width = 1.0f;
    f32 height = 1.0f;

    [[nodiscard]] math::Ray ray(f32 x, f32 y) const noexcept;
    // Pixel des Punktes; nichts, wenn er hinter der nahen Ebene liegt.
    [[nodiscard]] std::optional<math::Vec2> project(math::DVec3 point) const noexcept;
    // Größe eines Pixels in Welteinheiten in der Tiefe von point.
    [[nodiscard]] f64 pixel_size(math::DVec3 point) const noexcept;
};

enum class GizmoMode : u8 { Translate, Rotate, Scale };
// Achsen der Welt oder der Entity. Skalieren arbeitet immer in den Achsen der Entity.
enum class GizmoSpace : u8 { World, Local };
// All: gleichmäßig skalieren (Kugel in der Mitte).
enum class GizmoAxis : u8 { None, X, Y, Z, All };

struct GizmoSnap {
    bool enabled = false;
    // Raster beim Verschieben in Metern. Global rasten Positionen auf das Raster der Welt ein,
    // lokal die Verschiebung.
    f64 translate = 0.5;
    // Winkelschritt beim Drehen im Bogenmaß.
    f32 rotate = math::radians(15.0f);
    // Schritt der Skalierung.
    f32 scale = 0.1f;
};

// Eingabe eines Frames: Maus in Pixeln des Viewports und linke Taste.
struct GizmoInput {
    f32 x = 0.0f;
    f32 y = 0.0f;
    // Maus über dem Viewport.
    bool hovered = false;
    bool pressed = false;
    bool down = false;
};

// Lage des Gizmos in der Welt: Mittelpunkt, Achsen als normierte Spalten und Länge der Achsen
// in Welteinheiten (konstant in Pixeln, also abhängig von der Entfernung zur Kamera).
struct GizmoFrame {
    math::DVec3 origin;
    math::Mat3 axes;
    f64 size = 1.0;
};

// Form zum Zeichnen: Linien und Griffe in Weltkoordinaten, jeweils mit ihrer Achse.
struct GizmoShape {
    enum class Handle : u8 { Arrow, Box };
    struct Line {
        math::DVec3 a;
        math::DVec3 b;
        GizmoAxis axis = GizmoAxis::None;
    };
    struct Marker {
        math::DVec3 position;
        // Für Pfeile: Richtung der Spitze.
        math::DVec3 direction;
        Handle handle = Handle::Arrow;
        GizmoAxis axis = GizmoAxis::None;
    };
    std::vector<Line> lines;
    std::vector<Marker> markers;
};

class Gizmo {
public:
    GizmoMode mode = GizmoMode::Translate;
    GizmoSpace space = GizmoSpace::World;
    GizmoSnap snap;
    // Länge der Achsen in Pixeln.
    f32 size_pixels = 110.0f;

    // Lage für entity, nichts ohne Transform.
    [[nodiscard]] std::optional<GizmoFrame> frame(SceneDocument& document, EntityId entity,
                                                  const ViewCamera& camera) const;
    // Achse unter dem Strahl, None ohne Treffer.
    [[nodiscard]] GizmoAxis hit(const GizmoFrame& frame, const math::Ray& ray,
                                const ViewCamera& camera) const;
    [[nodiscard]] GizmoShape shape(const GizmoFrame& frame) const;

    // Ein Frame: Achse unter der Maus merken, mit gedrückter Taste auf einer Achse das Ziehen
    // beginnen, beim Ziehen Transform über einen zusammenfassenden Befehl setzen, beim Loslassen
    // den Schritt im Verlauf abschließen. Ein ganzes Ziehen ist ein Undo-Schritt. Liefert true,
    // wenn der Gizmo die Maus benutzt hat (dann wählt ein Klick nichts anderes aus).
    bool update(SceneDocument& document, EntityId entity, const ViewCamera& camera,
                const GizmoInput& input);
    // Bricht ein Ziehen ab, ohne den Verlauf anzufassen (z. B. beim Start des Spielens).
    void reset() noexcept;

    [[nodiscard]] bool dragging() const noexcept {
        return drag_.has_value();
    }
    // Achse unter der Maus bzw. die gezogene Achse.
    [[nodiscard]] GizmoAxis hovered() const noexcept {
        return hovered_;
    }
    [[nodiscard]] GizmoAxis active() const noexcept {
        return drag_ ? drag_->axis : GizmoAxis::None;
    }
    // Wert des laufenden Ziehens zum Anzeigen, z. B. „x +1.50 m“; leer ohne.
    [[nodiscard]] std::string drag_label() const;

private:
    struct Drag {
        EntityId entity = 0;
        GizmoAxis axis = GizmoAxis::None;
        GizmoFrame frame;
        world::Transform start;
        // Weltlage des Elternteils (Einheit ohne).
        math::Affine parent;
        // Richtung, entlang der gezogen wird, und Startwert darauf.
        math::DVec3 direction;
        f64 start_t = 0.0;
        // Drehen: letzter Vektor vom Mittelpunkt zum Treffer in der Ebene, Winkel bisher.
        math::DVec3 last_vector;
        f64 angle = 0.0;
        // Angezeigter Wert (Strecke, Winkel in Grad oder Faktor).
        f64 value = 0.0;
    };

    [[nodiscard]] std::optional<world::Transform> drag_to(Drag& drag, const math::Ray& ray) const;

    GizmoAxis hovered_ = GizmoAxis::None;
    std::optional<Drag> drag_;
};

} // namespace ngine::edit
