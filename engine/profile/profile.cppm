module;

#include <string_view>

// Profiling der Engine. Die Zonen setzt man mit den Makros aus <ngine/profile.hpp>, weil jede
// Zone statische Daten zu ihrer Quellstelle braucht. Dahinter steckt Tracy (ADR 0016); ohne
// NGINE_WITH_TRACY kostet alles nichts.
export module ngine.profile;

import ngine.core;

export namespace ngine::profile {

inline constexpr bool enabled = NGINE_WITH_TRACY != 0;

// Quellstelle einer Zone. Muss statisch leben; die Makros legen sie an.
struct SourceLocation {
    const char* name;
    const char* function;
    const char* file;
    u32 line;
    // 0xRRGGBB, 0 für die Standardfarbe.
    u32 color;
};

// Misst die Zeit von der Erzeugung bis zum Ende des Blocks.
class Zone {
public:
    explicit Zone(const SourceLocation& location) noexcept;
    ~Zone();
    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

private:
    // Platz für den Zonen-Kontext des Profilers.
    alignas(8) unsigned char context_[16];
};

// Name des aufrufenden Threads in der Profiler-Anzeige. Der Text wird kopiert.
void set_thread_name(const char* name) noexcept;
// Markiert das Ende eines Frames.
void frame_mark() noexcept;
// Wert über die Zeit, z. B. Speicherverbrauch. name muss statisch leben.
void plot(const char* name, f64 value) noexcept;
// Textnachricht in der Zeitleiste.
void message(std::string_view text) noexcept;

// --- GPU-Zeitleiste (CORE-07) -----------------------------------------------------------------
// Zeiten der GPU kommen über Zeitstempel, die erst Frames später vorliegen. Darum werden Zonen beim
// Aufzeichnen mit einer Abfrage-Nummer geöffnet und geschlossen und die Zeiten nachgereicht. Alle
// Aufrufe für einen Kontext aus demselben Thread; Abfrage-Nummern dürfen erst wieder vergeben
// werden, wenn ihre Zeit gemeldet ist.

// Legt eine GPU-Zeitleiste an. gpu_time: Zeitstempel der GPU, der jetzt gilt (in Ticks);
// period: Nanosekunden pro Tick. Liefert die Nummer des Kontexts.
[[nodiscard]] u8 gpu_context(std::string_view name, i64 gpu_time, f32 period) noexcept;
// Zone mit Namen (der Text wird kopiert), beginnt beim Zeitstempel query.
void gpu_zone_begin(u8 context, u16 query, std::string_view name) noexcept;
// Schließt die zuletzt geöffnete Zone beim Zeitstempel query.
void gpu_zone_end(u8 context, u16 query) noexcept;
// Reicht den Wert eines Zeitstempels nach.
void gpu_time(u8 context, u16 query, i64 gpu_time) noexcept;

} // namespace ngine::profile
