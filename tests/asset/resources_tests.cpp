#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;
import ngine.jobs;
import ngine.asset;

using namespace ngine;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto base =
            std::filesystem::temp_directory_path() / ("ngine_res_" + std::to_string(random()));
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
        return filesystem::join(path_, name);
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

// Ressource für die Tests: der Text einer Datei, die als Shader zählt (.slang), plus der Text
// einer Datei, die in der ersten Zeile nach „include:“ steht (Abhängigkeit).
struct Text {
    std::string value;
};

void add_text_loader(asset::ResourceManager& resources, std::atomic<int>* reads = nullptr) {
    resources.add_loader<Text, std::string>(
        [reads](const asset::LoadContext& context) -> asset::Result<std::string> {
            if (reads != nullptr) {
                ++*reads;
            }
            auto text = filesystem::read_text(context.path);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (text->starts_with("include:")) {
                const auto other =
                    filesystem::join(filesystem::parent_path(context.path), text->substr(8));
                context.dependencies->push_back(filesystem::normalize(other));
                auto included = filesystem::read_text(other);
                if (!included) {
                    return std::unexpected(included.error());
                }
                return *included;
            }
            return *text;
        },
        [](std::string& text) -> asset::Result<Text> {
            if (text == "fehler") {
                return std::unexpected(std::string{"absichtlich kaputt"});
            }
            return Text{text};
        });
}

} // namespace

TEST_CASE("Asset meta files round trip", "[asset]") {
    const auto text = asset::write_meta(123456789012345ull);
    const auto id = asset::parse_meta(text);
    REQUIRE(id);
    CHECK(*id == 123456789012345ull);
    CHECK_FALSE(asset::parse_meta("{}"));
    CHECK_FALSE(asset::parse_meta(R"({"format":"ngine.meta","version":1,"id":0})"));
    CHECK_FALSE(asset::parse_meta("kein json"));
}

TEST_CASE("Asset types follow the file extension", "[asset]") {
    CHECK(asset::type_of("modelle/kiste.gltf") == asset::AssetType::Model);
    CHECK(asset::type_of("a.GLB") == asset::AssetType::Model);
    CHECK(asset::type_of("holz.png") == asset::AssetType::Texture);
    CHECK(asset::type_of("pbr.slang") == asset::AssetType::Shader);
    CHECK(asset::type_of("level.json") == asset::AssetType::Scene);
    CHECK(asset::type_of("kiste.bin") == asset::AssetType::Unknown);
    CHECK(asset::type_of("kiste.gltf.meta") == asset::AssetType::Unknown);
    CHECK(asset::generated_id("a") == asset::generated_id("a"));
    CHECK(asset::generated_id("a") != asset::generated_id("b"));
    CHECK(asset::random_id() != 0);
}

TEST_CASE("Asset database creates stable ids in meta files", "[asset]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("modelle/kiste.gltf"), "{}"));
    REQUIRE(filesystem::write_text(dir.file("modelle/kiste.bin"), "x"));
    REQUIRE(filesystem::write_text(dir.file("holz.png"), "x"));

    asset::AssetId kiste = 0;
    {
        auto database = asset::AssetDatabase::open(dir.path());
        REQUIRE(database);
        REQUIRE(database->assets().size() == 2);
        CHECK(database->assets()[0].path == "holz.png");
        CHECK(database->assets()[1].path == "modelle/kiste.gltf");
        CHECK(filesystem::exists(dir.file("modelle/kiste.gltf.meta")));
        CHECK_FALSE(filesystem::exists(dir.file("modelle/kiste.bin.meta")));
        const auto* info = database->find_path("modelle/kiste.gltf");
        REQUIRE(info != nullptr);
        CHECK(info->type == asset::AssetType::Model);
        kiste = info->id;
        CHECK(database->find(kiste) == info);
        CHECK(database->find_path(dir.file("modelle/kiste.gltf")) == info);
    }

    // Neu geöffnet: dieselbe Kennung. Verschoben samt .meta-Datei: auch.
    REQUIRE(filesystem::rename(dir.file("modelle/kiste.gltf"), dir.file("kiste.gltf")));
    REQUIRE(filesystem::rename(dir.file("modelle/kiste.gltf.meta"), dir.file("kiste.gltf.meta")));
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);
    const auto* moved = database->find(kiste);
    REQUIRE(moved != nullptr);
    CHECK(moved->path == "kiste.gltf");

    // Kopierte .meta-Datei: die Kopie bekommt eine neue Kennung.
    REQUIRE(filesystem::write_text(dir.file("kopie.gltf"), "{}"));
    const auto meta = filesystem::read_text(dir.file("kiste.gltf.meta"));
    REQUIRE(meta);
    REQUIRE(filesystem::write_text(dir.file("kopie.gltf.meta"), *meta));
    const auto changes = database->refresh();
    REQUIRE(changes);
    CHECK(changes->added == 1);
    CHECK(changes->created_meta == 1);
    const auto* copy = database->find_path("kopie.gltf");
    REQUIRE(copy != nullptr);
    CHECK(copy->id != kiste);
    CHECK(database->find(kiste)->path == "kiste.gltf");

    REQUIRE(filesystem::remove(dir.file("kopie.gltf")));
    const auto removed = database->refresh();
    REQUIRE(removed);
    CHECK(removed->removed == 1);
}

TEST_CASE("Resources load once, report errors and unload when unused", "[asset]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("a.slang"), "hallo"));
    REQUIRE(filesystem::write_text(dir.file("kaputt.slang"), "fehler"));
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);
    const auto a = database->find_path("a.slang")->id;
    const auto broken = database->find_path("kaputt.slang")->id;

    std::atomic<int> reads = 0;
    asset::ResourceManager resources{&*database};
    resources.keep_updates = 2;
    add_text_loader(resources, &reads);

    {
        auto handle = resources.load<Text>(a);
        auto again = resources.load<Text>(a);
        CHECK(reads == 1); // ohne JobSystem sofort gelesen, fertig erst in update()
        CHECK(handle.state() == asset::ResourceState::Loading);
        resources.update();
        REQUIRE(handle.state() == asset::ResourceState::Ready);
        CHECK(handle.get()->value == "hallo");
        CHECK(again.get() == handle.get());
        CHECK(handle.version() == 1);

        auto failed = resources.load<Text>(broken);
        resources.update();
        CHECK(failed.state() == asset::ResourceState::Failed);
        CHECK(failed.get() == nullptr);
        CHECK_FALSE(failed.error().empty());

        auto missing = resources.load<Text>(42);
        CHECK(missing.state() == asset::ResourceState::Failed);
        CHECK(resources.stats().resident == 1);
    }
    // Niemand hält sie mehr: nach keep_updates weg.
    for (int i = 0; i < 4; ++i) {
        resources.update();
    }
    CHECK_FALSE(resources.find<Text>(a));
    CHECK(resources.stats().resident == 0);
}

TEST_CASE("Resources reload when their files or dependencies change", "[asset]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("a.slang"), "include:teil.txt"));
    REQUIRE(filesystem::write_text(dir.file("teil.txt"), "eins"));
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);
    const auto a = database->find_path("a.slang")->id;

    asset::ResourceManager resources{&*database};
    add_text_loader(resources);
    auto handle = resources.load<Text>(a);
    resources.update();
    REQUIRE(handle.get() != nullptr);
    CHECK(handle.get()->value == "eins");

    // Unbeteiligte Datei: nichts passiert.
    CHECK(resources.reload_files(
              std::vector<std::string>{filesystem::normalize(dir.file("anderes.txt"))}) == 0);

    REQUIRE(filesystem::write_text(dir.file("teil.txt"), "zwei"));
    CHECK(resources.reload_files(
              std::vector<std::string>{filesystem::normalize(dir.file("teil.txt"))}) == 1);
    resources.update();
    CHECK(handle.get()->value == "zwei");
    CHECK(handle.version() == 2);

    // Kaputtes Neuladen behält den alten Stand.
    REQUIRE(filesystem::write_text(dir.file("teil.txt"), "fehler"));
    resources.reload(a);
    resources.update();
    CHECK(handle.state() == asset::ResourceState::Ready);
    CHECK(handle.get()->value == "zwei");
    CHECK_FALSE(handle.error().empty());
    CHECK(resources.stats().reloads == 2);
}

TEST_CASE("Generated resources stay until removed", "[asset]") {
    asset::ResourceManager resources;
    const auto id = asset::generated_id("test.text");
    resources.add(id, Text{"erzeugt"});
    resources.keep_updates = 0;
    for (int i = 0; i < 3; ++i) {
        resources.update();
    }
    auto handle = resources.find<Text>(id);
    REQUIRE(handle.get() != nullptr);
    CHECK(handle.get()->value == "erzeugt");
    // reload() lässt erzeugte Ressourcen in Ruhe.
    resources.reload(id);
    CHECK(handle.state() == asset::ResourceState::Ready);

    handle = {};
    resources.remove(id);
    resources.update();
    CHECK_FALSE(resources.find<Text>(id));
}

TEST_CASE("Resources load in the background on the job system", "[asset][jobs]") {
    const TempDir dir;
    constexpr int count = 32;
    for (int i = 0; i < count; ++i) {
        REQUIRE(filesystem::write_text(dir.file("datei" + std::to_string(i) + ".slang"),
                                       std::to_string(i)));
    }
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);

    jobs::JobSystem jobs{{.worker_count = 3}};
    asset::ResourceManager resources{&*database, &jobs};
    add_text_loader(resources);
    std::vector<asset::Handle<Text>> handles;
    for (const auto& info : database->assets()) {
        handles.push_back(resources.load<Text>(info.id));
    }
    // Während geladen wird, neu laden: veraltete Ergebnisse dürfen nichts kaputt machen.
    resources.reload(database->assets()[0].id);
    resources.wait();
    for (usize i = 0; i < handles.size(); ++i) {
        REQUIRE(handles[i].state() == asset::ResourceState::Ready);
        const auto* info = database->find(handles[i].id());
        CHECK(filesystem::read_text(info->file).value() == handles[i].get()->value);
    }
    CHECK(resources.stats().resident == count);
}

TEST_CASE("Mip chains halve each level and average in linear space", "[asset]") {
    asset::Image image{.name = "test", .width = 4, .height = 2, .pixels = {}};
    for (u32 i = 0; i < 8; ++i) {
        // Schachbrett aus Schwarz und Weiß, Alpha halb.
        const auto value = static_cast<std::byte>(((i + i / 4) % 2) * 255);
        image.pixels.insert(image.pixels.end(), {value, value, value, std::byte{128}});
    }
    CHECK(asset::mip_count(4, 2) == 3);
    CHECK(asset::mip_count(1, 1) == 1);
    CHECK(asset::mip_count(5, 3) == 3);

    const auto linear = asset::mip_chain(image, false);
    REQUIRE(linear.size() == (8 + 2 + 1) * 4);
    // Stufe 1: 2x1, je Mittel aus 2 schwarzen und 2 weißen Texeln.
    CHECK(std::to_integer<u32>(linear[32]) == 128);
    CHECK(std::to_integer<u32>(linear[35]) == 128);
    CHECK(std::to_integer<u32>(linear[40]) == 128);

    // In sRGB ist das Mittel aus Schwarz und Weiß heller (0.5 linear ist etwa 188).
    const auto srgb = asset::mip_chain(image, true);
    REQUIRE(srgb.size() == linear.size());
    CHECK(std::to_integer<u32>(srgb[32]) == 188);
    CHECK(std::to_integer<u32>(srgb[35]) == 128); // Alpha bleibt linear

    CHECK(asset::mip_chain(image, false, 1).size() == 8 * 4);
    CHECK(asset::mip_chain(asset::Image{}, false).empty());

    // Ungerade Größe wie die GPU: 3x1 -> 1x1.
    asset::Image odd{.name = "odd", .width = 3, .height = 1, .pixels = {}};
    odd.pixels.resize(12, std::byte{60});
    CHECK(asset::mip_chain(odd, false).size() == (3 + 1) * 4);
}

TEST_CASE("Reloaded resources wait until their loader reports ready", "[asset]") {
    // TD-30: Ein neu geladener Wert ersetzt den alten erst, wenn er fertig ist (z. B. die GPU
    // ihre Texturen hochgeladen hat); bis dahin zeigen Handles auf den alten Stand.
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("a.slang"), "eins"));
    auto database = asset::AssetDatabase::open(dir.path());
    REQUIRE(database);
    const auto a = database->find_path("a.slang")->id;

    asset::ResourceManager resources{&*database};
    bool ready = false;
    resources.add_loader<Text, std::string>(
        [](const asset::LoadContext& context) -> asset::Result<std::string> {
            CHECK(context.id != 0);
            auto text = filesystem::read_text(context.path);
            if (!text) {
                return std::unexpected(text.error());
            }
            return *text;
        },
        [](std::string& text) -> asset::Result<Text> { return Text{text}; },
        [&ready](const Text&) { return ready; });

    auto handle = resources.load<Text>(a);
    resources.update();
    // Erstes Laden: noch nicht bereit, also noch nichts zu sehen.
    CHECK(handle.get() == nullptr);
    CHECK(handle.state() == asset::ResourceState::Loading);
    CHECK(resources.stats().pending == 1);
    ready = true;
    resources.update();
    REQUIRE(handle.get() != nullptr);
    CHECK(handle.get()->value == "eins");
    CHECK(handle.version() == 1);
    CHECK(resources.stats().pending == 0);

    ready = false;
    REQUIRE(filesystem::write_text(dir.file("a.slang"), "zwei"));
    resources.reload(a);
    resources.update();
    resources.update();
    // Der alte Stand bleibt sichtbar, bis der neue bereit ist.
    CHECK(handle.get()->value == "eins");
    CHECK(handle.version() == 1);
    ready = true;
    resources.update();
    CHECK(handle.get()->value == "zwei");
    CHECK(handle.version() == 2);
}
