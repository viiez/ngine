#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.asset;

// glTF ohne GPU: Die Tests schreiben ihre Dateien selbst, samt eines kleinen PNG ohne Kompression.

using namespace ngine;
using Catch::Approx;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_asset_" + std::to_string(random()));
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

using Bytes = std::vector<std::byte>;

template <typename T>
void append(Bytes& out, const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void append_be32(Bytes& out, u32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

void append_text(Bytes& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

u32 crc32(const Bytes& data, usize begin) {
    u32 crc = 0xFFFFFFFFu;
    for (usize i = begin; i < data.size(); ++i) {
        crc ^= std::to_integer<u32>(data[i]);
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}

void chunk(Bytes& png, std::string_view type, const Bytes& data) {
    append_be32(png, static_cast<u32>(data.size()));
    const usize start = png.size();
    append_text(png, type);
    png.insert(png.end(), data.begin(), data.end());
    append_be32(png, crc32(png, start));
}

// PNG mit RGBA8 und einem einzigen unkomprimierten Deflate-Block (bis 64 KiB Rohdaten).
Bytes encode_png(u32 width, u32 height, const std::vector<u8>& rgba) {
    Bytes raw;
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(std::byte{0}); // Filter: keiner
        for (u32 x = 0; x < width * 4; ++x) {
            raw.push_back(static_cast<std::byte>(rgba[(y * width * 4) + x]));
        }
    }
    Bytes zlib{std::byte{0x78}, std::byte{0x01}, std::byte{0x01}};
    const auto length = static_cast<u16>(raw.size());
    append(zlib, length);
    append(zlib, static_cast<u16>(~length));
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    u32 a = 1;
    u32 b = 0;
    for (const std::byte byte : raw) {
        a = (a + std::to_integer<u32>(byte)) % 65521;
        b = (b + a) % 65521;
    }
    append_be32(zlib, (b << 16) | a);

    Bytes png{std::byte{0x89}, std::byte{'P'},  std::byte{'N'},  std::byte{'G'},
              std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};
    Bytes header;
    append_be32(header, width);
    append_be32(header, height);
    for (const u8 value : {u8{8}, u8{6}, u8{0}, u8{0}, u8{0}}) {
        header.push_back(static_cast<std::byte>(value));
    }
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    return png;
}

// 2x1 Pixel: Rot, dann halbdurchsichtiges Grün.
Bytes test_png() {
    return encode_png(2, 1, {255, 0, 0, 255, 0, 255, 0, 128});
}

std::string base64(const Bytes& data) {
    static constexpr std::string_view table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (usize i = 0; i < data.size(); i += 3) {
        u32 n = std::to_integer<u32>(data[i]) << 16;
        if (i + 1 < data.size()) {
            n |= std::to_integer<u32>(data[i + 1]) << 8;
        }
        if (i + 2 < data.size()) {
            n |= std::to_integer<u32>(data[i + 2]);
        }
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < data.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[n & 63] : '=';
    }
    return out;
}

// Geometrie eines Dreiecks: Positionen (36 Bytes), Normalen (36), UVs (24), Indizes u16 (6 + 2
// Füllbytes) = 104 Bytes.
Bytes triangle_buffer() {
    Bytes data;
    for (const f32 v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        append(data, v);
    }
    for (int i = 0; i < 3; ++i) {
        for (const f32 v : {0.0f, 0.0f, 2.0f}) { // nicht normiert: der Import normiert
            append(data, v);
        }
    }
    for (const f32 v : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f}) {
        append(data, v);
    }
    for (const u16 i : {u16{0}, u16{1}, u16{2}, u16{0}}) {
        append(data, i);
    }
    return data;
}

// JSON für Accessoren und Buffer Views des Dreiecks; buffers und images ergänzt der Aufrufer.
std::string triangle_json(std::string_view buffers, std::string_view images,
                          std::string_view extra_views = "") {
    return std::string{R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [
    {"name": "Wurzel", "translation": [1, 2, 3], "children": [1, 2]},
    {"name": "Dreieck", "mesh": 0, "scale": [2, 2, 2]},
    {"name": "Matrix", "matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 4,5,6,1]}
  ],
  "meshes": [{"name": "Tri", "primitives": [{
    "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
    "indices": 3, "material": 0}]}],
  "materials": [{
    "name": "Rot",
    "pbrMetallicRoughness": {
      "baseColorFactor": [1, 0.5, 0.25, 1],
      "metallicFactor": 0.25, "roughnessFactor": 0.75,
      "baseColorTexture": {"index": 0}
    },
    "emissiveFactor": [0.1, 0.2, 0.3],
    "alphaMode": "MASK", "alphaCutoff": 0.4, "doubleSided": true
  }],
  "textures": [{"source": 0, "sampler": 0}],
  "samplers": [{"magFilter": 9728, "minFilter": 9729, "wrapS": 33071}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 36},
    {"buffer": 0, "byteOffset": 72, "byteLength": 24},
    {"buffer": 0, "byteOffset": 96, "byteLength": 6})"} +
           std::string{extra_views} + R"(
  ],
  "buffers": )" +
           std::string{buffers} +
           R"(,
  "images": )" +
           std::string{images} + "\n}\n";
}

void write(const std::string& path, std::string_view text) {
    REQUIRE(filesystem::write_text(path, text));
}

void write(const std::string& path, const Bytes& bytes) {
    REQUIRE(filesystem::write_file(path, bytes));
}

// Prüft alles, was in jeder Variante der Dreiecksdatei gleich ist.
void check_triangle_model(const asset::Model& model) {
    REQUIRE(model.nodes.size() == 3);
    CHECK(model.roots == std::vector<u32>{0});
    const asset::Node& root = model.nodes[0];
    CHECK(root.name == "Wurzel");
    CHECK(root.translation == math::Vec3{1.0f, 2.0f, 3.0f});
    CHECK(root.children == std::vector<u32>{1, 2});
    CHECK_FALSE(root.parent);
    CHECK(model.nodes[1].parent == 0u);
    CHECK(model.nodes[1].mesh == 0u);
    CHECK(model.nodes[1].scale == math::Vec3{2.0f, 2.0f, 2.0f});
    CHECK(model.nodes[2].translation.x == Approx(4.0f));
    CHECK(model.nodes[2].translation.z == Approx(6.0f));
    CHECK(model.nodes[2].rotation.w == Approx(1.0f));

    REQUIRE(model.meshes.size() == 1);
    REQUIRE(model.meshes[0].primitives.size() == 1);
    const asset::Primitive& primitive = model.meshes[0].primitives[0];
    CHECK(primitive.material == 0u);
    CHECK(primitive.indices == std::vector<u32>{0, 1, 2});
    REQUIRE(primitive.vertices.size() == 3);
    CHECK(primitive.vertices[1].position == math::Vec3{1.0f, 0.0f, 0.0f});
    CHECK(primitive.vertices[2].uv == math::Vec2{0.0f, 1.0f});
    CHECK(primitive.vertices[0].normal == math::Vec3{0.0f, 0.0f, 1.0f});
    // Tangente fehlt in der Datei: erzeugt in Richtung von u, mit positiver Bitangente (v).
    CHECK(math::near(primitive.vertices[0].tangent, math::Vec4{1.0f, 0.0f, 0.0f, 1.0f}, 1e-5f));

    REQUIRE(model.materials.size() == 1);
    const asset::Material& material = model.materials[0];
    CHECK(material.name == "Rot");
    CHECK(material.base_color == math::Vec4{1.0f, 0.5f, 0.25f, 1.0f});
    CHECK(material.metallic == 0.25f);
    CHECK(material.roughness == 0.75f);
    CHECK(material.emissive.z == Approx(0.3f));
    CHECK(material.base_color_texture == 0u);
    CHECK_FALSE(material.normal_texture);
    CHECK(material.alpha_mode == asset::AlphaMode::Mask);
    CHECK(material.alpha_cutoff == Approx(0.4f));
    CHECK(material.double_sided);

    REQUIRE(model.textures.size() == 1);
    CHECK(model.textures[0].image == 0);
    CHECK(model.textures[0].sampler.mag == asset::Filter::Nearest);
    CHECK_FALSE(model.textures[0].sampler.mipmaps);
    CHECK(model.textures[0].sampler.wrap_u == asset::Wrap::ClampToEdge);
    CHECK(model.textures[0].sampler.wrap_v == asset::Wrap::Repeat);

    REQUIRE(model.images.size() == 1);
    const asset::Image& image = model.images[0];
    CHECK(image.width == 2);
    CHECK(image.height == 1);
    REQUIRE(image.pixels.size() == 8);
    CHECK(std::to_integer<int>(image.pixels[0]) == 255);
    CHECK(std::to_integer<int>(image.pixels[5]) == 255);
    CHECK(std::to_integer<int>(image.pixels[7]) == 128);
}

} // namespace

TEST_CASE("glTF with embedded buffer and data URI image", "[asset]") {
    TempDir dir;
    const std::string buffer =
        R"([{"byteLength": 104, "uri": "data:application/octet-stream;base64,)" +
        base64(triangle_buffer()) + R"("}])";
    const std::string images =
        R"([{"uri": "data:image/png;base64,)" + base64(test_png()) + R"("}])";
    write(dir.file("eingebettet.gltf"), triangle_json(buffer, images));

    const auto model = asset::load_gltf(dir.file("eingebettet.gltf"));
    REQUIRE(model.has_value());
    check_triangle_model(*model);
}

TEST_CASE("glTF with external buffer and image files", "[asset]") {
    TempDir dir;
    write(dir.file("dreieck.bin"), triangle_buffer());
    write(dir.file("farbe.png"), test_png());
    write(dir.file("extern.gltf"), triangle_json(R"([{"byteLength": 104, "uri": "dreieck.bin"}])",
                                                 R"([{"uri": "farbe.png"}])"));

    const auto model = asset::load_gltf(dir.file("extern.gltf"));
    REQUIRE(model.has_value());
    check_triangle_model(*model);

    SECTION("missing external buffer") {
        REQUIRE(filesystem::remove(dir.file("dreieck.bin")));
        const auto missing = asset::load_gltf(dir.file("extern.gltf"));
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().find("extern.gltf") != std::string::npos);
    }
}

TEST_CASE("glb with binary chunk and image in a buffer view", "[asset]") {
    TempDir dir;
    Bytes bin = triangle_buffer();
    const Bytes png = test_png();
    const usize image_offset = bin.size();
    bin.insert(bin.end(), png.begin(), png.end());
    while (bin.size() % 4 != 0) {
        bin.push_back(std::byte{0});
    }
    std::string json =
        triangle_json(R"([{"byteLength": )" + std::to_string(bin.size()) + "}]",
                      R"([{"bufferView": 4, "mimeType": "image/png"}])",
                      ",\n    {\"buffer\": 0, \"byteOffset\": " + std::to_string(image_offset) +
                          ", \"byteLength\": " + std::to_string(png.size()) + "}");
    while (json.size() % 4 != 0) {
        json += ' ';
    }

    Bytes glb;
    append(glb, u32{0x46546C67}); // "glTF"
    append(glb, u32{2});
    append(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
    append(glb, static_cast<u32>(json.size()));
    append(glb, u32{0x4E4F534A}); // "JSON"
    append_text(glb, json);
    append(glb, static_cast<u32>(bin.size()));
    append(glb, u32{0x004E4942}); // "BIN\0"
    glb.insert(glb.end(), bin.begin(), bin.end());
    write(dir.file("modell.glb"), glb);

    const auto model = asset::load_gltf(dir.file("modell.glb"));
    REQUIRE(model.has_value());
    check_triangle_model(*model);
}

TEST_CASE("glTF without normals and indices gets generated ones", "[asset]") {
    TempDir dir;
    Bytes data;
    // Zwei Dreiecke eines Quadrats in der xz-Ebene, ohne Indizes und ohne UVs.
    for (const f32 v : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, //
                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}) {
        append(data, v);
    }
    write(dir.file("quadrat.gltf"),
          R"({"asset": {"version": "2.0"},
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "nodes": [{"mesh": 0}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 6, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 0, 1]}],
  "bufferViews": [{"buffer": 0, "byteLength": 72}],
  "buffers": [{"byteLength": 72, "uri": "data:application/octet-stream;base64,)" +
              base64(data) + R"("}]})");

    const auto model = asset::load_gltf(dir.file("quadrat.gltf"));
    REQUIRE(model.has_value());
    // Ohne Szene: alle Knoten ohne Elternteil sind Wurzeln.
    CHECK(model->roots == std::vector<u32>{0});
    const asset::Primitive& primitive = model->meshes[0].primitives[0];
    CHECK(primitive.indices == std::vector<u32>{0, 1, 2, 3, 4, 5});
    CHECK_FALSE(primitive.material);
    for (const asset::Vertex& vertex : primitive.vertices) {
        CHECK(math::near(vertex.normal, math::Vec3{0.0f, 1.0f, 0.0f}, 1e-5f));
        const math::Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
        CHECK(math::dot(tangent, vertex.normal) == Approx(0.0f).margin(1e-5f));
        CHECK(math::length(tangent) == Approx(1.0f));
    }
}

TEST_CASE("Broken glTF files report errors instead of crashing", "[asset]") {
    TempDir dir;
    const std::string buffer =
        R"([{"byteLength": 104, "uri": "data:application/octet-stream;base64,)" +
        base64(triangle_buffer()) + R"("}])";
    const std::string image = R"([{"uri": "data:image/png;base64,)" + base64(test_png()) + R"("}])";

    SECTION("missing file") {
        const auto model = asset::load_gltf(dir.file("gibtsnicht.gltf"));
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().find("gibtsnicht.gltf") != std::string::npos);
    }
    SECTION("invalid JSON") {
        write(dir.file("kaputt.gltf"), "{\"asset\": {\"version\": ");
        const auto model = asset::load_gltf(dir.file("kaputt.gltf"));
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().find("kaputt.gltf") != std::string::npos);
    }
    SECTION("index outside the vertices") {
        Bytes data = triangle_buffer();
        const u16 bad = 7;
        std::memcpy(data.data() + 98, &bad, sizeof(bad));
        const std::string bad_buffer =
            R"([{"byteLength": 104, "uri": "data:application/octet-stream;base64,)" + base64(data) +
            R"("}])";
        write(dir.file("index.gltf"), triangle_json(bad_buffer, image));
        const auto model = asset::load_gltf(dir.file("index.gltf"));
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().find("Index 7") != std::string::npos);
    }
    SECTION("buffer shorter than its views") {
        Bytes data = triangle_buffer();
        data.resize(40);
        const std::string short_buffer =
            R"([{"byteLength": 104, "uri": "data:application/octet-stream;base64,)" + base64(data) +
            R"("}])";
        write(dir.file("kurz.gltf"), triangle_json(short_buffer, image));
        CHECK_FALSE(asset::load_gltf(dir.file("kurz.gltf")).has_value());
    }
    SECTION("undecodable image") {
        const std::string bad_image =
            R"([{"uri": "data:image/png;base64,)" + base64(Bytes(16, std::byte{7})) + R"("}])";
        write(dir.file("bild.gltf"), triangle_json(buffer, bad_image));
        const auto model = asset::load_gltf(dir.file("bild.gltf"));
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().find("Bild 0") != std::string::npos);
    }
}

TEST_CASE("Images decode to RGBA8", "[asset]") {
    const auto image = asset::decode_image(encode_png(1, 2, {1, 2, 3, 4, 5, 6, 7, 8}));
    REQUIRE(image.has_value());
    CHECK(image->width == 1);
    CHECK(image->height == 2);
    CHECK(std::to_integer<int>(image->pixels[4]) == 5);

    CHECK_FALSE(asset::decode_image({}).has_value());
    CHECK_FALSE(asset::decode_image(Bytes(32, std::byte{0})).has_value());
    CHECK_FALSE(asset::load_image("gibt/es/nicht.png").has_value());
}

TEST_CASE("Generated tangents follow the texture coordinates", "[asset]") {
    // Quadrat in der xy-Ebene, u läuft entlang -y, v entlang +x: Tangente -y. Bitangente +x
    // = cross(n, t) mit n = +z und t = -y, also Vorzeichen +1.
    std::array<asset::Vertex, 4> vertices{};
    const std::array<math::Vec3, 4> positions{math::Vec3{0, 0, 0}, math::Vec3{0, -1, 0},
                                              math::Vec3{1, -1, 0}, math::Vec3{1, 0, 0}};
    const std::array<math::Vec2, 4> uvs{math::Vec2{0, 0}, math::Vec2{1, 0}, math::Vec2{1, 1},
                                        math::Vec2{0, 1}};
    for (usize i = 0; i < 4; ++i) {
        vertices[i].position = positions[i];
        vertices[i].uv = uvs[i];
    }
    const std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};
    asset::generate_normals(vertices, indices);
    asset::generate_tangents(vertices, indices);
    for (const asset::Vertex& vertex : vertices) {
        CHECK(math::near(vertex.normal, math::Vec3{0.0f, 0.0f, 1.0f}, 1e-5f));
        CHECK(math::near(vertex.tangent, math::Vec4{0.0f, -1.0f, 0.0f, vertex.tangent.w}, 1e-5f));
    }

    // Gespiegelte UVs drehen das Vorzeichen der Bitangente.
    std::array<asset::Vertex, 4> mirrored = vertices;
    for (asset::Vertex& vertex : mirrored) {
        vertex.uv.y = 1.0f - vertex.uv.y;
    }
    asset::generate_tangents(mirrored, indices);
    CHECK(mirrored[0].tangent.w == -vertices[0].tangent.w);
}

TEST_CASE("Generated spheres face outwards", "[asset]") {
    const asset::Primitive sphere = asset::make_sphere(2.0f, 16, 8);
    CHECK(sphere.vertices.size() == 17 * 9);
    // Polkappen haben je ein Dreieck pro Segment, die anderen Ringe zwei.
    CHECK(sphere.indices.size() == static_cast<usize>(3 * (16 * 2 + 16 * 6 * 2)));
    for (const asset::Vertex& vertex : sphere.vertices) {
        CHECK(math::length(vertex.position) == Approx(2.0f));
        const math::Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
        CHECK(math::dot(tangent, vertex.normal) == Approx(0.0f).margin(1e-5f));
    }
    for (usize t = 0; t < sphere.indices.size(); t += 3) {
        const math::Vec3 a = sphere.vertices[sphere.indices[t]].position;
        const math::Vec3 b = sphere.vertices[sphere.indices[t + 1]].position;
        const math::Vec3 c = sphere.vertices[sphere.indices[t + 2]].position;
        // Die Flächennormale zeigt vom Mittelpunkt weg.
        CHECK(math::dot(math::cross(b - a, c - a), a + b + c) > 0.0f);
    }
}
