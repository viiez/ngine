#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

import ngine.core;
import ngine.jobs;
import ngine.math;
import ngine.reflect;
import ngine.world;
import ngine.edit;

// Gizmos, Spielen im Editor und Umhängen mit Weltlage (R2.3), alles ohne Fenster und GPU.

using namespace ngine;
using Catch::Approx;
using math::DVec3;
using math::Vec3;

namespace {

// Laufzeitzustand, den „Spielen“ verändert und „Stoppen“ zurückholen muss.
struct Spinner {
    f32 speed = 1.0f;
    f32 angle = 0.0f;
    u32 ticks = 0;
};

reflect::TypeRegistry make_types() {
    reflect::TypeRegistry types;
    world::register_types(types);
    types.add<Spinner>("Spinner")
        .field("speed", &Spinner::speed)
        .field("angle", &Spinner::angle)
        .field("ticks", &Spinner::ticks);
    return types;
}

struct Fixture {
    reflect::TypeRegistry types = make_types();
    world::World world{types};
    events::EventBus bus;
    edit::SceneDocument document{world, &bus};

    edit::EntityId add(std::string name, edit::EntityId parent = 0) {
        auto created = edit::create_entity(document, std::move(name), parent);
        REQUIRE(document.execute(std::move(created.command)));
        return created.id;
    }
    world::Entity entity(edit::EntityId id) {
        return world.entity(document.entity_of(id));
    }
    void set_transform(edit::EntityId id, const world::Transform& transform) {
        entity(id).set(transform);
    }
    const world::Transform& transform(edit::EntityId id) {
        return *entity(id).get<world::Transform>();
    }
};

// Kamera bei (0, 0, 10), Blick entlang -z auf den Ursprung, 800 × 600 Pixel.
edit::ViewCamera front_camera() {
    return edit::ViewCamera{.position = DVec3{0.0, 0.0, 10.0},
                            .rotation = math::Mat3{},
                            .fov_y = math::radians(60.0f),
                            .near_plane = 0.05f,
                            .width = 800.0f,
                            .height = 600.0f};
}

math::Vec2 pixel_of(const edit::ViewCamera& camera, DVec3 point) {
    const auto p = camera.project(point);
    REQUIRE(p.has_value());
    return *p;
}

// Ein Ziehen mit der Maus von from nach to in steps Frames, dann loslassen.
void drag(edit::Gizmo& gizmo, Fixture& f, edit::EntityId id, const edit::ViewCamera& camera,
          math::Vec2 from, math::Vec2 to, u32 steps = 8) {
    REQUIRE(
        gizmo.update(f.document, id, camera,
                     {.x = from.x, .y = from.y, .hovered = true, .pressed = true, .down = true}));
    REQUIRE(gizmo.dragging());
    for (u32 i = 1; i <= steps; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
        const math::Vec2 p = from + (to - from) * t;
        gizmo.update(f.document, id, camera, {.x = p.x, .y = p.y, .hovered = true, .down = true});
    }
    gizmo.update(f.document, id, camera, {.x = to.x, .y = to.y, .hovered = true, .down = false});
    REQUIRE_FALSE(gizmo.dragging());
}

std::vector<std::byte> world_bytes(Fixture& f) {
    // Alle Entities in der Reihenfolge der Welt mit ihren Komponenten im Binärformat.
    std::vector<std::byte> bytes;
    for (const u64 entity : f.world.entity_ids()) {
        auto visit = [&](const reflect::TypeInfo& type, world::ComponentId, const void* data) {
            const auto captured = edit::capture(data, type);
            bytes.insert(bytes.end(), captured.begin(), captured.end());
        };
        f.world.for_each_component(
            entity, world::ComponentVisitor{
                        .context = &visit,
                        .visit = [](void* context, const reflect::TypeInfo& type,
                                    world::ComponentId component, const void* data) {
                            (*static_cast<decltype(visit)*>(context))(type, component, data);
                        }});
    }
    return bytes;
}

} // namespace

TEST_CASE("View camera rays and projection agree", "[edit][gizmo]") {
    const edit::ViewCamera camera = front_camera();
    // Bildmitte blickt geradeaus.
    const math::Ray center = camera.ray(400.0f, 300.0f);
    CHECK(math::near(center.direction, DVec3{0.0, 0.0, -1.0}, 1e-6));
    // Ein Punkt rechts oben landet rechts oben im Bild, und der Strahl dorthin trifft ihn.
    const DVec3 point{2.0, 1.5, -3.0};
    const math::Vec2 pixel = pixel_of(camera, point);
    CHECK(pixel.x > 400.0f);
    CHECK(pixel.y < 300.0f);
    const math::Ray ray = camera.ray(pixel.x, pixel.y);
    const auto closest = math::closest_to_segment(ray, point, point);
    CHECK(closest.distance < 1e-4);
    // Hinter der Kamera gibt es kein Pixel.
    CHECK_FALSE(camera.project(DVec3{0.0, 0.0, 11.0}).has_value());
    // Ein Pixel in 10 m Entfernung: 2 · 10 · tan(30°) / 600.
    CHECK(camera.pixel_size(DVec3{}) == Approx(2.0 * 10.0 * std::tan(math::pi_f64 / 6.0) / 600.0));
}

TEST_CASE("Gizmo hit tests find axes, rings and the center", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId id = f.add("Kiste");
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    const auto frame = gizmo.frame(f.document, id, camera);
    REQUIRE(frame.has_value());
    CHECK(frame->size == Approx(camera.pixel_size(DVec3{}) * gizmo.size_pixels));

    const auto ray_at = [&](DVec3 point) {
        const math::Vec2 p = pixel_of(camera, point);
        return camera.ray(p.x, p.y);
    };
    // Verschieben: Mitte der x- und y-Achse; z zeigt auf die Kamera und ist nicht zu treffen.
    CHECK(gizmo.hit(*frame, ray_at(DVec3{frame->size * 0.6, 0.0, 0.0}), camera) ==
          edit::GizmoAxis::X);
    CHECK(gizmo.hit(*frame, ray_at(DVec3{0.0, frame->size * 0.6, 0.0}), camera) ==
          edit::GizmoAxis::Y);
    CHECK(gizmo.hit(*frame, ray_at(DVec3{frame->size * 0.6, frame->size * 0.6, 0.0}), camera) ==
          edit::GizmoAxis::None);

    // Drehen: Der Ring um z liegt in der Bildebene.
    gizmo.mode = edit::GizmoMode::Rotate;
    const f64 d = frame->size * std::sqrt(0.5);
    CHECK(gizmo.hit(*frame, ray_at(DVec3{d, d, 0.0}), camera) == edit::GizmoAxis::Z);
    CHECK(gizmo.hit(*frame, ray_at(DVec3{d * 0.5, d * 0.5, 0.0}), camera) == edit::GizmoAxis::None);

    // Skalieren: Kugel in der Mitte skaliert gleichmäßig.
    gizmo.mode = edit::GizmoMode::Scale;
    CHECK(gizmo.hit(*frame, ray_at(DVec3{}), camera) == edit::GizmoAxis::All);
    CHECK(gizmo.hit(*frame, ray_at(DVec3{frame->size * 0.9, 0.0, 0.0}), camera) ==
          edit::GizmoAxis::X);

    // Form: drei Achsen mit Pfeilen bzw. drei Ringe.
    gizmo.mode = edit::GizmoMode::Translate;
    CHECK(gizmo.shape(*frame).lines.size() == 3);
    CHECK(gizmo.shape(*frame).markers.size() == 3);
    gizmo.mode = edit::GizmoMode::Rotate;
    CHECK(gizmo.shape(*frame).lines.size() == 3 * 64);
}

TEST_CASE("Dragging the translate gizmo moves along the axis in one undo step", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId id = f.add("Kiste");
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    const auto frame = gizmo.frame(f.document, id, camera);
    REQUIRE(frame.has_value());

    const usize steps_before = f.document.history().size();
    const math::Vec2 from = pixel_of(camera, DVec3{frame->size * 0.6, 0.0, 0.0});
    const math::Vec2 to = pixel_of(camera, DVec3{frame->size * 0.6 + 2.0, 0.7, 0.0});
    drag(gizmo, f, id, camera, from, to);

    // Nur entlang x, die Bewegung der Maus nach oben zählt nicht. Ganz genau 2 m sind es wegen der
    // Perspektive nicht: Der Strahl zum Ziel läuft schräg an der Achse vorbei.
    CHECK(f.transform(id).position.x == Approx(2.0).margin(0.05));
    CHECK(f.transform(id).position.y == 0.0);
    CHECK(f.transform(id).position.z == 0.0);
    CHECK(f.document.history().size() == steps_before + 1);
    CHECK(f.document.history().undo_label() == "Verschieben");

    REQUIRE(f.document.undo());
    CHECK(f.transform(id).position.x == 0.0);
    REQUIRE(f.document.redo());
    CHECK(f.transform(id).position.x == Approx(2.0).margin(0.05));
}

TEST_CASE("Translate gizmo snaps to the world grid and works under a parent", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId parent = f.add("Eltern");
    const edit::EntityId id = f.add("Kind", parent);
    // Elternteil um 90° um y gedreht und doppelt so groß: Welt-x ist dort lokal +z/2.
    f.set_transform(parent,
                    world::Transform{.position = DVec3{0.0, 0.0, 0.0},
                                     .rotation = math::angle_axis(math::radians(90.0f), {0, 1, 0}),
                                     .scale = Vec3{2.0f, 2.0f, 2.0f}});
    f.set_transform(id, world::Transform{.position = DVec3{0.0, 0.0, 0.1}});
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    gizmo.snap.enabled = true;
    gizmo.snap.translate = 0.5;
    const auto frame = gizmo.frame(f.document, id, camera);
    REQUIRE(frame.has_value());
    // Kind liegt in der Welt bei x = 0.2.
    REQUIRE(frame->origin.x == Approx(0.2).margin(1e-6));

    const DVec3 start = frame->origin + DVec3{frame->size * 0.6, 0.0, 0.0};
    drag(gizmo, f, id, camera, pixel_of(camera, start),
         pixel_of(camera, start + DVec3{1.1, 0.0, 0.0}));
    // 0.2 + 1.1 = 1.3, eingerastet auf 1.5. Lokal: 1.3 / 2 = 0.65 dazu, entlang +z des
    // gedrehten Elternteils.
    const DVec3 world_position = world::world_affine(f.world, f.document.entity_of(id)).translation;
    CHECK(world_position.x == Approx(1.5).margin(1e-5));
    CHECK(world_position.z == Approx(0.0).margin(1e-5));
    CHECK(f.transform(id).position.z == Approx(0.75).margin(1e-5));
    CHECK(f.transform(id).position.x == Approx(0.0).margin(1e-5));
}

TEST_CASE("Rotate gizmo turns around the ring axis and snaps to the angle step", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId id = f.add("Kiste");
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    gizmo.mode = edit::GizmoMode::Rotate;
    gizmo.snap.enabled = true;
    const auto frame = gizmo.frame(f.document, id, camera);
    REQUIRE(frame.has_value());

    // Auf dem Ring um z von 0° bis knapp 90° gegen den Uhrzeigersinn, über mehrere Frames.
    const f64 r = frame->size;
    const math::Vec2 from = pixel_of(camera, DVec3{r, 0.0, 0.0});
    REQUIRE(
        gizmo.update(f.document, id, camera,
                     {.x = from.x, .y = from.y, .hovered = true, .pressed = true, .down = true}));
    for (int degrees = 5; degrees <= 85; degrees += 5) {
        const f64 a = math::radians(static_cast<f32>(degrees));
        const math::Vec2 p = pixel_of(camera, DVec3{r * std::cos(a), r * std::sin(a), 0.0});
        gizmo.update(f.document, id, camera, {.x = p.x, .y = p.y, .hovered = true, .down = true});
    }
    CHECK(gizmo.drag_label() == "Drehen z +90.0°");
    gizmo.update(f.document, id, camera, {.down = false});

    // 85° rastet mit 15°-Schritten auf 90° ein.
    const Vec3 x_axis = f.transform(id).rotation * Vec3{1.0f, 0.0f, 0.0f};
    CHECK(math::near(x_axis, Vec3{0.0f, 1.0f, 0.0f}, 1e-5f));
    CHECK(f.document.history().undo_label() == "Drehen");
}

TEST_CASE("Scale gizmo scales one axis or all axes", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId id = f.add("Kiste");
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    gizmo.mode = edit::GizmoMode::Scale;
    const auto frame = gizmo.frame(f.document, id, camera);
    REQUIRE(frame.has_value());

    // Griff auf x um eine Achslänge nach außen: Faktor 2.
    const DVec3 handle{frame->size * 0.9, 0.0, 0.0};
    drag(gizmo, f, id, camera, pixel_of(camera, handle),
         pixel_of(camera, handle + DVec3{frame->size, 0.0, 0.0}));
    CHECK(f.transform(id).scale.x == Approx(2.0f).margin(1e-3f));
    CHECK(f.transform(id).scale.y == 1.0f);

    // Mitte: alle Achsen gleich; nach links wird es kleiner, aber nie null.
    drag(gizmo, f, id, camera, pixel_of(camera, DVec3{}),
         pixel_of(camera, DVec3{-frame->size * 5.0, 0.0, 0.0}));
    CHECK(f.transform(id).scale.y == Approx(0.001f));
    CHECK(f.transform(id).scale.z == Approx(0.001f));
    CHECK(f.document.history().size() == 3);
}

TEST_CASE("Clicks beside the gizmo are left for selection", "[edit][gizmo]") {
    Fixture f;
    const edit::EntityId id = f.add("Kiste");
    const edit::ViewCamera camera = front_camera();
    edit::Gizmo gizmo;
    CHECK_FALSE(
        gizmo.update(f.document, id, camera,
                     {.x = 10.0f, .y = 10.0f, .hovered = true, .pressed = true, .down = true}));
    CHECK_FALSE(gizmo.dragging());
    CHECK(f.document.history().size() == 1);
}

TEST_CASE("Reparenting keeps the world transform and undo restores the local one",
          "[edit][transform]") {
    Fixture f;
    const edit::EntityId parent = f.add("Eltern");
    const edit::EntityId child = f.add("Kind");
    f.set_transform(parent,
                    world::Transform{.position = DVec3{10.0, 0.0, 0.0},
                                     .rotation = math::angle_axis(math::radians(90.0f), {0, 1, 0}),
                                     .scale = Vec3{2.0f, 2.0f, 2.0f}});
    const world::Transform original{.position = DVec3{1.0, 2.0, 3.0},
                                    .rotation = math::angle_axis(0.3f, {1, 0, 0}),
                                    .scale = Vec3{1.0f, 0.5f, 1.0f}};
    f.set_transform(child, original);
    const u64 handle = f.document.entity_of(child);
    const math::Affine before = world::world_affine(f.world, handle);

    REQUIRE(f.document.execute(edit::reparent(child, parent)));
    const math::Affine after = world::world_affine(f.world, handle);
    CHECK(math::near(after, before, 1e-5f, 1e-5));
    CHECK_FALSE(math::near(f.transform(child).position, original.position, 1e-3));

    // Lösen bringt die Weltlage ebenfalls mit.
    REQUIRE(f.document.execute(edit::reparent(child, 0)));
    CHECK(math::near(world::world_affine(f.world, handle), before, 1e-5f, 1e-5));

    REQUIRE(f.document.undo());
    REQUIRE(f.document.undo());
    CHECK(f.transform(child).position == original.position);
    CHECK(f.transform(child).rotation == original.rotation);
    CHECK(f.transform(child).scale == original.scale);

    // Ohne keep_world bleibt die lokale Lage wie in R2.2.
    REQUIRE(f.document.execute(edit::reparent(child, parent, false)));
    CHECK(f.transform(child).position == original.position);
}

TEST_CASE("Play and stop restore the world bit for bit", "[edit][play]") {
    Fixture f;
    const edit::EntityId root = f.add("Wurzel");
    const edit::EntityId child = f.add("Kind", root);
    const edit::EntityId other = f.add("Anderes");
    // Kind zuletzt umhängen, damit die Reihenfolge der Kennungen nicht der Hierarchie folgt.
    REQUIRE(f.document.execute(edit::reparent(child, other)));
    f.entity(child).set(Spinner{.speed = 2.0f});
    f.entity(root).set(Spinner{.speed = -1.0f});
    (void)world::update_transforms(f.world);
    const std::string scene_before = world::save_scene(f.world);
    const std::vector<std::byte> bytes_before = world_bytes(f);
    const usize history = f.document.history().size();
    const bool dirty = f.document.dirty();

    REQUIRE(f.document.begin_play());
    CHECK(f.document.playing());
    CHECK(f.document.read_only());
    CHECK_FALSE(f.document.begin_play());

    // Spielen: Systeme ändern Komponenten, Entities entstehen und verschwinden.
    jobs::JobSystem jobs{{.worker_count = 1}};
    world::Schedule schedule{f.world};
    schedule.add<Spinner, world::Transform>("Drehen", [](Spinner& s, world::Transform& t) {
        s.angle += s.speed * 0.1f;
        ++s.ticks;
        t.position.y += 1.0;
    });
    for (int i = 0; i < 10; ++i) {
        schedule.run(jobs);
    }
    (void)f.world.create("Geschoss");
    f.world.destroy(f.document.entity_of(root));
    CHECK(world_bytes(f) != bytes_before);

    // Während des Spielens keine Befehle, kein Undo, kein Speichern.
    CHECK_FALSE(f.document.execute(edit::rename(other, "Nein")));
    CHECK_FALSE(f.document.undo());
    CHECK_FALSE(f.document.save("nicht-gespeichert.json"));

    REQUIRE(f.document.end_play());
    CHECK_FALSE(f.document.playing());
    CHECK(world_bytes(f) == bytes_before);
    CHECK(world::save_scene(f.world) == scene_before);
    CHECK(f.world.entity_count() == 3);
    // Kennungen im Dokument und Verlauf gelten weiter.
    CHECK(f.entity(child).name() == "Kind");
    CHECK(f.entity(child).parent() == f.entity(other));
    CHECK(f.document.history().size() == history);
    CHECK(f.document.dirty() == dirty);
    REQUIRE(f.document.undo());
    CHECK(f.entity(child).parent() == f.entity(root));
}

TEST_CASE("Play state changes are announced and stop without play fails", "[edit][play]") {
    Fixture f;
    (void)f.add("Lampe");
    std::vector<bool> states;
    const events::Subscription subscription = f.bus.subscribe<edit::PlayStateChanged>(
        [&](const edit::PlayStateChanged& change) { states.push_back(change.playing); });
    CHECK_FALSE(f.document.end_play());
    REQUIRE(f.document.begin_play());
    REQUIRE(f.document.end_play());
    // Öffnen oder Leeren während des Spielens verwirft die Sicherung.
    REQUIRE(f.document.begin_play());
    f.document.clear();
    CHECK_FALSE(f.document.playing());
    CHECK(states == std::vector<bool>{true, false, true, false});
}
