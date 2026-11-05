module;

#include <span>
#include <vector>

// Zeichenliste der Editor-UI in reinen Engine-Typen: Was ImGui zeichnen will, ohne dass der Pass
// oder ein Test ImGui kennen muss. Ohne GPU prüfbar.
export module ngine.editor_ui:draw;

import ngine.core;

export namespace ngine::editor_ui {

// Kennung einer Textur in der UI; 0 heißt: keine. Für Texturen der RHI ist sie der bindlose Index
// plus eins (texture_id() im Pass).
using TextureId = u64;
inline constexpr TextureId no_texture = 0;

// Eckpunkt wie ImDrawVert: Position in Pixeln (Ursprung oben links), Texturkoordinate, Farbe als
// RGBA mit 8 Bit je Kanal, sRGB-kodiert, R im niedrigsten Byte.
struct UiVertex {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    u32 color = 0xFFFFFFFF;
};
static_assert(sizeof(UiVertex) == 20);

// Rechteck in Pixeln, Ursprung oben links.
struct UiRect {
    f32 min_x = 0.0f;
    f32 min_y = 0.0f;
    f32 max_x = 0.0f;
    f32 max_y = 0.0f;
};

// Ein Draw: index_count Indizes ab first_index, beschnitten auf clip, mit einer Textur.
struct UiCommand {
    UiRect clip;
    TextureId texture = no_texture;
    u32 first_index = 0;
    u32 index_count = 0;
};

// Alles, was die UI in einem Frame zeichnet, in Reihenfolge. Die Indizes zeigen direkt in
// vertices (über alle Fenster hinweg schon verschoben). width und height sind die Größe der
// Zeichenfläche in Pixeln.
struct UiDrawData {
    std::vector<UiVertex> vertices;
    std::vector<u32> indices;
    std::vector<UiCommand> commands;
    f32 width = 0.0f;
    f32 height = 0.0f;

    [[nodiscard]] bool empty() const noexcept {
        return commands.empty();
    }
    void clear() noexcept {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
};

// Verwaltet die Texturen, die die UI selbst braucht (Schriftatlas). Der Pass implementiert das mit
// der RHI, Tests ohne GPU mit einer Attrappe. Pixel sind RGBA mit 8 Bit je Kanal, Zeile für Zeile.
class TextureHost {
public:
    TextureHost() = default;
    TextureHost(const TextureHost&) = delete;
    TextureHost& operator=(const TextureHost&) = delete;
    virtual ~TextureHost() = default;

    // Neue Textur mit Inhalt; no_texture bei Fehler.
    [[nodiscard]] virtual TextureId create_texture(u32 width, u32 height,
                                                   std::span<const u8> rgba) = 0;
    // Gibt eine Textur frei. Ein Frame, der sie noch benutzt, darf zu Ende laufen.
    virtual void destroy_texture(TextureId texture) = 0;
};

} // namespace ngine::editor_ui
