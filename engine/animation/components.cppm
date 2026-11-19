module;

#include <string>

export module ngine.animation:components;

import ngine.core;
import ngine.reflect;

export namespace ngine::animation {

// Wann das AnimationSystem Posen rechnet. Der Automat läuft immer, damit Zustand und Ereignisse auf
// Server und Client gleich sind.
enum class PoseMode : u8 {
    // Nur, wenn ein Renderer sie braucht (nicht auf dem Server und mit --headless).
    Visible,
    // Immer, auch auf dem Server (z. B. später für Trefferzonen an Gelenken).
    Always,
    // Nie: nur der Automat.
    Never,
};

// Spielt einen Zustandsautomaten (.animgraph) auf dem Skelett seines Modells (ANIM-02). Netze mit
// Skin an dieser Entity oder an ihren Kindern werden mit der Pose verformt, wenn ihr Modell das
// des Automaten ist. Parameter beginnen mit den Werten aus der Datei; Skripte setzen sie.
struct Animator {
    // Asset-Kennung des Automaten; 0 heißt keiner.
    u64 graph = 0;
    // Faktor auf die Zeit.
    f32 speed = 1.0f;
    bool active = true;
    PoseMode pose_mode = PoseMode::Visible;
};

// Ereignis aus einem Zustand (z. B. ein Fuß setzt auf), über den EventBus nach update(), nach
// Entity sortiert.
struct AnimationEvent {
    u64 entity = 0;
    std::string name;
};

// Registriert die Typen der Animation (nach world::register_types()).
void register_types(reflect::TypeRegistry& types);

} // namespace ngine::animation
