module;

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

module ngine.filesystem;

import ngine.core;
import :platform;

namespace ngine::filesystem {

namespace fs = std::filesystem;

namespace {

fs::path to_path(std::string_view utf8) {
    // Der Konstruktor mit char interpretiert den Text unter Windows in der Codepage des Systems,
    // der mit char8_t immer als UTF-8.
    return fs::path{std::u8string{utf8.begin(), utf8.end()}};
}

std::string error_text(std::string_view action, std::string_view path, const std::error_code& ec) {
    return std::format("{} '{}': {}", action, path, ec.message());
}

template <typename Container>
Result<Container> read_into(std::string_view path) {
    const fs::path native = to_path(path);
    std::ifstream file{native, std::ios::binary | std::ios::ate};
    if (!file) {
        std::error_code ec;
        const bool found = fs::exists(native, ec);
        return std::unexpected(found ? std::format("Datei '{}' lässt sich nicht öffnen", path)
                                     : std::format("Datei '{}' gibt es nicht", path));
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::unexpected(std::format("Größe von '{}' unbekannt", path));
    }
    Container data(static_cast<usize>(size), typename Container::value_type{});
    file.seekg(0);
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::unexpected(std::format("Datei '{}' lässt sich nicht lesen", path));
    }
    return data;
}

} // namespace

std::string path_to_utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string{text.begin(), text.end()};
}

Result<std::vector<std::byte>> read_file(std::string_view path) {
    return read_into<std::vector<std::byte>>(path);
}

Result<std::string> read_text(std::string_view path) {
    return read_into<std::string>(path);
}

Result<void> write_file(std::string_view path, std::span<const std::byte> data) {
    const fs::path target = to_path(path);
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return std::unexpected(error_text("Ordner für", path, ec));
        }
    }

    fs::path temporary = target;
    temporary += ".tmp";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            return std::unexpected(
                std::format("Datei '{}' lässt sich nicht anlegen", path_to_utf8(temporary)));
        }
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) {
            file.close();
            fs::remove(temporary, ec);
            return std::unexpected(std::format("Schreiben nach '{}' fehlgeschlagen", path));
        }
    }

    // Ersetzt eine vorhandene Datei, auch unter Windows (MoveFileEx mit REPLACE_EXISTING).
    fs::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return std::unexpected(error_text("Umbenennen nach", path, ec));
    }
    return {};
}

Result<void> write_text(std::string_view path, std::string_view text) {
    return write_file(path, std::as_bytes(std::span{text.data(), text.size()}));
}

bool exists(std::string_view path) noexcept {
    std::error_code ec;
    try {
        return fs::exists(to_path(path), ec);
    } catch (...) {
        // Nur bei Speichermangel beim Umwandeln des Pfads.
        return false;
    }
}

Result<void> create_directories(std::string_view path) {
    std::error_code ec;
    fs::create_directories(to_path(path), ec);
    if (ec) {
        return std::unexpected(error_text("Ordner", path, ec));
    }
    return {};
}

Result<void> remove(std::string_view path) {
    std::error_code ec;
    fs::remove_all(to_path(path), ec);
    if (ec) {
        return std::unexpected(error_text("Löschen von", path, ec));
    }
    return {};
}

std::string join(std::string_view path, std::string_view name) {
    return path_to_utf8(to_path(path) / to_path(name));
}

Result<std::string> user_data_dir(std::string_view application) {
    auto base = platform_user_data_dir();
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    const fs::path directory = to_path(*base) / to_path(application);
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(error_text("Benutzerordner", path_to_utf8(directory), ec));
    }
    return path_to_utf8(directory);
}

} // namespace ngine::filesystem
