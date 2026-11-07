module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
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

// Wie path_to_utf8, aber mit „/“ als Trenner auf allen Plattformen.
std::string generic_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
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

bool is_directory(std::string_view path) noexcept {
    std::error_code ec;
    try {
        return fs::is_directory(to_path(path), ec);
    } catch (...) {
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

Result<void> rename(std::string_view from, std::string_view to) {
    std::error_code ec;
    fs::rename(to_path(from), to_path(to), ec);
    if (ec) {
        return std::unexpected(error_text("Umbenennen von", from, ec));
    }
    return {};
}

std::string join(std::string_view path, std::string_view name) {
    return path_to_utf8(to_path(path) / to_path(name));
}

std::string normalize(std::string_view path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(to_path(path), ec);
    if (ec) {
        absolute = to_path(path);
    }
    std::string text = generic_utf8(absolute.lexically_normal());
    // „ordner/“ und „ordner“ sollen gleich aussehen.
    while (text.size() > 1 && text.back() == '/' && text[text.size() - 2] != ':') {
        text.pop_back();
    }
    return text;
}

std::string relative(std::string_view path, std::string_view base) {
    const fs::path normal_path = to_path(normalize(path));
    const fs::path normal_base = to_path(normalize(base));
    const fs::path result = normal_path.lexically_relative(normal_base);
    if (result.empty()) {
        return {};
    }
    std::string text = generic_utf8(result);
    if (text == "." || text.starts_with("..")) {
        return {};
    }
    return text;
}

std::string parent_path(std::string_view path) {
    return path_to_utf8(to_path(path).parent_path());
}

std::string file_name(std::string_view path) {
    return path_to_utf8(to_path(path).filename());
}

std::string extension(std::string_view path) {
    std::string text = path_to_utf8(to_path(path).extension());
    std::ranges::transform(text, text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

namespace {

i64 ticks(const fs::file_time_type& time) {
    return static_cast<i64>(time.time_since_epoch().count());
}

bool hidden(const fs::path& name) {
    const std::u8string text = name.filename().u8string();
    return !text.empty() && text.front() == u8'.';
}

} // namespace

Result<std::vector<FileEntry>> list_directory(std::string_view path, bool recursive) {
    const fs::path root = to_path(path);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return std::unexpected(std::format("Ordner '{}' gibt es nicht", path));
    }
    std::vector<FileEntry> entries;
    const auto add = [&](const fs::directory_entry& entry) {
        FileEntry file;
        file.path = generic_utf8(entry.path().lexically_relative(root));
        std::error_code status_ec;
        file.directory = entry.is_directory(status_ec);
        if (!file.directory) {
            file.size = static_cast<u64>(entry.file_size(status_ec));
            if (status_ec) {
                return;
            }
        }
        file.modified = ticks(entry.last_write_time(status_ec));
        entries.push_back(std::move(file));
    };
    if (recursive) {
        fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied,
                                            ec};
        for (; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
            if (hidden(it->path())) {
                if (it->is_directory()) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            add(*it);
        }
    } else {
        for (fs::directory_iterator it{root, ec}; !ec && it != fs::directory_iterator{};
             it.increment(ec)) {
            add(*it);
        }
    }
    if (ec) {
        return std::unexpected(error_text("Ordner", path, ec));
    }
    std::ranges::sort(entries, {}, &FileEntry::path);
    return entries;
}

std::optional<FileEntry> status(std::string_view path) {
    const fs::path native = to_path(path);
    std::error_code ec;
    const fs::file_status state = fs::status(native, ec);
    if (ec || !fs::exists(state)) {
        return std::nullopt;
    }
    FileEntry entry;
    entry.path = std::string{path};
    entry.directory = fs::is_directory(state);
    if (!entry.directory) {
        entry.size = static_cast<u64>(fs::file_size(native, ec));
    }
    entry.modified = ticks(fs::last_write_time(native, ec));
    if (ec) {
        return std::nullopt;
    }
    return entry;
}

Result<std::string> executable_dir() {
    auto path = platform_executable_path();
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    return path_to_utf8(to_path(*path).parent_path());
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
