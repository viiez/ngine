module;

export module ngine.audio:components;

import ngine.core;
import ngine.reflect;
import :types;

// Komponenten des Audios (AUD-02) in der Reflection: im Inspector bearbeitbar, in Szenen und
// Prefabs gespeichert und in C# als Structs sichtbar.
export namespace ngine::audio {

// Klangquelle an einer Entity. Mit spatial hört man sie an der Lage der Entity (WorldTransform):
// leiser mit dem Abstand, links oder rechts je nach Blick, höher oder tiefer mit Bewegung.
struct AudioSource {
    // Asset-Kennung eines Klangs oder Klangsatzes; 0 heißt keiner.
    u64 sound = 0;
    AudioBus bus = AudioBus::Effects;
    f32 volume = 1.0f;
    f32 pitch = 1.0f;
    bool loop = false;
    // Spielt los, sobald die Quelle im Spiel erscheint.
    bool play_on_start = true;
    // Aus für Musik und Oberfläche: dann gleich laut auf beiden Seiten.
    bool spatial = true;
    // Bis hierhin volle Lautstärke, ab max_distance nicht mehr leiser (Meter).
    f32 min_distance = 1.0f;
    f32 max_distance = 40.0f;
    Rolloff rolloff = Rolloff::Inverse;
    f32 rolloff_factor = 1.0f;
    // 0 ohne Doppler, 1 physikalisch.
    f32 doppler = 1.0f;
};

// Hörer, meist an der Kamera. Bei mehreren gilt der mit der kleinsten Kennung.
struct AudioListener {
    // Lautstärke von allem, was dieser Hörer hört.
    f32 volume = 1.0f;
};

// Registriert die Typen des Audios (nach world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::audio
