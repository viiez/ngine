#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.jobs;
import ngine.filesystem;
import ngine.asset;
import ngine.world;
import ngine.animation;

// Animation ohne GPU (R4.6): Import von Skins und Clips, Abtasten, Mischen, Automat und System.
// Ein kleines Modell aus Code: zwei Knochen übereinander (A am Ursprung, B einen Meter darüber),
// ein Streifen aus Eckpunkten bei y = 0, 1 und 2, der Clip „Beugen“ dreht B in einer Sekunde um
// 90° um z, der Clip „Schieben“ verschiebt A nach einer halben Sekunde um einen Meter nach x.

using namespace ngine;
using Catch::Matchers::WithinAbs;

namespace {

constexpr f32 pi = std::numbers::pi_v<f32>;

asset::Model two_bones() {
    asset::Model model;
    model.nodes.resize(3);
    model.nodes[0] = {.name = "Wurzel", .children = {1}};
    model.nodes[1] = {.name = "Knochen_A", .parent = 0u, .children = {2}};
    model.nodes[2] = {.name = "Knochen_B", .translation = {0.0f, 1.0f, 0.0f}, .parent = 1u};
    model.roots = {0};

    asset::Skin skin;
    skin.name = "Skelett";
    skin.joints = {1, 2};
    skin.inverse_bind = {
        math::mat4_identity(),
        math::compose({0.0f, -1.0f, 0.0f}, math::quat_identity(), {1.0f, 1.0f, 1.0f})};
    model.skins.push_back(skin);

    asset::Primitive primitive;
    for (const f32 y : {0.0f, 1.0f, 2.0f}) {
        for (const f32 x : {-0.1f, 0.1f}) {
            asset::Vertex vertex{};
            vertex.position = {x, y, 0.0f};
            vertex.normal = {0.0f, 0.0f, 1.0f};
            vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
            primitive.vertices.push_back(vertex);
            asset::SkinVertex weights{};
            if (y == 0.0f) {
                weights.joints[0] = 0;
                weights.weights[0] = 65535;
            } else if (y == 1.0f) {
                weights.joints[0] = 0;
                weights.joints[1] = 1;
                weights.weights[0] = 32768;
                weights.weights[1] = 32767;
            } else {
                weights.joints[0] = 1;
                weights.weights[0] = 65535;
            }
            primitive.skin.push_back(weights);
        }
    }
    primitive.indices = {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
    model.meshes.push_back({.name = "Streifen", .primitives = {primitive}, .skin = 0u});

    asset::Animation bend;
    bend.name = "Beugen";
    bend.duration = 1.0f;
    const math::Quat quarter = math::angle_axis(pi / 2.0f, {0.0f, 0.0f, 1.0f});
    bend.channels.push_back(
        {.node = 2,
         .path = asset::AnimationPath::Rotation,
         .interpolation = asset::Interpolation::Linear,
         .times = {0.0f, 1.0f},
         .values = {0.0f, 0.0f, 0.0f, 1.0f, quarter.x, quarter.y, quarter.z, quarter.w}});
    model.animations.push_back(bend);

    asset::Animation push;
    push.name = "Schieben";
    push.duration = 1.0f;
    push.channels.push_back({.node = 1,
                             .path = asset::AnimationPath::Translation,
                             .interpolation = asset::Interpolation::Step,
                             .times = {0.0f, 0.5f, 1.0f},
                             .values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}});
    model.animations.push_back(push);
    return model;
}

math::Vec3 apply(const animation::JointMatrix& m, math::Vec3 p) {
    return {m.rows[0].x * p.x + m.rows[0].y * p.y + m.rows[0].z * p.z + m.rows[0].w,
            m.rows[1].x * p.x + m.rows[1].y * p.y + m.rows[1].z * p.z + m.rows[1].w,
            m.rows[2].x * p.x + m.rows[2].y * p.y + m.rows[2].z * p.z + m.rows[2].w};
}

f32 pose_distance(const animation::Pose& a, const animation::Pose& b) {
    f32 distance = 0.0f;
    for (usize i = 0; i < a.size() && i < b.size(); ++i) {
        distance = std::max(distance, math::length(a[i].translation - b[i].translation));
        const f32 d = std::abs(math::dot(a[i].rotation, b[i].rotation));
        distance = std::max(distance, 1.0f - std::min(d, 1.0f));
    }
    return distance;
}

animation::AnimGraph walk_graph(u64 model) {
    auto graph = animation::parse_anim_graph(R"({
      "format": "ngine.animgraph", "version": 1, "model": )" +
                                             std::to_string(model) + R"(,
      "initial": "Boden",
      "parameters": [ { "name": "Tempo", "type": "float", "value": 0 },
                      { "name": "Halt", "type": "bool", "value": false },
                      { "name": "Los", "type": "trigger" } ],
      "states": [
        { "name": "Boden", "parameter": "Tempo",
          "blend": [ { "clip": "Beugen", "value": 0 }, { "clip": "Schieben", "value": 2 } ],
          "events": [ { "time": 0.5, "name": "Mitte" } ] },
        { "name": "Sprung", "clip": "Beugen", "loop": false },
        { "name": "Stand", "clip": "Gibt es nicht" } ],
      "transitions": [
        { "from": "*", "to": "Sprung", "duration": 0.2,
          "conditions": [ { "parameter": "Los", "op": "trigger" } ] },
        { "from": "Sprung", "to": "Boden", "duration": 0.1, "after_end": true },
        { "from": "Boden", "to": "Stand", "duration": 0.0,
          "conditions": [ { "parameter": "Halt", "op": "true" } ] } ] })");
    REQUIRE(graph);
    return *graph;
}

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_anim_" + std::to_string(random()));
        const std::u8string text = base.u8string();
        path_.assign(text.begin(), text.end());
    }
    ~TempDir() {
        static_cast<void>(filesystem::remove(path_));
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(std::string_view name) const {
        return filesystem::join(path_, name);
    }

private:
    std::string path_;
};

std::string base64(std::span<const std::byte> data) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < data.size(); i += 3) {
        u32 chunk = std::to_integer<u32>(data[i]) << 16;
        if (i + 1 < data.size()) {
            chunk |= std::to_integer<u32>(data[i + 1]) << 8;
        }
        if (i + 2 < data.size()) {
            chunk |= std::to_integer<u32>(data[i + 2]);
        }
        out += table[(chunk >> 18) & 63];
        out += table[(chunk >> 12) & 63];
        out += i + 1 < data.size() ? table[(chunk >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[chunk & 63] : '=';
    }
    return out;
}

template <typename T>
void append(std::vector<std::byte>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

// glTF mit dem Modell aus two_bones(): Gelenke als u8, fünf Einflüsse an einem Eckpunkt (einer
// davon fällt beim Import weg) und der Clip als CUBICSPLINE.
std::string skinned_gltf() {
    std::vector<std::byte> bin;
    // Positionen (3 Eckpunkte), Gelenke JOINTS_0 und JOINTS_1 (u8), Gewichte WEIGHTS_0 und
    // WEIGHTS_1 (f32), Indizes, inverse Bindematrizen, Zeiten, Werte (kubisch: 3 je Schlüssel).
    for (const f32 y : {0.0f, 1.0f, 2.0f}) {
        append(bin, 0.0f);
        append(bin, y);
        append(bin, 0.0f);
    } // 0..36
    const u8 joints0[12] = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
    const u8 joints1[12] = {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    for (const u8 j : joints0) {
        append(bin, j);
    } // 36..48
    for (const u8 j : joints1) {
        append(bin, j);
    } // 48..60
    const f32 weights0[12] = {1, 0, 0, 0, 0.4f, 0.3f, 0.1f, 0.1f, 1, 0, 0, 0};
    const f32 weights1[12] = {0, 0, 0, 0, 0.1f, 0, 0, 0, 0, 0, 0, 0};
    for (const f32 w : weights0) {
        append(bin, w);
    } // 60..108
    for (const f32 w : weights1) {
        append(bin, w);
    } // 108..156
    for (const u32 i : {0u, 1u, 2u}) {
        append(bin, i);
    } // 156..168
    const f32 matrices[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0,  0, 1,
                              1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
    for (const f32 m : matrices) {
        append(bin, m);
    } // 168..296
    append(bin, 0.0f);
    append(bin, 1.0f); // 296..304
    const f32 s = std::sin(pi / 4.0f);
    const f32 values[24] = {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, s, s, 0, 0, 0, 0};
    for (const f32 v : values) {
        append(bin, v);
    } // 304..400
    const std::string uri = "data:application/octet-stream;base64," + base64(bin);
    return R"({
  "asset": { "version": "2.0" },
  "scene": 0, "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "name": "Wurzel", "children": [ 1, 3 ] },
             { "name": "Knochen_A", "children": [ 2 ] },
             { "name": "Knochen_B", "translation": [ 0, 1, 0 ] },
             { "name": "Netz", "mesh": 0, "skin": 0 } ],
  "meshes": [ { "name": "Streifen", "primitives": [ { "attributes": {
      "POSITION": 0, "JOINTS_0": 1, "JOINTS_1": 2, "WEIGHTS_0": 3, "WEIGHTS_1": 4 },
      "indices": 5 } ] } ],
  "skins": [ { "joints": [ 1, 2 ], "inverseBindMatrices": 6 } ],
  "animations": [ { "name": "Beugen",
      "samplers": [ { "input": 7, "output": 8, "interpolation": "CUBICSPLINE" } ],
      "channels": [ { "sampler": 0, "target": { "node": 2, "path": "rotation" } } ] } ],
  "buffers": [ { "byteLength": 400, "uri": ")" +
           uri + R"(" } ],
  "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
                   { "buffer": 0, "byteOffset": 36, "byteLength": 12 },
                   { "buffer": 0, "byteOffset": 48, "byteLength": 12 },
                   { "buffer": 0, "byteOffset": 60, "byteLength": 48 },
                   { "buffer": 0, "byteOffset": 108, "byteLength": 48 },
                   { "buffer": 0, "byteOffset": 156, "byteLength": 12 },
                   { "buffer": 0, "byteOffset": 168, "byteLength": 128 },
                   { "buffer": 0, "byteOffset": 296, "byteLength": 8 },
                   { "buffer": 0, "byteOffset": 304, "byteLength": 96 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [ 0, 0, 0 ], "max": [ 0, 2, 0 ] },
    { "bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 5, "componentType": 5125, "count": 3, "type": "SCALAR" },
    { "bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4" },
    { "bufferView": 7, "componentType": 5126, "count": 2, "type": "SCALAR",
      "min": [ 0 ], "max": [ 1 ] },
    { "bufferView": 8, "componentType": 5126, "count": 6, "type": "VEC4" } ] })";
}

} // namespace

TEST_CASE("glTF skins and clips are imported", "[animation]") {
    TempDir dir;
    const std::string path = dir.file("knochen.gltf");
    REQUIRE(filesystem::write_text(path, skinned_gltf()));
    auto model = asset::load_gltf(path);
    REQUIRE(model);
    REQUIRE(model->skins.size() == 1);
    CHECK(model->skins[0].joints == std::vector<u32>{1, 2});
    CHECK_THAT(model->skins[0].inverse_bind[1][3].y, WithinAbs(-1.0, 1e-6));
    REQUIRE(model->meshes.size() == 1);
    CHECK(model->meshes[0].skin == 0u);
    CHECK(model->nodes[3].skin == 0u);
    const asset::Primitive& primitive = model->meshes[0].primitives[0];
    REQUIRE(primitive.skin.size() == 3);
    for (const asset::SkinVertex& vertex : primitive.skin) {
        u32 sum = 0;
        for (const u16 w : vertex.weights) {
            sum += w;
        }
        CHECK(sum == 65535);
    }
    // Fünf Einflüsse: der schwächste (0,1 an Gelenk 1 aus JOINTS_1) wird gekürzt, die anderen
    // neu normiert.
    CHECK(primitive.skin[1].joints[0] == 0);
    CHECK(primitive.skin[1].weights[0] > primitive.skin[1].weights[1]);

    REQUIRE(model->animations.size() == 1);
    const asset::Animation& clip = model->animations[0];
    CHECK(clip.name == "Beugen");
    CHECK_THAT(clip.duration, WithinAbs(1.0, 1e-6));
    REQUIRE(clip.channels.size() == 1);
    // CUBICSPLINE wird auf 30 Hz linear abgetastet.
    CHECK(clip.channels[0].interpolation == asset::Interpolation::Linear);
    CHECK(clip.channels[0].times.size() == 31);
    f32 end[4] = {};
    asset::sample_channel(clip.channels[0], 1.0f, end);
    CHECK_THAT(end[2], WithinAbs(std::sin(pi / 4.0f), 1e-5));
}

TEST_CASE("Cooked models keep skins, weights and clips", "[animation]") {
    asset::CookedModel cooked;
    cooked.model = two_bones();
    asset::ensure_meshlets(cooked.model);
    const auto bytes = asset::write_cooked_model(cooked);
    auto read = asset::read_cooked_model(bytes);
    REQUIRE(read);
    const asset::Model& model = read->model;
    REQUIRE(model.skins.size() == 1);
    CHECK(model.skins[0].joints == cooked.model.skins[0].joints);
    // Mat4 vergleicht MSVC nicht mit ==, deshalb Byte für Byte.
    REQUIRE(model.skins[0].inverse_bind.size() == cooked.model.skins[0].inverse_bind.size());
    CHECK(std::memcmp(model.skins[0].inverse_bind.data(), cooked.model.skins[0].inverse_bind.data(),
                      model.skins[0].inverse_bind.size() * sizeof(math::Mat4)) == 0);
    CHECK(model.meshes[0].skin == 0u);
    REQUIRE(model.meshes[0].primitives[0].skin.size() == 6);
    CHECK(std::memcmp(model.meshes[0].primitives[0].skin.data(),
                      cooked.model.meshes[0].primitives[0].skin.data(),
                      6 * sizeof(asset::SkinVertex)) == 0);
    REQUIRE(model.animations.size() == 2);
    CHECK(model.animations[1].channels[0].interpolation == asset::Interpolation::Step);
    CHECK(model.animations[0].channels[0].values == cooked.model.animations[0].channels[0].values);

    // Ein Gelenk außerhalb des Skins macht die Datei ungültig.
    asset::CookedModel broken = cooked;
    broken.model.meshes[0].primitives[0].skin[0].joints[0] = 7;
    CHECK_FALSE(asset::read_cooked_model(asset::write_cooked_model(broken)));
}

TEST_CASE("Skinned meshlets enclose every pose of every clip", "[animation]") {
    asset::Model model = two_bones();
    asset::ensure_meshlets(model);
    const asset::Primitive& primitive = model.meshes[0].primitives[0];
    REQUIRE_FALSE(primitive.meshlets.empty());
    // Nach „Beugen“ liegt die Spitze bei (-1, 1, 0), nach „Schieben“ alles einen Meter weiter in x.
    for (const auto& [clip, time] : {std::pair{0u, 1.0f}, std::pair{1u, 0.75f}}) {
        const auto nodes = asset::node_matrices(model, &model.animations[clip], time);
        const auto palette = asset::skin_palette(model, 0, nodes);
        for (usize v = 0; v < primitive.vertices.size(); ++v) {
            const math::Vec3 p =
                asset::skin_position(primitive.skin[v], primitive.vertices[v].position, palette);
            bool inside = false;
            for (const meshlet::Meshlet& m : primitive.meshlets) {
                inside = inside || math::length(p - m.center) <= m.radius;
            }
            CHECK(inside);
        }
    }
    for (const meshlet::Meshlet& m : primitive.meshlets) {
        CHECK(m.cone_cutoff == 1.0f);
    }
}

TEST_CASE("Sampling, blending and palettes", "[animation]") {
    auto rig = animation::make_rig(two_bones());
    REQUIRE(rig);
    CHECK(rig->order.front() == 0);
    CHECK(rig->find_clip("Schieben") == 1u);
    CHECK_FALSE(rig->find_clip("Fehlt"));

    animation::Pose pose;
    animation::sample(*rig, 0, 1.0f, pose);
    std::vector<math::Mat4> model;
    animation::model_space(*rig, pose, model);
    std::vector<animation::JointMatrix> palette;
    animation::palette(*rig, 0, model, palette);
    REQUIRE(palette.size() == 2);
    // Eckpunkt an B bei (0, 2, 0) dreht um (0, 1, 0) nach (-1, 1, 0).
    const math::Vec3 tip = apply(palette[1], {0.0f, 2.0f, 0.0f});
    CHECK_THAT(tip.x, WithinAbs(-1.0, 1e-5));
    CHECK_THAT(tip.y, WithinAbs(1.0, 1e-5));
    // A bleibt.
    const math::Vec3 base = apply(palette[0], {0.0f, 0.0f, 0.0f});
    CHECK_THAT(math::length(base), WithinAbs(0.0, 1e-6));

    // Halb gemischt mit der Ruhelage: 45°.
    animation::blend(rig->rest, pose, 0.5f, pose);
    animation::model_space(*rig, pose, model);
    animation::palette(*rig, 0, model, palette);
    const math::Vec3 half = apply(palette[1], {0.0f, 2.0f, 0.0f});
    CHECK_THAT(half.x, WithinAbs(-std::sin(pi / 4.0f), 1e-4));

    // STEP springt bei 0,5 s.
    animation::sample(*rig, 1, 0.49f, pose);
    CHECK_THAT(pose[1].translation.x, WithinAbs(0.0, 1e-6));
    animation::sample(*rig, 1, 0.5f, pose);
    CHECK_THAT(pose[1].translation.x, WithinAbs(1.0, 1e-6));
}

TEST_CASE("Animation graphs parse and write", "[animation]") {
    const animation::AnimGraph graph = walk_graph(42);
    CHECK(graph.model == 42);
    REQUIRE(graph.states.size() == 3);
    CHECK(graph.states[0].blend.size() == 2);
    CHECK(graph.states[1].loop == false);
    REQUIRE(graph.transitions.size() == 3);
    CHECK(graph.transitions[0].conditions[0].op == animation::ConditionOp::Trigger);
    CHECK(graph.transitions[1].after_end);
    auto again = animation::parse_anim_graph(animation::write_anim_graph(graph));
    REQUIRE(again);
    CHECK(*again == graph);
    CHECK_FALSE(animation::parse_anim_graph(R"({"format": "ngine.animgraph", "states": []})"));
    CHECK_FALSE(animation::parse_anim_graph(R"({"format": "anders", "states": [{"name": "a"}]})"));
}

TEST_CASE("State machine blends, transitions without jumps and fires events", "[animation]") {
    auto rig = animation::make_rig(two_bones());
    REQUIRE(rig);
    std::vector<std::string> warnings;
    auto created = animation::StateMachine::create(walk_graph(1), *rig, &warnings);
    REQUIRE(created);
    animation::StateMachine machine = *created;
    // Clip „Gibt es nicht“ fehlt: eine Warnung, der Zustand zeigt die Ruhelage.
    CHECK(warnings.size() == 1);
    CHECK(machine.state() == "Boden");

    // Tempo 1 mischt „Beugen“ und „Schieben“ halb.
    CHECK(machine.set_float("Tempo", 1.0f));
    CHECK_FALSE(machine.set_float("Unbekannt", 1.0f));
    std::vector<const std::string*> events;
    machine.update(0.25f, &events);
    CHECK(events.empty());
    machine.update(0.3f, &events);
    REQUIRE(events.size() == 1);
    CHECK(*events[0] == "Mitte");

    // Übergang mit Auslöser: Die Pose ändert sich in kleinen Schritten stetig.
    animation::Pose before;
    animation::Pose after;
    machine.evaluate(*rig, before);
    CHECK(machine.set_trigger("Los"));
    f32 largest = 0.0f;
    for (int i = 0; i < 60; ++i) {
        machine.update(1.0f / 120.0f);
        machine.evaluate(*rig, after);
        largest = std::max(largest, pose_distance(before, after));
        before = after;
    }
    CHECK(machine.state() == "Sprung");
    CHECK(machine.parameter("Los") == 0.0f);
    CHECK(largest < 0.05f);

    // Mitten in einer Überblendung ein neuer Übergang: auch dann kein Sprung. „Sprung“ endet und
    // blendet zurück auf den Boden; mitten darin kommt der Auslöser.
    for (int i = 0; i < 600 && machine.state() != "Boden"; ++i) {
        machine.update(1.0f / 120.0f);
        machine.evaluate(*rig, before);
    }
    REQUIRE(machine.state() == "Boden");
    CHECK(machine.fade() < 1.0f);
    machine.set_trigger("Los");
    largest = 0.0f;
    for (int i = 0; i < 30; ++i) {
        machine.update(1.0f / 120.0f);
        machine.evaluate(*rig, after);
        largest = std::max(largest, pose_distance(before, after));
        before = after;
    }
    CHECK(machine.state() == "Sprung");
    CHECK(largest < 0.05f);

    // Nach dem Ende von „Sprung“ (ohne Schleife) zurück auf den Boden.
    for (int i = 0; i < 150; ++i) {
        machine.update(1.0f / 60.0f);
    }
    CHECK(machine.state() == "Boden");

    // Schalter ohne Überblendung.
    machine.set_bool("Halt", true);
    machine.update(0.01f);
    CHECK(machine.state() == "Stand");
    CHECK(machine.fade() == 1.0f);
}

TEST_CASE("Animation system poses animators where they are needed", "[animation]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    animation::register_types(types);
    world::World world{types};

    asset::ResourceManager resources;
    animation::register_loaders(resources);
    animation::register_graph_loader(resources);
    const u64 model_id = asset::generated_id("test-knochen");
    const u64 graph_id = asset::generated_id("test-automat");
    auto rig = animation::make_rig(two_bones());
    REQUIRE(rig);
    resources.add<animation::Rig>(model_id, *rig);
    resources.add<animation::AnimGraph>(graph_id, walk_graph(model_id));

    events::EventBus bus;
    std::vector<animation::AnimationEvent> received;
    auto subscription = bus.subscribe<animation::AnimationEvent>(
        [&](const animation::AnimationEvent& event) { received.push_back(event); });

    animation::AnimationSystem system{resources};
    system.set_events(&bus);
    world::Entity figure = world.create("Figur");
    figure.set(world::Transform{}).set(animation::Animator{.graph = graph_id});
    world::Entity body = world.create("Körper");
    body.set(world::Transform{});
    body.set_parent(figure);

    // Vor dem ersten update(): Werte werden gemerkt.
    CHECK(system.set_float(figure.id(), "Tempo", 2.0f));
    world::update_transforms(world);
    system.update(world, 1.0 / 60.0);
    CHECK(system.stats().animators == 1);
    CHECK(system.stats().running == 1);
    CHECK(system.parameter(figure.id(), "Tempo") == 2.0f);
    CHECK(system.state(figure.id()) == "Boden");
    // Das Netz am Kind findet die Palette am Vorfahren, aber nur für das richtige Modell.
    CHECK(system.palette(world, body.id(), model_id, 0).size() == 2);
    CHECK(system.palette(world, body.id(), model_id + 1, 0).empty());

    // Ereignis „Mitte“ bei der halben Zeit des Zustands.
    for (int i = 0; i < 60; ++i) {
        system.update(world, 1.0 / 60.0);
    }
    REQUIRE_FALSE(received.empty());
    CHECK(received[0].entity == figure.id());
    CHECK(received[0].name == "Mitte");

    // Ohne Renderer: Visible rechnet keine Pose mehr, Always schon; der Automat läuft weiter.
    system.set_rendering(false);
    system.update(world, 1.0 / 60.0);
    CHECK(system.palette(world, body.id(), model_id, 0).empty());
    CHECK(system.stats().posed == 0);
    figure.get_mut<animation::Animator>()->pose_mode = animation::PoseMode::Always;
    system.update(world, 1.0 / 60.0);
    CHECK(system.palette(world, body.id(), model_id, 0).size() == 2);

    // Hot Reload des Automaten: Parameter mit gleichem Namen behalten ihren Wert.
    resources.add<animation::AnimGraph>(graph_id, walk_graph(model_id));
    system.update(world, 1.0 / 60.0);
    CHECK(system.parameter(figure.id(), "Tempo") == 2.0f);

    // Ohne Animator vergisst das System die Entity.
    figure.remove<animation::Animator>();
    system.update(world, 1.0 / 60.0);
    CHECK(system.stats().animators == 0);
    CHECK_FALSE(system.view(figure.id()));
}

TEST_CASE("Many animators run in parallel jobs", "[animation][jobs]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    animation::register_types(types);
    world::World world{types};
    jobs::JobSystem jobs{{.worker_count = 3}};
    asset::ResourceManager resources;
    const u64 model_id = asset::generated_id("test-knochen");
    const u64 graph_id = asset::generated_id("test-automat");
    auto rig = animation::make_rig(two_bones());
    REQUIRE(rig);
    resources.add<animation::Rig>(model_id, *rig);
    resources.add<animation::AnimGraph>(graph_id, walk_graph(model_id));

    animation::AnimationSystem parallel{resources, &jobs};
    animation::AnimationSystem serial{resources};
    std::vector<world::Entity> figures;
    for (int i = 0; i < 200; ++i) {
        world::Entity figure = world.create();
        figure.set(world::Transform{}).set(animation::Animator{.graph = graph_id});
        figures.push_back(figure);
    }
    world::update_transforms(world);
    for (int frame = 0; frame < 30; ++frame) {
        for (usize i = 0; i < figures.size(); ++i) {
            const f32 tempo = static_cast<f32>((i + static_cast<usize>(frame)) % 3);
            parallel.set_float(figures[i].id(), "Tempo", tempo);
            serial.set_float(figures[i].id(), "Tempo", tempo);
        }
        parallel.update(world, 1.0 / 60.0);
        serial.update(world, 1.0 / 60.0);
    }
    CHECK(parallel.stats().posed == 200);
    // Gleiche Eingaben, gleiche Paletten, egal in welchem Thread gerechnet.
    for (const world::Entity& figure : figures) {
        const auto a = parallel.palette(world, figure.id(), model_id, 0);
        const auto b = serial.palette(world, figure.id(), model_id, 0);
        REQUIRE(a.size() == b.size());
        CHECK(std::memcmp(a.data(), b.data(), a.size_bytes()) == 0);
    }
}

TEST_CASE("Trigger counters reach followers between snapshots", "[animation]") {
    reflect::TypeRegistry types;
    world::register_types(types);
    animation::register_types(types);
    asset::ResourceManager resources;
    animation::register_loaders(resources);
    animation::register_graph_loader(resources);
    const u64 model_id = asset::generated_id("test-knochen");
    const u64 graph_id = asset::generated_id("test-automat");
    auto rig = animation::make_rig(two_bones());
    REQUIRE(rig);
    resources.add<animation::Rig>(model_id, *rig);
    resources.add<animation::AnimGraph>(graph_id, walk_graph(model_id));

    // Server zählt, Client folgt; Schnappschüsse kopieren AnimatorParameters.
    world::World server_world{types};
    world::World client_world{types};
    animation::AnimationSystem server{resources};
    animation::AnimationSystem client{resources};
    server.set_parameter_sync(animation::ParameterSync::Publish);
    client.set_parameter_sync(animation::ParameterSync::Follow);
    world::Entity on_server = server_world.create("Figur");
    on_server.set(world::Transform{}).set(animation::Animator{.graph = graph_id});
    world::Entity on_client = client_world.create("Figur");
    on_client.set(world::Transform{}).set(animation::Animator{.graph = graph_id});
    const auto frame = [&](bool snapshot) {
        server.update(server_world, 1.0 / 60.0);
        if (snapshot) {
            on_client.set(*on_server.get<animation::AnimatorParameters>());
        }
        client.update(client_world, 1.0 / 60.0);
    };
    frame(true);
    frame(true);
    CHECK(on_server.get<animation::AnimatorParameters>()->triggers == 0);
    CHECK(client.state(on_client.id()) == "Boden");

    // Ausgelöst zwischen zwei Schnappschüssen: Auf dem Server ist der Auslöser im nächsten Bild
    // verbraucht, der Zähler auf Platz 2 (Los) bleibt.
    CHECK(server.set_trigger(on_server.id(), "Los"));
    frame(false);
    frame(false);
    CHECK(on_server.get<animation::AnimatorParameters>()->triggers == (1u << 16));
    for (int i = 0; i < 40; ++i) {
        frame(true);
    }
    CHECK(server.state(on_server.id()) == "Sprung");
    CHECK(client.state(on_client.id()) == "Sprung");

    // Ein neuer Folger übernimmt den Stand, ohne alte Auslöser noch einmal auszulösen.
    animation::AnimationSystem late{resources};
    late.set_parameter_sync(animation::ParameterSync::Follow);
    world::World late_world{types};
    world::Entity on_late = late_world.create("Figur");
    on_late.set(world::Transform{})
        .set(animation::Animator{.graph = graph_id})
        .set(*on_server.get<animation::AnimatorParameters>());
    for (int i = 0; i < 10; ++i) {
        late.update(late_world, 1.0 / 60.0);
    }
    CHECK(late.state(on_late.id()) == "Boden");

    // Der Zähler läuft nach 255 über, ohne die anderen Plätze zu berühren.
    for (int i = 0; i < 256; ++i) {
        CHECK(server.set_trigger(on_server.id(), "Los"));
    }
    frame(false);
    CHECK(on_server.get<animation::AnimatorParameters>()->triggers == (1u << 16));
}
