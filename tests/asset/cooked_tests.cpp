#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.asset;

using namespace ngine;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_cooked_" + std::to_string(random()));
        const std::u8string text = base.u8string();
        path_.assign(text.begin(), text.end());
        static_cast<void>(filesystem::create_directories(path_));
    }
    ~TempDir() {
        static_cast<void>(filesystem::remove(path_));
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(std::string_view name) const {
        return filesystem::normalize(filesystem::join(path_, name));
    }

private:
    std::string path_;
};

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const char c : text) {
        result.push_back(static_cast<std::byte>(c));
    }
    return result;
}

// Kleines Modell: ein Dreieck mit Material und einer Textur.
asset::CookedModel sample_model() {
    asset::CookedModel cooked;
    asset::Model& model = cooked.model;
    asset::Primitive primitive;
    primitive.vertices = {
        {.position = {0, 0, 0}, .normal = {0, 0, 1}, .tangent = {1, 0, 0, 1}, .uv = {0, 0}},
        {.position = {1, 0, 0}, .normal = {0, 0, 1}, .tangent = {1, 0, 0, 1}, .uv = {1, 0}},
        {.position = {0, 1, 0}, .normal = {0, 0, 1}, .tangent = {1, 0, 0, -1}, .uv = {0, 1}},
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;
    model.meshes.push_back({.name = "Dreieck", .primitives = {primitive}});
    asset::Material material;
    material.name = "Rot";
    material.base_color = {1.0f, 0.0f, 0.0f, 1.0f};
    material.base_color_texture = 0;
    material.alpha_mode = asset::AlphaMode::Mask;
    model.materials.push_back(material);
    model.textures.push_back({.image = 0, .sampler = {.wrap_u = asset::Wrap::ClampToEdge}});
    model.images.push_back({.name = "farbe", .width = 4, .height = 2, .pixels = {}});
    asset::Node node;
    node.name = "Wurzel";
    node.translation = {1.0f, 2.0f, 3.0f};
    node.mesh = 0;
    model.nodes.push_back(node);
    model.roots = {0};
    asset::TextureData bc7{.format = asset::TextureFormat::BC7,
                           .srgb = true,
                           .width = 4,
                           .height = 2,
                           .levels = 1,
                           .data = std::vector<std::byte>(16, std::byte{7})};
    cooked.textures.push_back(
        {.image = 0, .srgb = true, .ktx2 = bytes("ktx2-daten"), .bc7 = std::move(bc7)});
    cooked.sources.push_back(
        {.path = "/quelle/modell.gltf", .size = 12, .modified = 34, .hash = 56});
    return cooked;
}

} // namespace

TEST_CASE("Cooked models round trip through the binary format", "[asset]") {
    const asset::CookedModel original = sample_model();
    const std::vector<std::byte> data = asset::write_cooked_model(original);
    auto read = asset::read_cooked_model(data);
    REQUIRE(read);
    const asset::Model& model = read->model;
    REQUIRE(model.meshes.size() == 1);
    CHECK(model.meshes[0].name == "Dreieck");
    REQUIRE(model.meshes[0].primitives.size() == 1);
    const asset::Primitive& primitive = model.meshes[0].primitives[0];
    REQUIRE(primitive.vertices.size() == 3);
    CHECK(primitive.vertices[2].tangent.w == -1.0f);
    CHECK(primitive.vertices[1].uv.x == 1.0f);
    CHECK(primitive.indices == std::vector<u32>{0, 1, 2});
    CHECK(primitive.material == std::optional<u32>{0});
    REQUIRE(model.materials.size() == 1);
    CHECK(model.materials[0].name == "Rot");
    CHECK(model.materials[0].base_color_texture == std::optional<u32>{0});
    CHECK_FALSE(model.materials[0].normal_texture);
    CHECK(model.materials[0].alpha_mode == asset::AlphaMode::Mask);
    REQUIRE(model.textures.size() == 1);
    CHECK(model.textures[0].sampler.wrap_u == asset::Wrap::ClampToEdge);
    REQUIRE(model.images.size() == 1);
    CHECK(model.images[0].width == 4);
    CHECK(model.images[0].pixels.empty());
    REQUIRE(model.nodes.size() == 1);
    CHECK(model.nodes[0].translation.z == 3.0f);
    CHECK(model.roots == std::vector<u32>{0});
    REQUIRE(read->textures.size() == 1);
    CHECK(read->textures[0].srgb);
    CHECK(read->textures[0].ktx2 == bytes("ktx2-daten"));
    const asset::TextureData& bc7 = read->textures[0].bc7;
    CHECK(bc7.format == asset::TextureFormat::BC7);
    CHECK(bc7.srgb);
    CHECK(bc7.width == 4);
    CHECK(bc7.height == 2);
    CHECK(bc7.levels == 1);
    CHECK(bc7.data == original.textures[0].bc7.data);
    REQUIRE(read->sources.size() == 1);
    CHECK(read->sources[0] == original.sources[0]);
}

TEST_CASE("Damaged cooked models are rejected", "[asset]") {
    std::vector<std::byte> data = asset::write_cooked_model(sample_model());
    // Abgeschnitten.
    CHECK_FALSE(asset::read_cooked_model(std::span{data}.first(data.size() / 2)));
    // Falsche Kennung am Anfang.
    std::vector<std::byte> wrong = data;
    wrong[0] = std::byte{'X'};
    CHECK_FALSE(asset::read_cooked_model(wrong));
    // Ein Index zeigt hinter die Eckpunkte.
    asset::CookedModel broken = sample_model();
    broken.model.meshes[0].primitives[0].indices = {0, 1, 7};
    CHECK_FALSE(asset::read_cooked_model(asset::write_cooked_model(broken)));
    // BC7-Fassung passt nicht zur Größe.
    asset::CookedModel wrong_bc7 = sample_model();
    wrong_bc7.textures[0].bc7.levels = 3;
    CHECK_FALSE(asset::read_cooked_model(asset::write_cooked_model(wrong_bc7)));
    // Ohne BC7-Fassung ist es gültig.
    asset::CookedModel no_bc7 = sample_model();
    no_bc7.textures[0].bc7 = {};
    CHECK(asset::read_cooked_model(asset::write_cooked_model(no_bc7)));
    CHECK_FALSE(asset::read_cooked_model({}));
}

TEST_CASE("Source stamps notice changed files", "[asset]") {
    const TempDir dir;
    const std::string file = dir.file("quelle.txt");
    REQUIRE(filesystem::write_text(file, "erster Stand"));
    auto stamp = asset::stamp_file(file);
    REQUIRE(stamp);
    CHECK(stamp->size == 12);
    CHECK(stamp->hash == asset::content_hash(bytes("erster Stand")));
    std::vector<asset::SourceStamp> stamps{*stamp};
    CHECK(asset::stamps_current(stamps));

    // Nur die Änderungszeit anders (z. B. nach git checkout): gilt als gleich, Stempel erneuert.
    std::vector<asset::SourceStamp> touched{*stamp};
    touched[0].modified -= 1000;
    bool refreshed = false;
    CHECK(asset::stamps_current(touched, &refreshed));
    CHECK(refreshed);
    CHECK(touched[0].modified == stamp->modified);

    // Inhalt anders.
    REQUIRE(filesystem::write_text(file, "zweiter Stand"));
    CHECK_FALSE(asset::stamps_current(stamps));
    // Datei weg.
    REQUIRE(filesystem::remove(file));
    CHECK_FALSE(asset::stamps_current(stamps));
}

TEST_CASE("Asset cache returns cooked models only while sources are unchanged", "[asset]") {
    const TempDir dir;
    const std::string source = dir.file("assets/modell.gltf");
    REQUIRE(filesystem::write_text(source, "{}"));
    const asset::AssetCache cache{dir.file("assets/.cache")};
    CHECK(cache.enabled());
    CHECK_FALSE(asset::AssetCache{}.enabled());
    constexpr asset::AssetId id = 0x1234abcd;

    // Noch nichts gekocht.
    auto missing = cache.load_model(id);
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());
    CHECK_FALSE(cache.model_current(id));

    asset::CookedModel model = sample_model();
    auto stamp = asset::stamp_file(source);
    REQUIRE(stamp);
    model.sources = {*stamp};
    REQUIRE(cache.store_model(id, model));
    CHECK(filesystem::exists(cache.model_path(id)));
    CHECK(cache.model_current(id));
    auto loaded = cache.load_model(id);
    REQUIRE(loaded);
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->model.meshes[0].name == "Dreieck");

    // Quelle geändert: Der Eintrag gilt nicht mehr.
    REQUIRE(filesystem::write_text(source, "{ }"));
    CHECK_FALSE(cache.model_current(id));
    auto stale = cache.load_model(id);
    REQUIRE(stale);
    CHECK_FALSE(stale->has_value());

    // Kaputte Datei oder andere Version: gilt wie veraltet, Kochen ersetzt sie.
    REQUIRE(filesystem::write_text(cache.model_path(id), "kaputt"));
    auto broken = cache.load_model(id);
    REQUIRE(broken);
    CHECK_FALSE(broken->has_value());
    CHECK(asset::default_cache_dir(dir.file("assets")) ==
          filesystem::join(dir.file("assets"), ".cache"));
}

TEST_CASE("Textures from images contain all mip levels", "[asset]") {
    asset::Image image{.name = "bild", .width = 4, .height = 2, .pixels = {}};
    image.pixels.resize(4 * 2 * 4, std::byte{200});
    const asset::TextureData full = asset::texture_from_image(image, true, true);
    CHECK(full.format == asset::TextureFormat::RGBA8);
    CHECK(full.srgb);
    CHECK(full.levels == 3); // 4x2, 2x1, 1x1
    CHECK(full.data.size() == (8 + 2 + 1) * 4);
    CHECK(full.data.size() == asset::texture_size(full.format, 4, 2, 3));
    const asset::TextureData single = asset::texture_from_image(image, false, false);
    CHECK(single.levels == 1);
    CHECK(single.data.size() == 32);
    // BC7: Blöcke aus 4x4 Pixeln zu 16 Bytes, auch für kleine Stufen.
    CHECK(asset::texture_size(asset::TextureFormat::BC7, 8, 8, 4) == (4 + 1 + 1 + 1) * 16);
    CHECK_FALSE(asset::decode_ktx2(bytes("keine ktx2-datei"), asset::TextureFormat::RGBA8));
}
