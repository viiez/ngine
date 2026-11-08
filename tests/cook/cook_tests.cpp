#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.asset;
import ngine.cook;

using namespace ngine;
using Catch::Matchers::WithinAbs;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_cook_" + std::to_string(random()));
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
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

// Bild mit Verlauf, damit die Kodierung etwas zu tun hat.
asset::Image gradient(u32 width, u32 height) {
    asset::Image image{.name = "verlauf", .width = width, .height = height, .pixels = {}};
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            image.pixels.push_back(static_cast<std::byte>(x * 255 / (width - 1)));
            image.pixels.push_back(static_cast<std::byte>(y * 255 / (height - 1)));
            image.pixels.push_back(std::byte{96});
            image.pixels.push_back(std::byte{255});
        }
    }
    return image;
}

void copy(const std::string& from, const std::string& to) {
    auto data = filesystem::read_file(from);
    REQUIRE(data);
    REQUIRE(filesystem::write_file(to, *data));
}

} // namespace

TEST_CASE("MikkTSpace tangents follow the texture u direction", "[cook]") {
    // Quadrat in der xy-Ebene, u läuft entlang -x (gespiegelt), v entlang +y.
    asset::Primitive primitive;
    const math::Vec3 normal{0.0f, 0.0f, 1.0f};
    primitive.vertices = {
        {.position = {0, 0, 0}, .normal = normal, .tangent = {}, .uv = {1, 0}},
        {.position = {1, 0, 0}, .normal = normal, .tangent = {}, .uv = {0, 0}},
        {.position = {1, 1, 0}, .normal = normal, .tangent = {}, .uv = {0, 1}},
        {.position = {0, 1, 0}, .normal = normal, .tangent = {}, .uv = {1, 1}},
    };
    primitive.indices = {0, 1, 2, 0, 2, 3};
    primitive.generated_tangents = true;
    cook::generate_tangents(primitive);
    CHECK_FALSE(primitive.generated_tangents);
    // Keine Naht: Die Eckpunkte bleiben geteilt.
    CHECK(primitive.vertices.size() == 4);
    REQUIRE(primitive.indices.size() == 6);
    for (const asset::Vertex& vertex : primitive.vertices) {
        CHECK_THAT(vertex.tangent.x, WithinAbs(-1.0, 1e-4));
        CHECK_THAT(vertex.tangent.y, WithinAbs(0.0, 1e-4));
        CHECK_THAT(vertex.tangent.z, WithinAbs(0.0, 1e-4));
        // Bitangente = w · (N × T) = w · (0, -1, 0) muss entlang +v (+y) zeigen.
        CHECK(vertex.tangent.w == -1.0f);
    }
}

TEST_CASE("MikkTSpace splits vertices at mirrored texture seams", "[cook]") {
    // Zwei Dreiecke teilen eine Kante, das zweite hat gespiegelte u-Koordinaten.
    asset::Primitive primitive;
    const math::Vec3 normal{0.0f, 0.0f, 1.0f};
    primitive.vertices = {
        {.position = {0, 0, 0}, .normal = normal, .tangent = {}, .uv = {0, 0}},
        {.position = {1, 0, 0}, .normal = normal, .tangent = {}, .uv = {1, 0}},
        {.position = {0, 1, 0}, .normal = normal, .tangent = {}, .uv = {0, 1}},
        {.position = {-1, 0, 0}, .normal = normal, .tangent = {}, .uv = {1, 0}},
    };
    primitive.indices = {0, 1, 2, 0, 2, 3};
    cook::generate_tangents(primitive);
    // Eckpunkte 0 und 2 brauchen zwei verschiedene Tangenten.
    CHECK(primitive.vertices.size() == 6);
    for (const u32 index : primitive.indices) {
        REQUIRE(index < primitive.vertices.size());
        const math::Vec4 t = primitive.vertices[index].tangent;
        CHECK_THAT(std::abs(t.x), WithinAbs(1.0, 1e-4));
    }
}

TEST_CASE("KTX2 without Basis Universal keeps RGBA8 levels", "[cook]") {
    const asset::Image image = gradient(16, 8);
    auto encoded = cook::encode_ktx2(image, true, true, {.basis = false});
    REQUIRE(encoded);
    auto decoded = asset::decode_ktx2(*encoded, asset::TextureFormat::BC7);
    REQUIRE(decoded);
    // Unkomprimiert bleibt RGBA8, auch wenn BC7 gewünscht ist.
    CHECK(decoded->format == asset::TextureFormat::RGBA8);
    CHECK(decoded->srgb);
    CHECK(decoded->width == 16);
    CHECK(decoded->height == 8);
    CHECK(decoded->levels == 5);
    const asset::TextureData reference = asset::texture_from_image(image, true, true);
    CHECK(decoded->data == reference.data);
}

TEST_CASE("KTX2 with UASTC transcodes to BC7 and RGBA8", "[cook]") {
    const asset::Image image = gradient(32, 32);
    auto encoded = cook::encode_ktx2(image, false, true, {.uastc_level = 0, .threads = 1});
    REQUIRE(encoded);
    // zstd hält die Datei kleiner als die Rohdaten.
    CHECK(encoded->size() < 32 * 32 * 4);

    auto bc7 = asset::decode_ktx2(*encoded, asset::TextureFormat::BC7);
    REQUIRE(bc7);
    CHECK(bc7->format == asset::TextureFormat::BC7);
    CHECK_FALSE(bc7->srgb);
    CHECK(bc7->levels == 6);
    CHECK(bc7->data.size() == asset::texture_size(asset::TextureFormat::BC7, 32, 32, 6));

    auto rgba = asset::decode_ktx2(*encoded, asset::TextureFormat::RGBA8);
    REQUIRE(rgba);
    CHECK(rgba->format == asset::TextureFormat::RGBA8);
    REQUIRE(rgba->data.size() == asset::texture_size(asset::TextureFormat::RGBA8, 32, 32, 6));
    // Fast verlustfrei: Ecke oben rechts ist rot, unten links grün.
    const auto pixel = [&](u32 x, u32 y, u32 channel) {
        return std::to_integer<int>(rgba->data[(y * 32 + x) * 4 + channel]);
    };
    CHECK(std::abs(pixel(31, 0, 0) - 255) <= 16);
    CHECK(std::abs(pixel(0, 31, 1) - 255) <= 16);
    CHECK(std::abs(pixel(16, 16, 2) - 96) <= 16);
}

TEST_CASE("KTX2 with ETC1S encodes and transcodes", "[cook]") {
    const asset::Image image = gradient(16, 16);
    auto encoded =
        cook::encode_ktx2(image, true, false, {.uastc = false, .etc1s_quality = 64, .threads = 1});
    REQUIRE(encoded);
    auto rgba = asset::decode_ktx2(*encoded, asset::TextureFormat::RGBA8);
    REQUIRE(rgba);
    CHECK(rgba->srgb);
    CHECK(rgba->levels == 1);
    CHECK(rgba->data.size() == 16 * 16 * 4);
}

TEST_CASE("Cooking a glTF model stores geometry, KTX2 textures and stamps", "[cook]") {
    const TempDir dir;
    for (const char* name : {"kiste.gltf", "kiste.bin", "kiste.png"}) {
        copy(filesystem::join(NGINE_ASSET_SOURCE_DIR "/modelle", name), dir.file(name));
    }
    auto cooked = cook::cook_model(dir.file("kiste.gltf"), {.textures = {.uastc_level = 0}});
    REQUIRE(cooked);
    REQUIRE(cooked->model.meshes.size() == 1);
    const asset::Primitive& primitive = cooked->model.meshes[0].primitives[0];
    CHECK_FALSE(primitive.vertices.empty());
    CHECK_FALSE(primitive.generated_tangents);
    // Bilder ohne Pixel, dafür als KTX2.
    REQUIRE(cooked->model.images.size() == 1);
    CHECK(cooked->model.images[0].pixels.empty());
    REQUIRE(cooked->textures.size() == 1);
    CHECK(cooked->textures[0].srgb);
    CHECK(asset::decode_ktx2(cooked->textures[0].ktx2, asset::TextureFormat::RGBA8));
    // Die BC7-Fassung liegt schon fertig bei.
    const asset::TextureData& bc7 = cooked->textures[0].bc7;
    CHECK(bc7.format == asset::TextureFormat::BC7);
    CHECK(bc7.width == cooked->model.images[0].width);
    CHECK(bc7.levels > 1);
    CHECK(bc7.data.size() ==
          asset::texture_size(asset::TextureFormat::BC7, bc7.width, bc7.height, bc7.levels));
    // Stempel für glTF, Puffer und Bild.
    CHECK(cooked->sources.size() == 3);
    CHECK(asset::stamps_current(cooked->sources));
    CHECK_FALSE(cook::cook_model(dir.file("fehlt.gltf")));
}

TEST_CASE("Cooking a folder only regenerates what changed", "[cook]") {
    const TempDir dir;
    for (const char* name : {"kiste.gltf", "kiste.bin", "kiste.png"}) {
        copy(filesystem::join(NGINE_ASSET_SOURCE_DIR "/modelle", name), dir.file(name));
    }
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);
    const asset::AssetCache cache{asset::default_cache_dir(database->root())};
    const cook::CookSettings settings{.textures = {.uastc_level = 0}};

    const cook::CookReport first = cook::cook_all(*database, cache, settings);
    CHECK(first.cooked == 1);
    CHECK(first.failed == 0);
    const asset::AssetId id = database->find_path("kiste.gltf")->id;
    CHECK(cache.model_current(id));

    const cook::CookReport second = cook::cook_all(*database, cache, settings);
    CHECK(second.cooked == 0);
    CHECK(second.current == 1);

    // Die Textur ändert sich: Das Modell wird neu gekocht.
    auto png = filesystem::read_file(dir.file("kiste.png"));
    REQUIRE(png);
    png->push_back(std::byte{0});
    REQUIRE(filesystem::write_file(dir.file("kiste.png"), *png));
    CHECK_FALSE(cache.model_current(id));
    const cook::CookReport third = cook::cook_all(*database, cache, settings);
    CHECK(third.cooked == 1);

    const cook::CookReport forced = cook::cook_all(*database, cache, settings, true);
    CHECK(forced.cooked == 1);
    CHECK(forced.current == 0);
}
