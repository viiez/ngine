#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

import ngine.core;
import ngine.filesystem;

using namespace ngine;

namespace {

// Eigener Ordner pro Test unter dem temporären Verzeichnis; wird am Ende gelöscht.
class TempDir {
public:
    TempDir() {
        std::random_device random;
        const auto name = "ngine_tests_" + std::to_string(random());
        const auto base = std::filesystem::temp_directory_path() / name;
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
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

} // namespace

TEST_CASE("Text files round trip and create missing directories", "[filesystem]") {
    const TempDir dir;
    const auto path = dir.file("unter/ordner/szene.json");
    REQUIRE(filesystem::write_text(path, "{\"hallo\": \"Welt\"}"));
    CHECK(filesystem::exists(path));

    const auto text = filesystem::read_text(path);
    REQUIRE(text);
    CHECK(*text == "{\"hallo\": \"Welt\"}");
}

TEST_CASE("Binary files keep every byte", "[filesystem]") {
    const TempDir dir;
    const auto path = dir.file("daten.bin");
    std::vector<std::byte> data;
    for (int i = 0; i < 512; ++i) {
        data.push_back(static_cast<std::byte>(i % 256));
    }
    REQUIRE(filesystem::write_file(path, data));

    const auto read = filesystem::read_file(path);
    REQUIRE(read);
    CHECK(*read == data);
}

TEST_CASE("Writing replaces an existing file and leaves no temporary file", "[filesystem]") {
    const TempDir dir;
    const auto path = dir.file("datei.txt");
    REQUIRE(filesystem::write_text(path, "alt und lang"));
    REQUIRE(filesystem::write_text(path, "neu"));
    CHECK(filesystem::read_text(path).value_or("") == "neu");
    CHECK_FALSE(filesystem::exists(path + ".tmp"));
}

TEST_CASE("Empty files can be written and read", "[filesystem]") {
    const TempDir dir;
    const auto path = dir.file("leer");
    REQUIRE(filesystem::write_text(path, ""));
    const auto text = filesystem::read_text(path);
    REQUIRE(text);
    CHECK(text->empty());
}

TEST_CASE("Reading a missing file reports an error", "[filesystem]") {
    const TempDir dir;
    const auto result = filesystem::read_text(dir.file("gibt-es-nicht.txt"));
    REQUIRE_FALSE(result);
    CHECK(result.error().find("gibt-es-nicht.txt") != std::string::npos);
}

TEST_CASE("Paths with non-ASCII characters work", "[filesystem]") {
    const TempDir dir;
    // UTF-8 für "Größe/Übung.txt"; unter Windows darf daraus kein Codepage-Salat werden.
    const auto path = dir.file("Gr\xC3\xB6\xC3\x9F"
                               "e/\xC3\x9C"
                               "bung.txt");
    REQUIRE(filesystem::write_text(path, "ok"));
    CHECK(filesystem::read_text(path).value_or("") == "ok");
}

TEST_CASE("Removing deletes directories with content", "[filesystem]") {
    const TempDir dir;
    const auto folder = dir.file("weg");
    REQUIRE(filesystem::write_text(filesystem::join(folder, "a.txt"), "a"));
    REQUIRE(filesystem::remove(folder));
    CHECK_FALSE(filesystem::exists(folder));
}

TEST_CASE("User data directory exists and contains the application", "[filesystem]") {
    const auto dir = filesystem::user_data_dir("NGineTests");
    REQUIRE(dir);
    CHECK(filesystem::exists(*dir));
    CHECK(dir->find("NGineTests") != std::string::npos);
    static_cast<void>(filesystem::remove(*dir));
}

TEST_CASE("Paths normalize to absolute forward-slash form", "[filesystem]") {
    const TempDir dir;
    const auto base = filesystem::normalize(dir.path());
    CHECK(base.find('\\') == std::string::npos);
    CHECK(filesystem::normalize(dir.path() + "/a/../b/") == base + "/b");
    CHECK(filesystem::relative(base + "/b/c.txt", base) == "b/c.txt");
    CHECK(filesystem::file_name(base + "/b/c.TXT") == "c.TXT");
    CHECK(filesystem::extension(base + "/b/c.TXT") == ".txt");
    CHECK(filesystem::parent_path(base + "/b/c.txt") == base + "/b");
}

TEST_CASE("Directories list recursively, sorted and without hidden entries", "[filesystem]") {
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("b.txt"), "bb"));
    REQUIRE(filesystem::write_text(dir.file("a/c.txt"), "c"));
    REQUIRE(filesystem::write_text(dir.file(".git/config"), "x"));
    REQUIRE(filesystem::write_text(dir.file(".versteckt"), "x"));

    const auto entries = filesystem::list_directory(dir.path(), true);
    REQUIRE(entries);
    REQUIRE(entries->size() == 3);
    CHECK((*entries)[0].path == "a");
    CHECK((*entries)[0].directory);
    CHECK((*entries)[1].path == "a/c.txt");
    CHECK((*entries)[2].path == "b.txt");
    CHECK((*entries)[2].size == 2);
    CHECK_FALSE(filesystem::list_directory(dir.file("fehlt"), true));
}

TEST_CASE("Executable directory exists", "[filesystem]") {
    const auto dir = filesystem::executable_dir();
    REQUIRE(dir);
    CHECK(filesystem::is_directory(*dir));
}

TEST_CASE("File watcher reports settled changes once", "[filesystem]") {
    using filesystem::FileChange;
    const TempDir dir;
    REQUIRE(filesystem::write_text(dir.file("a.txt"), "a"));
    filesystem::FileWatcher watcher{dir.path()};
    CHECK(watcher.scan().empty()); // erster Durchlauf merkt sich nur den Stand

    REQUIRE(filesystem::write_text(dir.file("sub/b.txt"), "b"));
    REQUIRE(filesystem::write_text(dir.file("c.tmp"), "c"));
    REQUIRE(filesystem::write_text(dir.file(".d"), "d"));
    CHECK(watcher.scan().empty()); // noch nicht ruhig
    auto events = watcher.scan();
    REQUIRE(events.size() == 1);
    CHECK(events[0].path == filesystem::normalize(dir.file("sub/b.txt")));
    CHECK(events[0].change == FileChange::Added);
    CHECK(watcher.scan().empty());

    REQUIRE(filesystem::write_text(dir.file("a.txt"), "aaaa"));
    CHECK(watcher.scan().empty());
    events = watcher.scan();
    REQUIRE(events.size() == 1);
    CHECK(events[0].path == filesystem::normalize(dir.file("a.txt")));
    CHECK(events[0].change == FileChange::Modified);

    REQUIRE(filesystem::remove(dir.file("sub/b.txt")));
    events = watcher.scan();
    REQUIRE(events.size() == 1);
    CHECK(events[0].change == FileChange::Removed);
}

TEST_CASE("File watcher on a missing directory stays quiet", "[filesystem]") {
    const TempDir dir;
    filesystem::FileWatcher watcher{dir.file("fehlt")};
    CHECK(watcher.scan().empty());
    CHECK(watcher.scan().empty());
}
