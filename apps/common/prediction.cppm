module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Vorhersage der eigenen Figur auf dem Client (R5.3, NET-05, ADR 0053). Hängt sich in die feste
// Simulation einer app::GameWorld, deren Physik im Modus remote rechnet:
//
// - Vor der Physik wird aus den Aktionen (fixed_values()) und dem Blick der Figur ein nummerierter
//   Befehl, und apply_motion() setzt daraus die Wünsche an die Physik, wie auf dem Server das
//   Skript Spielersteuerung (ohne .NET derselbe C++-Code).
// - Nach dem Schritt merkt sich replication::Predictor Befehl und Zustand; die neuesten drei
// Befehle
//   gehen an den Server.
// - Meldet der Server den Stand nach einem Befehl (OwnerState), vergleicht der Predictor und
// rechnet
//   bei Abweichung ab dort mit PhysicsWorld::step_character() neu. Den Sprung in der Darstellung
//   baut ein Versatz in world::Interpolated weich ab.
// - Nach der Länge der Warteschlange beim Server stellt die Vorhersage den Takt ihrer festen
//   Schritte um bis zu ±5 % nach.
// - Die Parameter der Animation der eigenen Figur setzt der Client selbst (wie das Skript).
// - Seit R5.4 gräbt die Vorhersage nach jedem Schritt mit dig wie der Server (DigTimer,
//   figure_dig_target()) und hält den Pinsel über replication::TerrainReceiver vorhergesagt, bis
//   der Server den Befehl bestätigt. Solange die Deltas um die Figur nicht vollständig da sind,
//   wartet die Vorhersage.
//
// Sandbox mit Fenster, Client ohne Fenster und Tests nutzen sie gleich.
export module app.prediction;

import ngine.core;
import ngine.animation;
import ngine.input;
import ngine.math;
import ngine.physics;
import ngine.reflect;
import ngine.replication;
import ngine.world;
import app.game_world;
import app.player;

namespace app {

using namespace ngine;

// Grenze des Blicks nach oben und unten, wie auf dem Server.
export constexpr f32 max_pitch = 1.55f;

export struct PredictionSettings {
    replication::PredictionSettings predictor;
    // Halbwertszeit, mit der eine Korrektur in der Darstellung abklingt (s).
    f32 smoothing_half_life = 0.06f;
    // Größere Sprünge setzt die Darstellung sofort (m).
    f32 snap_distance = 2.0f;
    // So viele Befehle sollen beim Server warten.
    f32 target_queue = 2.0f;
    // Größte Abweichung des Takts vom Normalen.
    f32 max_rate_change = 0.05f;
};

// Korrekturen nach Ursache (R5.3, Abnahme): Bodies, wenn die Figur auf Client oder Server kurz
// vorher einen beweglichen Körper oder eine andere Figur berührte (Kisten, Kugeln: erwartet, weil
// fremde Körper auf dem Client in der Vergangenheit stehen); sonst Air, wenn sie auf einer Seite in
// der Luft war; sonst Free. Free sollte nahe null sein, sonst rechnen Client und Server
// verschieden. Bis die Figur auf beiden Seiten zum ersten Mal steht (Laden der Welt, Fallen auf
// den Boden), zählt jede Korrektur als loading und nicht pro Minute.
export struct CorrectionCounts {
    u64 free = 0;
    u64 bodies = 0;
    u64 air = 0;
    u64 loading = 0;
};

export struct PredictionReport {
    replication::PredictionStats predictor;
    // Korrekturen in der letzten Minute, insgesamt und nach Ursache.
    u32 corrections_per_minute = 0;
    CorrectionCounts minute;
    // Alle Korrekturen nach Ursache.
    CorrectionCounts causes;
    // Befehle, die beim Server zuletzt warteten, und deren gleitendes Mittel.
    u32 server_queue = 0;
    f32 server_queue_average = 0.0f;
    // Takt der festen Schritte (1 normal).
    f32 rate = 1.0f;
    // Länge des Versatzes der Darstellung (m).
    f32 smoothing = 0.0f;
    // Gesendete Befehle.
    u64 commands = 0;
    // Zeit des letzten Neurechnens (ms).
    f64 replay_ms = 0.0;
    // Vorhergesagte Pinselstriche (R5.4).
    u64 digs = 0;
    // Die Vorhersage wartet auf die Deltas um die Figur (R5.4).
    bool waiting_for_terrain = false;
};

// Eine Zeile für das Log.
export std::string describe_prediction(const PredictionReport& report) {
    const replication::PredictionStats& p = report.predictor;
    return std::format(
        "Vorhersage: {} Korrekturen ({} in der letzten Minute, zuletzt {:.1f} cm), "
        "{} Schritte neu gerechnet ({:.2f} ms), letzte Abweichung {:.2f} cm, {} "
        "Befehle offen, {} gesendet, beim Server {} (Mittel {:.1f}), Takt {:+.1f} %, "
        "Versatz {:.1f} cm; in der letzten Minute {} frei, {} an Körpern, {} in der Luft; {} "
        "Pinsel{}",
        p.corrections, report.corrections_per_minute, p.last_correction * 100.0, p.replayed,
        report.replay_ms, p.last_error * 100.0, p.pending, report.commands, report.server_queue,
        report.server_queue_average, (report.rate - 1.0f) * 100.0f, report.smoothing * 100.0f,
        report.minute.free, report.minute.bodies, report.minute.air, report.digs,
        report.waiting_for_terrain ? "; wartet auf das Terrain" : "");
}

export class PlayerPrediction {
public:
    using Send = std::function<void(std::span<const std::byte>)>;

    // game und actions müssen die Vorhersage überleben. send schickt eine Nachricht des Spiels an
    // den Server (unzuverlässig). Die Physik von game rechnet ab jetzt im Modus remote.
    PlayerPrediction(GameWorld& game, input::ActionState& actions, Send send,
                     PredictionSettings settings = {})
        : game_{&game}, actions_{&actions}, send_{std::move(send)}, settings_{settings},
          predictor_{settings.predictor}, map_hash_{actions.map().hash()} {
        game.physics().set_remote(true);
        game.set_fixed_hooks([this] { before_physics(); }, [this] { after_step(); });
    }
    ~PlayerPrediction() {
        game_->set_fixed_hooks({}, {});
        game_->simulation().set_time_scale(1.0);
    }
    PlayerPrediction(const PlayerPrediction&) = delete;
    PlayerPrediction& operator=(const PlayerPrediction&) = delete;

    // Deltas des Terrains vom Server (R5.4): Graben wird darüber vorhergesagt, und die Vorhersage
    // wartet, bis die Umgebung der Figur vollständig ist. nullptr: Graben nur lokal, kein Warten.
    void set_terrain(replication::TerrainReceiver* terrain) noexcept {
        terrain_ = terrain;
    }

    // Einmal pro Bild vor der Simulation: die eigene Figur in der Welt des Clients (lokale
    // Kennung, 0 solange unbekannt) und ihre Kennung auf dem Server.
    void set_figure(u64 local, u64 server_id, replication::Mirror& mirror) {
        world::World& world = game_->world();
        if (local != 0 && !world.alive(local)) {
            local = 0;
        }
        // Beitritt (R5.4): Ohne die Deltas um die Figur stünde sie auf Boden, den der Server
        // vielleicht längst weggegraben hat. Bis dahin zeigt der Client sie wie fremde Figuren.
        if (local != 0 && figure_ == 0 && !terrain_ready()) {
            local = 0;
        }
        if (local == figure_ && (local == 0 || world.entity(local).has<physics::Predicted>())) {
            return;
        }
        if (figure_ != 0 && world.alive(figure_)) {
            world.entity(figure_).remove<physics::Predicted>();
        }
        if (animated_ != 0) {
            game_->animation().set_local(animated_, false);
        }
        figure_ = 0;
        animated_ = 0;
        predictor_.reset();
        outgoing_.clear();
        contacts_.clear();
        settled_ = false;
        world::Entity entity = world.entity(local);
        if (local == 0 || !entity.has<physics::CharacterController>() ||
            !entity.has<world::Transform>() || !entity.has<PlayerInput>()) {
            // Die Komponenten kommen mit dem nächsten Schnappschuss.
            mirror.set_predicted(server_id, {});
            return;
        }
        figure_ = local;
        entity.set(physics::Predicted{});
        if (!entity.has<world::TerrainObserver>()) {
            entity.set(world::TerrainObserver{});
        }
        const reflect::TypeRegistry& types = game_->types();
        const std::array<const reflect::TypeInfo*, 3> owned{types.find<world::Transform>(),
                                                            types.find<physics::CharacterMotion>(),
                                                            types.find<PlayerInput>()};
        mirror.set_predicted(server_id, owned);
        // Animator an der Figur oder darunter.
        if (entity.has<animation::Animator>()) {
            animated_ = local;
        } else {
            for (const world::Entity child : entity.children()) {
                if (child.has<animation::Animator>()) {
                    animated_ = child.id();
                    break;
                }
            }
        }
        if (animated_ != 0) {
            game_->animation().set_local(animated_, true);
        }
        const auto* input = entity.get<PlayerInput>();
        log::info("Vorhersage: eigene Figur {} (Blick {:.2f})", entity.name(), input->yaw);
    }

    // Einmal pro Bild: Blick und Kamera aus den Aktionen, wie im Spiel (Maus, Stick).
    void look(const input::ActionState& actions) {
        world::Entity own = game_->world().entity(figure_);
        auto* input = own_alive() ? own.get_mut<PlayerInput>() : nullptr;
        if (input == nullptr) {
            return;
        }
        const input::ActionValue& look = actions.value("look");
        input->yaw -= look.x * input->look_speed;
        input->pitch = std::clamp(input->pitch - look.y * input->look_speed, -max_pitch, max_pitch);
        if (actions.pressed("camera")) {
            input->view = input->view == CameraView::FirstPerson ? CameraView::Shoulder
                                                                 : CameraView::FirstPerson;
        }
        if (auto* transform = own.get_mut<world::Transform>()) {
            transform->rotation = math::angle_axis(input->yaw, {0.0f, 1.0f, 0.0f});
            own.modified<world::Transform>();
        }
    }

    // Nachricht des Servers zur eigenen Figur.
    void handle_owner(const replication::OwnerState& owner) {
        server_queue_ = owner.queued;
        queue_average_ += (static_cast<f32>(owner.queued) - queue_average_) * 0.1f;
        const f32 error = settings_.target_queue - queue_average_;
        rate_ =
            1.0f + std::clamp(error * 0.02f, -settings_.max_rate_change, settings_.max_rate_change);
        game_->simulation().set_time_scale(rate_);
        if (!own_alive()) {
            return;
        }
        // Geladen ist, sobald die Figur auf Server und Client zum ersten Mal steht.
        if (!settled_) {
            const auto air = static_cast<u8>(physics::GroundState::Air);
            const auto latest = predictor_.latest();
            settled_ = owner.ground != air && latest && latest->ground != air;
        }
        physics::PhysicsWorld& physics = game_->physics();
        const auto before = physics.character_snapshot(figure_);
        const replication::PredictedState server{.position = owner.position,
                                                 .velocity = owner.velocity,
                                                 .ground = owner.ground,
                                                 .crouched = owner.crouched,
                                                 .touching = owner.touching};
        const std::optional<replication::PredictedState> mine =
            predictor_.predicted(owner.command);
        const auto start = std::chrono::steady_clock::now();
        const f32 dt = game_->world().time().fixed_delta;
        const bool corrected = predictor_.reconcile(
            owner.command, server,
            [&](const replication::PredictedState& state) {
                (void)physics.set_character(figure_, to_snapshot(state));
            },
            [&](const replication::Command& command) { return replay(command, dt); });
        if (!corrected) {
            return;
        }
        replay_ms_ =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        const Cause cause = settled_ ? classify(owner.command, server, mine) : Cause::Loading;
        if (cause != Cause::Loading) {
            corrections_.emplace_back(std::chrono::steady_clock::now(), cause);
        }
        count(causes_, cause);
        if (cause == Cause::Free && free_logged_ < 5 && mine) {
            ++free_logged_;
            log::info("Vorhersage: Korrektur ohne Kontakt nach Befehl {}: {:.1f} cm, Geschwindigkeit "
                      "Server ({:.2f}, {:.2f}, {:.2f}), Client ({:.2f}, {:.2f}, {:.2f}), Boden {}/{}, "
                      "geduckt {}/{}",
                      owner.command, replication::position_error(*mine, server) * 100.0,
                      server.velocity.x, server.velocity.y, server.velocity.z, mine->velocity.x,
                      mine->velocity.y, mine->velocity.z, static_cast<int>(server.ground),
                      static_cast<int>(mine->ground),
                      server.crouched, mine->crouched);
        }
        const auto after = physics.character_snapshot(figure_);
        if (!before || !after) {
            return;
        }
        // Darstellung: dort weiter, wo sie war, und den Unterschied abklingen lassen.
        const math::DVec3 jump = after->position - before->position;
        auto* interpolated = game_->world().entity(figure_).get_mut<world::Interpolated>();
        if (interpolated == nullptr) {
            return;
        }
        const math::Vec3 offset =
            interpolated->offset - math::Vec3{static_cast<f32>(jump.x), static_cast<f32>(jump.y),
                                              static_cast<f32>(jump.z)};
        if (math::length(offset) > settings_.snap_distance) {
            interpolated->offset = {};
        } else {
            interpolated->offset = offset;
            interpolated->previous.position = interpolated->previous.position + jump;
        }
    }

    // Einmal pro Bild nach der Simulation: Versatz abbauen, Animation der eigenen Figur.
    void update(f32 dt) {
        world::World& world = game_->world();
        if (figure_ == 0 || !world.alive(figure_)) {
            return;
        }
        world::Entity own = world.entity(figure_);
        if (auto* interpolated = own.get_mut<world::Interpolated>()) {
            const f32 half_life = std::max(settings_.smoothing_half_life, 0.001f);
            interpolated->offset = interpolated->offset * std::exp2(-dt / half_life);
            if (math::length(interpolated->offset) < 0.0005f) {
                interpolated->offset = {};
            }
            smoothing_ = math::length(interpolated->offset);
        }
        if (animated_ != 0) {
            if (const auto* motion = own.get<physics::CharacterMotion>()) {
                const f32 speed = std::sqrt(motion->velocity.x * motion->velocity.x +
                                            motion->velocity.z * motion->velocity.z);
                tempo_ += (speed - tempo_) * std::min(10.0f * dt, 1.0f);
                animation::AnimationSystem& animation = game_->animation();
                (void)animation.set_float(animated_, "Tempo", tempo_);
                (void)animation.set_bool(animated_, "AmBoden",
                                         motion->ground != physics::GroundState::Air);
                (void)animation.set_bool(animated_, "Geduckt", motion->crouched);
            }
        }
    }

    [[nodiscard]] u64 figure() const noexcept {
        return figure_;
    }
    // Die Umgebung der Figur ist da (oder es gibt kein Terrain bzw. keinen Empfänger).
    [[nodiscard]] bool terrain_ready() {
        if (terrain_ == nullptr || terrain_->complete()) {
            return true;
        }
        bool any = false;
        game_->world().each<const world::Terrain>(
            [&](world::Entity, const world::Terrain&) { any = true; });
        return !any;
    }
    // Die eigene Figur ist bekannt und lebt (der Spiegel kann sie zwischen zwei Bildern beenden).
    [[nodiscard]] bool own_alive() const {
        return figure_ != 0 && game_->world().alive(figure_);
    }
    [[nodiscard]] PredictionReport report() {
        const auto now = std::chrono::steady_clock::now();
        while (!corrections_.empty() &&
               now - corrections_.front().first > std::chrono::minutes{1}) {
            corrections_.pop_front();
        }
        CorrectionCounts minute;
        for (const auto& [time, cause] : corrections_) {
            count(minute, cause);
        }
        return PredictionReport{.predictor = predictor_.stats(),
                                .corrections_per_minute = static_cast<u32>(corrections_.size()),
                                .minute = minute,
                                .causes = causes_,
                                .server_queue = server_queue_,
                                .server_queue_average = queue_average_,
                                .rate = rate_,
                                .smoothing = smoothing_,
                                .commands = commands_,
                                .replay_ms = replay_ms_,
                                .digs = digs_,
                                .waiting_for_terrain = figure_ == 0 && !terrain_ready()};
    }

private:
    static physics::CharacterSnapshot to_snapshot(const replication::PredictedState& state) {
        return physics::CharacterSnapshot{.position = state.position,
                                          .velocity = state.velocity,
                                          .ground = static_cast<physics::GroundState>(state.ground),
                                          .crouched = state.crouched};
    }
    static replication::PredictedState to_state(const physics::CharacterSnapshot& snapshot) {
        return replication::PredictedState{.position = snapshot.position,
                                           .velocity = snapshot.velocity,
                                           .ground = static_cast<u8>(snapshot.ground),
                                           .crouched = snapshot.crouched,
                                           .touching = snapshot.touching};
    }

    enum class Cause : u8 { Free, Bodies, Air, Loading };

    static void count(CorrectionCounts& counts, Cause cause) {
        switch (cause) {
        case Cause::Free:
            ++counts.free;
            break;
        case Cause::Bodies:
            ++counts.bodies;
            break;
        case Cause::Air:
            ++counts.air;
            break;
        case Cause::Loading:
            ++counts.loading;
            break;
        }
    }

    // Ursache einer Korrektur nach Befehl number: Berührung in den 15 Schritten davor (Client)
    // oder im Stand des Servers, sonst Luft auf einer Seite, sonst frei.
    [[nodiscard]] Cause classify(u32 number, const replication::PredictedState& server,
                                 const std::optional<replication::PredictedState>& mine) const {
        bool touched = server.touching || (mine && mine->touching);
        for (const auto& [command, touching] : contacts_) {
            if (touching && command <= number && command + 15 >= number) {
                touched = true;
                break;
            }
        }
        if (touched) {
            return Cause::Bodies;
        }
        const auto air = static_cast<u8>(physics::GroundState::Air);
        if (server.ground == air || (mine && mine->ground == air)) {
            return Cause::Air;
        }
        return Cause::Free;
    }

    // Wünsche an die Physik aus einem Befehl; Kamera und Graben zählen hier nicht.
    void apply(const replication::Command& command) {
        PlayerControls controls = player_controls(actions_->map(), command.actions);
        controls.switch_view = false;
        controls.dig = false;
        apply_motion(game_->world().entity(figure_), command.yaw, controls);
    }

    replication::PredictedState replay(const replication::Command& command, f32 dt) {
        apply(command);
        physics::PhysicsWorld& physics = game_->physics();
        (void)physics.step_character(figure_, dt);
        const auto snapshot = physics.character_snapshot(figure_);
        return snapshot ? to_state(*snapshot) : replication::PredictedState{};
    }

    void before_physics() {
        pending_.reset();
        world::World& world = game_->world();
        if (figure_ == 0 || !world.alive(figure_)) {
            return;
        }
        const auto* input = world.entity(figure_).get<PlayerInput>();
        if (input == nullptr) {
            return;
        }
        replication::Command command;
        command.number = std::max(predictor_.next_number(), next_number_);
        command.yaw = input->yaw;
        command.pitch = input->pitch;
        const std::span<const input::ActionValue> values = actions_->fixed_values();
        command.actions.assign(values.begin(), values.end());
        const input::ActionMap& map = actions_->map();
        if (const auto look = map.find("look"); look && *look < command.actions.size()) {
            command.actions[*look].x = 0.0f;
            command.actions[*look].y = 0.0f;
        }
        // Wie auf dem Server: Werte in den Grenzen der Belegung, -0 wird 0 (wie über die Leitung).
        (void)replication::limit_command(command, map, max_pitch);
        for (input::ActionValue& value : command.actions) {
            if (value.x == 0.0f && value.y == 0.0f) {
                value.x = 0.0f;
                value.y = 0.0f;
            }
        }
        next_number_ = command.number + 1;
        apply(command);
        if (animated_ != 0) {
            const auto* motion = world.entity(figure_).get<physics::CharacterMotion>();
            const auto jump = map.find("jump");
            if (motion != nullptr && motion->ground != physics::GroundState::Air && jump &&
                *jump < command.actions.size() && command.actions[*jump].pressed) {
                (void)game_->animation().set_trigger(animated_, "Springen");
            }
        }
        pending_ = std::move(command);
    }

    void after_step() {
        if (pending_) {
            if (const auto snapshot = game_->physics().character_snapshot(figure_)) {
                predictor_.record(*pending_, to_state(*snapshot));
                contacts_.emplace_back(pending_->number, snapshot->touching);
                while (contacts_.size() > 64) {
                    contacts_.pop_front();
                }
            }
            predict_dig(*pending_);
            outgoing_.push_back(std::move(*pending_));
            while (outgoing_.size() > replication::commands_per_message) {
                outgoing_.pop_front();
            }
            const std::vector<replication::Command> sent(outgoing_.begin(), outgoing_.end());
            if (send_) {
                send_(replication::encode_commands(map_hash_, sent));
            }
            ++commands_;
            pending_.reset();
        }
        actions_->end_fixed_step();
    }

    // Graben wie auf dem Server nach dem Schritt des Befehls (R5.4): mit dig im Takt der
    // Grabpause, mit fast auftragen. Ohne Empfänger gräbt es nur lokal.
    void predict_dig(const replication::Command& command) {
        const PlayerControls controls = player_controls(actions_->map(), command.actions);
        const f32 dt = game_->world().time().fixed_delta;
        if (!dig_timer_.ready(dt, controls.dig)) {
            return;
        }
        world::World& world = game_->world();
        const world::Entity own = world.entity(figure_);
        const auto* input = own.get<PlayerInput>();
        const auto target = figure_dig_target(world, own, game_->physics());
        if (input == nullptr || !target) {
            return;
        }
        const voxel::Brush brush = dig_brush(*target, *input, controls.run);
        if (terrain_ != nullptr) {
            terrain_->predict(target->terrain, command.number, brush);
        } else {
            (void)world::edit_terrain(target->terrain, brush);
        }
        dig_timer_.used();
        ++digs_;
    }

    GameWorld* game_;
    replication::TerrainReceiver* terrain_ = nullptr;
    DigTimer dig_timer_;
    u64 digs_ = 0;
    input::ActionState* actions_;
    Send send_;
    PredictionSettings settings_;
    replication::Predictor predictor_;
    u32 map_hash_ = 0;
    u64 figure_ = 0;
    u64 animated_ = 0;
    u32 next_number_ = 1;
    std::optional<replication::Command> pending_;
    std::deque<replication::Command> outgoing_;
    u32 server_queue_ = 0;
    f32 queue_average_ = 2.0f;
    f32 rate_ = 1.0f;
    f32 smoothing_ = 0.0f;
    f32 tempo_ = 0.0f;
    u64 commands_ = 0;
    f64 replay_ms_ = 0.0;
    std::deque<std::pair<std::chrono::steady_clock::time_point, Cause>> corrections_;
    CorrectionCounts causes_;
    std::deque<std::pair<u32, bool>> contacts_;
    u32 free_logged_ = 0;
    bool settled_ = false;
};

} // namespace app
