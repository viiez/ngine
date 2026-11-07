module;

#include <chrono>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module ngine.filesystem;

import ngine.core;

namespace ngine::filesystem {

namespace {

f64 now_seconds() {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ignored(std::string_view path) {
    return path.ends_with(".tmp") || path.ends_with("~");
}

} // namespace

FileWatcher::FileWatcher(std::string_view root, f64 interval_seconds)
    : root_{normalize(root)}, interval_{interval_seconds} {}

std::vector<FileEvent> FileWatcher::poll() {
    const f64 now = now_seconds();
    if (now - last_scan_ < interval_) {
        return {};
    }
    last_scan_ = now;
    return scan();
}

std::vector<FileEvent> FileWatcher::scan() {
    std::map<std::string, Stamp> current;
    if (auto entries = list_directory(root_, true)) {
        for (const FileEntry& entry : *entries) {
            if (!entry.directory && !ignored(entry.path)) {
                current.emplace(root_ + "/" + entry.path, Stamp{entry.size, entry.modified});
            }
        }
    }
    std::vector<FileEvent> events;
    if (first_) {
        first_ = false;
        known_ = std::move(current);
        return events;
    }

    std::map<std::string, Stamp> pending;
    for (const auto& [path, stamp] : current) {
        const auto known = known_.find(path);
        if (known != known_.end() && known->second == stamp) {
            continue;
        }
        // Erst melden, wenn die Datei seit dem letzten Durchlauf gleich geblieben ist.
        const auto seen = pending_.find(path);
        if (seen != pending_.end() && seen->second == stamp) {
            events.push_back(
                {path, known == known_.end() ? FileChange::Added : FileChange::Modified});
            known_[path] = stamp;
        } else {
            pending.emplace(path, stamp);
        }
    }
    pending_ = std::move(pending);
    for (auto it = known_.begin(); it != known_.end();) {
        if (!current.contains(it->first)) {
            events.push_back({it->first, FileChange::Removed});
            it = known_.erase(it);
        } else {
            ++it;
        }
    }
    return events;
}

} // namespace ngine::filesystem
