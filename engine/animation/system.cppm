module;

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.animation:system;

import ngine.core;
import ngine.math;
import ngine.jobs;
import ngine.asset;
import ngine.world;
import :rig;
import :graph;
import :machine;
import :components;

// Animation in der Welt (ANIM-01, ANIM-02): führt für jede Entity mit Animator einen
// Zustandsautomaten, rechnet in der Nachbereitung (Phase::Late, nach update_transforms()) Posen
// und Paletten in Jobs und schickt Ereignisse über den EventBus. Schreibt nichts in die Welt; die
// Prüfsumme der Simulation ändert es nicht.
export namespace ngine::animation {

struct AnimationStats {
    // Animatoren in der Welt, davon mit fertigem Automaten und davon mit Pose in diesem Bild.
    usize animators = 0;
    usize running = 0;
    usize posed = 0;
    // Warten auf Automat oder Modell (laden noch oder fehlen).
    usize waiting = 0;
    // Zeit des letzten update() in Millisekunden.
    f64 update_ms = 0.0;
};

// Was das Editor-Fenster „Animation“ über einen Animator zeigt.
// Wohin die Parameter der Automaten gehen (R5.2, AnimatorParameters).
enum class ParameterSync : u8 {
    // Nur im System (Spiel ohne Netz, Editor).
    None,
    // Nach jedem update() in AnimatorParameters schreiben (Server).
    Publish,
    // Vor jedem update() aus AnimatorParameters lesen (Client).
    Follow,
};

struct AnimatorView {
    const StateMachine* machine = nullptr;
    const Rig* rig = nullptr;
    asset::AssetId model = 0;
    bool posed = false;
    std::string error;
};

class AnimationSystem {
public:
    // jobs darf nullptr sein (alles im aufrufenden Thread). Manager und Jobs müssen das System
    // überleben. Lader für Rig und AnimGraph müssen registriert sein.
    explicit AnimationSystem(asset::ResourceManager& resources, jobs::JobSystem* jobs = nullptr);
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    void set_events(events::EventBus* bus) noexcept {
        events_ = bus;
    }
    // Ob ein Renderer Posen braucht (PoseMode::Visible). Aus auf dem Server und mit --headless.
    void set_rendering(bool rendering) noexcept {
        rendering_ = rendering;
    }
    [[nodiscard]] bool rendering() const noexcept {
        return rendering_;
    }
    void set_parameter_sync(ParameterSync sync) noexcept {
        sync_ = sync;
    }
    // Mit ParameterSync::Follow: Parameter dieses Animators setzt der Client selbst (eigene,
    // vorhergesagte Figur, R5.3) und übernimmt sie nicht aus AnimatorParameters.
    void set_local(u64 entity, bool local);

    // Einmal pro Bild nach update_transforms(); dt ist die Bildzeit in Sekunden.
    void update(world::World& world, f64 dt);

    // Parameter des Animators an entity. Vor dem ersten update() oder solange der Automat lädt,
    // merkt sich das System den Wert. Liefert false, wenn der fertige Automat ihn nicht kennt.
    bool set_float(u64 entity, std::string_view name, f32 value);
    bool set_bool(u64 entity, std::string_view name, bool value);
    bool set_trigger(u64 entity, std::string_view name);
    [[nodiscard]] std::optional<f32> parameter(u64 entity, std::string_view name) const;
    // Aktiver Zustand (leer, solange der Automat lädt).
    [[nodiscard]] std::string_view state(u64 entity) const;

    // Palette für ein Netz mit Skin skin aus dem Modell model an entity: vom Animator an entity
    // oder dem nächsten Vorfahren, dessen Automat dieses Modell spielt und in diesem Bild eine
    // Pose hat. Sonst leer (das Netz bleibt in der Bindepose).
    [[nodiscard]] std::span<const JointMatrix> palette(const world::World& world, u64 entity,
                                                       asset::AssetId model, u32 skin) const;
    // Lagen aller Knoten im Modellraum (für das Skelett im Debug-Zeichnen), leer ohne Pose.
    [[nodiscard]] std::span<const math::Mat4> model_space(u64 entity) const;

    [[nodiscard]] std::optional<AnimatorView> view(u64 entity) const;

    // Vergisst alle Automaten (z. B. beim Stoppen des Spielmodus).
    void clear();

    [[nodiscard]] const AnimationStats& stats() const noexcept {
        return stats_;
    }

private:
    struct Instance {
        u64 entity = 0;
        u64 graph_id = 0;
        asset::Handle<AnimGraph> graph;
        asset::Handle<Rig> rig;
        u32 graph_version = 0;
        u32 rig_version = 0;
        StateMachine machine;
        bool ready = false;
        std::string error;
        f32 speed = 1.0f;
        bool active = true;
        PoseMode mode = PoseMode::Visible;
        // Werte, die vor dem fertigen Automaten gesetzt wurden (Name, Wert, Auslöser).
        std::vector<std::pair<std::string, f32>> pending;
        Pose pose;
        std::vector<math::Mat4> model;
        std::vector<std::vector<JointMatrix>> palettes;
        bool posed = false;
        std::vector<const std::string*> fired;
        u64 seen = 0;
    };

    void prepare(Instance& instance);
    void run(Instance& instance, f32 dt) const;
    [[nodiscard]] Instance* find(u64 entity);
    [[nodiscard]] const Instance* find(u64 entity) const;

    asset::ResourceManager* resources_;
    jobs::JobSystem* jobs_;
    events::EventBus* events_ = nullptr;
    bool rendering_ = true;
    ParameterSync sync_ = ParameterSync::None;
    std::vector<u64> local_;
    std::map<u64, Instance> instances_;
    std::vector<Instance*> active_;
    // Automaten (Kennung, Stand von Automat und Rig), deren Warnungen schon im Log stehen.
    std::vector<std::pair<u64, u64>> warned_;
    u64 frame_ = 0;
    AnimationStats stats_;
};

} // namespace ngine::animation
