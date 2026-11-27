module;

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>

// Spielfigur in Sandbox und Editor (R4.3): Steuerung aus den Aktionen, Kamera in Ich- und
// Schulterperspektive und Graben mit dem Pinsel aus R3.4. Die Bewegung selbst rechnet die Physik
// (physics::CharacterController); hier entstehen nur die Wünsche (CharacterMotion) und die
// Kamera. In R4.4 übernimmt ein C#-Skript die Steuerung; bis dahin teilen Sandbox und Editor diesen
// Code.
export module app.player;

import ngine.core;
import ngine.input;
import ngine.math;
import ngine.physics;
import ngine.reflect;
import ngine.voxel;
import ngine.world;

namespace app {

using namespace ngine;
using math::DVec3;
using math::Vec3;

export enum class CameraView : u8 { FirstPerson, Shoulder };

// Markiert die Figur, die der Spieler steuert, und hält ihren Blick. Gehört zur Szene (Prefab
// prefabs/spielfigur.prefab); yaw und pitch sind der Blick beim Speichern.
export struct PlayerInput {
    // Blick um die Hochachse und nach oben (+) bzw. unten, im Bogenmaß.
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    CameraView view = CameraView::FirstPerson;
    // Augenhöhe über dem Fußpunkt, stehend; geduckt im selben Verhältnis wie die Kapsel.
    f32 eye_height = 1.6f;
    // Schulterkamera: Abstand hinter und neben dem Auge.
    f32 shoulder_distance = 3.0f;
    f32 shoulder_side = 0.5f;
    // Graben: Radius des Pinsels und Reichweite in Metern.
    f32 dig_radius = 0.8f;
    f32 dig_reach = 6.0f;
    // Empfindlichkeit des Blicks (Bogenmaß pro Pixel der Maus).
    f32 look_speed = 0.0025f;
};

export void register_player_types(reflect::TypeRegistry& types) {
    if (types.find<CameraView>() == nullptr) {
        types.add<CameraView>("CameraView")
            .value("FirstPerson", CameraView::FirstPerson)
            .value("Shoulder", CameraView::Shoulder);
    }
    if (types.find<PlayerInput>() == nullptr) {
        // Blick und Kamera gehen an die Clients (R5.2): Der Client startet mit dem Blick seiner
        // Figur und stellt die Kamera wie der Server.
        types.add<PlayerInput>("PlayerInput")
            .replicated()
            .field("yaw", &PlayerInput::yaw)
            .hint(reflect::Hint::Angle)
            .field("pitch", &PlayerInput::pitch)
            .hint(reflect::Hint::Angle)
            .range(-1.55, 1.55)
            .field("view", &PlayerInput::view)
            .field("eye_height", &PlayerInput::eye_height)
            .range(0.1, 10.0, 0.01)
            .field("shoulder_distance", &PlayerInput::shoulder_distance)
            .range(0.0, 20.0, 0.05)
            .field("shoulder_side", &PlayerInput::shoulder_side)
            .range(-5.0, 5.0, 0.05)
            .field("dig_radius", &PlayerInput::dig_radius)
            .range(0.1, 10.0, 0.05)
            .local()
            .field("dig_reach", &PlayerInput::dig_reach)
            .range(0.5, 50.0, 0.1)
            .local()
            .field("look_speed", &PlayerInput::look_speed)
            .range(0.0001, 0.05, 0.0001)
            .local();
    }
}

// Eingaben eines Bildes für die Figur, unabhängig von der Plattformschicht.
export struct PlayerControls {
    // Rechts (+) und vor (+), -1 bis 1.
    f32 right = 0.0f;
    f32 forward = 0.0f;
    // Drehung seit dem letzten Bild in Pixeln der Maus (der Stick liefert dasselbe Maß).
    f32 look_x = 0.0f;
    f32 look_y = 0.0f;
    bool run = false;
    bool crouch = false;
    // Flanken dieses Bildes.
    bool jump = false;
    bool switch_view = false;
    // Gehalten.
    bool dig = false;
};

// Liest die Aktionen move, look, fast, crouch, jump, camera und dig (Belegung in
// assets/eingabe/standard.input).
export PlayerControls player_controls(const input::ActionState& actions) {
    const input::ActionValue& move = actions.value("move");
    const input::ActionValue& look = actions.value("look");
    return PlayerControls{.right = move.x,
                          .forward = move.y,
                          .look_x = look.x,
                          .look_y = look.y,
                          .run = actions.down("fast"),
                          .crouch = actions.down("crouch"),
                          .jump = actions.pressed("jump"),
                          .switch_view = actions.pressed("camera"),
                          .dig = actions.down("dig")};
}

// Dasselbe aus den Werten eines Befehls (R5.3) in der Reihenfolge der Belegung map. Ein Befehl hat
// keine Mausbewegung, der Blick steht in ihm selbst.
export PlayerControls player_controls(const input::ActionMap& map,
                                      std::span<const input::ActionValue> values) {
    const auto value = [&](std::string_view name) {
        const auto index = map.find(name);
        return index && *index < values.size() ? values[*index] : input::ActionValue{};
    };
    const input::ActionValue move = value("move");
    return PlayerControls{.right = move.x,
                          .forward = move.y,
                          .run = value("fast").down,
                          .crouch = value("crouch").down,
                          .jump = value("jump").pressed,
                          .switch_view = value("camera").pressed,
                          .dig = value("dig").down};
}

// Erste Figur mit PlayerInput und CharacterController (kleinste Kennung), sonst eine leere Entity.
export world::Entity find_player(world::World& world) {
    u64 best = 0;
    world.each<const PlayerInput, const physics::CharacterController>(
        [&](world::Entity entity, const PlayerInput&, const physics::CharacterController&) {
            if (best == 0 || entity.id() < best) {
                best = entity.id();
            }
        });
    return best != 0 ? world.entity(best) : world::Entity{};
}

[[nodiscard]] math::Quat yaw_rotation(f32 yaw) {
    return math::angle_axis(yaw, {0.0f, 1.0f, 0.0f});
}

// Blickrichtung aus yaw und pitch (−z ist vorn).
export math::Quat view_rotation(const PlayerInput& player) {
    return yaw_rotation(player.yaw) * math::angle_axis(player.pitch, {1.0f, 0.0f, 0.0f});
}

export void apply_motion(world::Entity player, f32 yaw, const PlayerControls& controls);

// Überträgt die Eingaben eines Bildes auf die Figur: Blick, Drehung um die Hochachse, Wünsche an
// die Physik. Springen bleibt gesetzt, bis die Physik es im nächsten festen Schritt verbraucht.
export void apply_controls(world::Entity player, const PlayerControls& controls) {
    auto* input = player.get_mut<PlayerInput>();
    if (input == nullptr) {
        return;
    }
    input->yaw -= controls.look_x * input->look_speed;
    input->pitch = std::clamp(input->pitch - controls.look_y * input->look_speed, -1.55f, 1.55f);
    if (controls.switch_view) {
        input->view =
            input->view == CameraView::FirstPerson ? CameraView::Shoulder : CameraView::FirstPerson;
    }
    if (auto* transform = player.get_mut<world::Transform>()) {
        transform->rotation = yaw_rotation(input->yaw);
    }
    apply_motion(player, input->yaw, controls);
}

// Nur die Wünsche an die Physik aus Blickrichtung yaw und controls, ohne Blick und Drehung zu
// ändern (Vorhersage, R5.3: beim Neurechnen gemerkter Befehle).
export void apply_motion(world::Entity player, f32 yaw, const PlayerControls& controls) {
    auto* motion = player.get_mut<physics::CharacterMotion>();
    if (motion == nullptr) {
        // Die Physik legt den Zustand im ersten Schritt an.
        return;
    }
    const math::Quat facing = yaw_rotation(yaw);
    const Vec3 forward = facing * Vec3{0.0f, 0.0f, -1.0f};
    const Vec3 right = facing * Vec3{1.0f, 0.0f, 0.0f};
    Vec3 move = forward * controls.forward + right * controls.right;
    move.y = 0.0f;
    if (math::length(move) > 1.0f) {
        move = math::normalize(move);
    }
    motion->move = move;
    motion->run = controls.run;
    motion->crouch = controls.crouch;
    motion->jump = motion->jump || controls.jump;
}

// Lage der Kamera einer Figur.
export struct PlayerView {
    DVec3 position{0.0, 0.0, 0.0};
    math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    // Auge der Figur (Ausgang für Strahlen wie das Graben).
    DVec3 eye{0.0, 0.0, 0.0};
};

// Kamera aus der gezeichneten Lage der Figur (WorldTransform, interpoliert). Die Schulterkamera
// rückt vor Wänden heran.
export PlayerView player_view(world::Entity player, const physics::PhysicsWorld& physics) {
    PlayerView view;
    const auto* input = player.get<PlayerInput>();
    const auto* transform = player.get<world::WorldTransform>();
    if (input == nullptr || transform == nullptr) {
        return view;
    }
    f32 eye_height = input->eye_height;
    const auto* controller = player.get<physics::CharacterController>();
    const auto* motion = player.get<physics::CharacterMotion>();
    if (controller != nullptr && motion != nullptr && motion->crouched && controller->height > 0) {
        eye_height *= controller->crouch_height / controller->height;
    }
    view.eye = transform->position() + DVec3{0.0, static_cast<f64>(eye_height), 0.0};
    view.rotation = view_rotation(*input);
    view.position = view.eye;
    if (input->view == CameraView::Shoulder) {
        const Vec3 back = view.rotation * Vec3{0.0f, 0.0f, 1.0f};
        const Vec3 side = view.rotation * Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 offset = back * input->shoulder_distance + side * input->shoulder_side;
        f32 distance = math::length(offset);
        if (distance > 0.0f) {
            const Vec3 direction = offset * (1.0f / distance);
            if (const auto hit = physics.raycast(view.eye, direction, distance + 0.2f,
                                                 {.ignore = player.id()})) {
                distance = std::max(hit->distance - 0.2f, 0.0f);
            }
            view.position = view.eye + math::to_f64(direction * distance);
        }
    }
    return view;
}

// Was das Graben treffen würde: Punkt auf einem Terrain entlang des Blicks aus dem Auge.
export struct DigTarget {
    world::Entity terrain;
    DVec3 point{0.0, 0.0, 0.0};
};

export std::optional<DigTarget> dig_target(world::World& world, world::Entity player,
                                           const PlayerView& view,
                                           const physics::PhysicsWorld& physics) {
    const auto* input = player.get<PlayerInput>();
    if (input == nullptr) {
        return std::nullopt;
    }
    const Vec3 forward = view.rotation * Vec3{0.0f, 0.0f, -1.0f};
    // Von der Kamera aus, damit das Ziel in der Bildmitte liegt; die Reichweite zählt ab dem Auge.
    const f32 behind = static_cast<f32>(math::length(view.position - view.eye));
    const auto hit =
        physics.raycast(view.position, forward, input->dig_reach + behind, {.ignore = player.id()});
    if (!hit) {
        return std::nullopt;
    }
    const world::Entity entity = world.entity(hit->entity);
    if (!entity || !entity.has<world::Terrain>()) {
        return std::nullopt;
    }
    return DigTarget{.terrain = entity, .point = hit->point};
}

// Ziel des Grabens wie im Skript Spielersteuerung (R5.4): Strahl aus dem Auge der Figur (Transform,
// nicht die gezeichnete Lage) in ihrem Blick, so lang wie dig_reach. So graben Server und
// Vorhersage des Clients nach demselben Befehl an derselben Stelle; die Kamera zählt nicht.
export std::optional<DigTarget> figure_dig_target(world::World& world, world::Entity player,
                                                  const physics::PhysicsWorld& physics) {
    const auto* input = player.get<PlayerInput>();
    const auto* transform = player.get<world::Transform>();
    if (input == nullptr || transform == nullptr) {
        return std::nullopt;
    }
    f32 eye = input->eye_height;
    const auto* controller = player.get<physics::CharacterController>();
    const auto* motion = player.get<physics::CharacterMotion>();
    if (controller != nullptr && motion != nullptr && motion->crouched && controller->height > 0) {
        eye *= controller->crouch_height / controller->height;
    }
    const DVec3 origin = transform->position + DVec3{0.0, static_cast<f64>(eye), 0.0};
    const Vec3 forward = view_rotation(*input) * Vec3{0.0f, 0.0f, -1.0f};
    const auto hit = physics.raycast(origin, forward, input->dig_reach, {.ignore = player.id()});
    if (!hit) {
        return std::nullopt;
    }
    const world::Entity entity = world.entity(hit->entity);
    if (!entity || !entity.has<world::Terrain>()) {
        return std::nullopt;
    }
    return DigTarget{.terrain = entity, .point = hit->point};
}

// Pinsel am Ziel: Kugel mit dig_radius abtragen (oder mit add Erde auftragen).
export voxel::Brush dig_brush(const DigTarget& target, const PlayerInput& input, bool add) {
    const DVec3 center = world::terrain_voxels(target.terrain, target.point);
    return voxel::Brush{
        .mode = add ? voxel::BrushMode::Add : voxel::BrushMode::Remove,
        .center = {static_cast<f32>(center.x), static_cast<f32>(center.y),
                   static_cast<f32>(center.z)},
        .radius = static_cast<f32>(static_cast<f64>(input.dig_radius) / voxel::voxel_size),
        .material = voxel::earth};
}

// Trägt am Ziel eine Kugel ab (oder mit add auf).
export void dig(const DigTarget& target, const PlayerInput& input, bool add) {
    (void)world::edit_terrain(target.terrain, dig_brush(target, input, add));
}

// Takt des Grabens bei gehaltener Taste, wie Grabpause im Skript Spielersteuerung: nach einem
// Pinselstrich pause Sekunden nichts. Gezählt wird in f32 wie im Skript, damit Server und
// Vorhersage im selben Befehl graben (R5.4).
export struct DigTimer {
    f32 pause = 0.15f;
    f32 cooldown = 0.0f;

    // Einmal pro Bild bzw. festem Schritt: ob jetzt gegraben werden darf.
    [[nodiscard]] bool ready(f32 dt, bool down) {
        cooldown = std::max(cooldown - dt, 0.0f);
        return down && cooldown <= 0.0f;
    }
    // Nach einem Pinselstrich, der etwas getroffen hat.
    void used() {
        cooldown = pause;
    }
};

} // namespace app
