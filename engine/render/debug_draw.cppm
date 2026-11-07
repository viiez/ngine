module;

#include <span>
#include <string>
#include <vector>

// Debug-Zeichnen (REND-15): Linien und Texte in 64-Bit-Weltkoordinaten, gesammelt für einen
// Frame. Der Renderer zeichnet die Linien im Vorwärts-Pass mit Tiefentest und leert danach beide
// Listen. Texte zeichnet er nicht selbst, weil die Engine keine Schrift hat: Die Anwendung bringt
// sie mit ihrer UI ins Bild, bevor add_passes() die Liste leert (der Editor über die Zeichenliste
// von Dear ImGui, seit R2.3).
export module ngine.render:debug_draw;

import ngine.core;
import ngine.math;

export namespace ngine::render {

struct DebugLine {
    math::DVec3 a;
    math::DVec3 b;
    math::Vec3 color;
};

// Text an einem Punkt der Welt, z. B. Name oder Wert eines Objekts.
struct DebugText {
    math::DVec3 position;
    std::string text;
    math::Vec3 color;
};

// Eine Linie, wie debug.slang sie liest: Endpunkte kamerarelativ (48 Bytes).
struct GpuDebugLine {
    math::Vec4 a;
    math::Vec4 b;
    math::Vec4 color;
};
static_assert(sizeof(GpuDebugLine) == 48);

class DebugDraw {
public:
    // Aus heißt: Aufrufe sammeln nichts, der Renderer zeichnet nichts.
    bool enabled = true;
    // Breite der Linien in Pixeln.
    f32 width = 2.0f;

    void line(math::DVec3 a, math::DVec3 b, math::Vec3 color);
    // Quader von local_min bis local_max, abgebildet mit transform (z. B. die Hülle eines Meshes
    // an der Lage seiner Entity): zwölf Kanten.
    void box(const math::Affine& transform, math::Vec3 local_min, math::Vec3 local_max,
             math::Vec3 color);
    // Achsenparalleler Quader in Weltkoordinaten.
    void aabb(math::DVec3 min, math::DVec3 max, math::Vec3 color);
    // Drei Kreise um die Achsen.
    void sphere(math::DVec3 center, f32 radius, math::Vec3 color, u32 segments = 32);
    // Achsenkreuz: x rot, y grün, z blau, jeweils size lang.
    void axes(const math::Affine& transform, f32 size);

    // Text, dessen linke obere Ecke im Bild über position liegt.
    void text(math::DVec3 position, std::string text, math::Vec3 color = {1.0f, 1.0f, 1.0f});

    void clear() noexcept;
    [[nodiscard]] std::span<const DebugLine> lines() const noexcept;
    [[nodiscard]] std::span<const DebugText> texts() const noexcept;

private:
    std::vector<DebugLine> lines_;
    std::vector<DebugText> texts_;
};

// Schneidet eine Linie (kamerarelativ) an der Ebene dot(p, forward) = near ab. false, wenn sie
// ganz dahinter liegt.
[[nodiscard]] bool clip_line(math::Vec3& a, math::Vec3& b, math::Vec3 forward, f32 near_plane);

// Linien relativ zur Kamera und vor ihrer nahen Ebene, bereit für die GPU.
[[nodiscard]] std::vector<GpuDebugLine> prepare_lines(std::span<const DebugLine> lines,
                                                      math::DVec3 camera, math::Vec3 forward,
                                                      f32 near_plane);

} // namespace ngine::render
