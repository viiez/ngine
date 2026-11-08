module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.jobs;
import ngine.profile;

namespace ngine::asset {

namespace {

// Daten eines Eintrags, die nur der Manager kennt.
struct Slot {
    detail::ResourcePtr entry;
    // Datei und Abhängigkeiten des letzten Ladens (normalisiert).
    std::vector<std::string> files;
    // Zählt Anfragen; ein Ergebnis, das nicht zur letzten passt, ist veraltet.
    u32 request = 0;
    bool in_flight = false;
    bool removed = false;
    u32 idle_updates = 0;
    // Fertiger Wert, der noch nicht bereit ist (TD-30); ersetzt dann entry->value.
    std::shared_ptr<void> pending;
};

// Ergebnis von read(), wartet auf finish() im Hauptthread.
struct Completion {
    detail::ResourcePtr entry;
    u32 request = 0;
    Result<std::shared_ptr<void>> data;
    std::vector<std::string> files;
};

using Key = std::pair<AssetId, detail::TypeKey>;

} // namespace

struct ResourceManager::Impl {
    const AssetDatabase* database = nullptr;
    jobs::JobSystem* jobs = nullptr;
    std::unordered_map<detail::TypeKey, Loader> loaders;
    std::map<Key, Slot> slots;
    // Gemeinsam mit den Jobs: fertig gelesene Ressourcen.
    std::mutex mutex;
    std::vector<Completion> completed;
    jobs::Counter counter;
    usize reloads = 0;

    Slot* slot_of(const detail::ResourcePtr& entry) {
        const auto found = slots.find({entry->id, entry->type});
        return found != slots.end() && found->second.entry == entry ? &found->second : nullptr;
    }

    // Startet read() für slot (im Hintergrund oder sofort).
    void start(Slot& slot) {
        const auto loader = loaders.find(slot.entry->type);
        const AssetInfo* info = database != nullptr ? database->find(slot.entry->id) : nullptr;
        if (loader == loaders.end() || info == nullptr) {
            const std::string message =
                loader == loaders.end()
                    ? std::format("Asset {}: kein Lader für diesen Typ", slot.entry->id)
                    : std::format("Asset {} gibt es nicht", slot.entry->id);
            if (slot.entry->value == nullptr) {
                slot.entry->state = ResourceState::Failed;
            }
            slot.entry->error = message;
            return;
        }
        const u32 request = ++slot.request;
        slot.in_flight = true;
        // Die Daten des Auftrags liegen auf dem Heap: Job-Funktionen haben wenig Platz.
        struct Task {
            detail::ResourcePtr entry;
            u32 request = 0;
            std::string path;
            std::function<Result<std::shared_ptr<void>>(const LoadContext&)> read;
        };
        auto task = std::make_shared<Task>(Task{.entry = slot.entry,
                                                .request = request,
                                                .path = info->file,
                                                .read = loader->second.read});
        auto job = [this, task = std::move(task)] {
            NGINE_PROFILE_ZONE("ResourceManager::read");
            std::vector<std::string> files{task->path};
            auto data = task->read(
                LoadContext{.id = task->entry->id, .path = task->path, .dependencies = &files});
            const std::scoped_lock lock{mutex};
            completed.push_back({.entry = std::move(task->entry),
                                 .request = task->request,
                                 .data = std::move(data),
                                 .files = std::move(files)});
        };
        if (jobs != nullptr) {
            jobs->run(std::move(job), &counter);
        } else {
            job();
        }
    }
};

ResourceManager::ResourceManager(const AssetDatabase* database, jobs::JobSystem* jobs)
    : impl_{std::make_unique<Impl>()} {
    impl_->database = database;
    impl_->jobs = jobs;
}

ResourceManager::~ResourceManager() {
    // Jobs halten Zeiger auf den Manager: erst warten, dann aufräumen.
    if (impl_->jobs != nullptr) {
        impl_->jobs->wait(impl_->counter);
    }
}

void ResourceManager::set_database(const AssetDatabase* database) noexcept {
    impl_->database = database;
}

void ResourceManager::set_loader(detail::TypeKey type, Loader loader) {
    impl_->loaders[type] = std::move(loader);
}

detail::ResourcePtr ResourceManager::load_entry(AssetId id, detail::TypeKey type) {
    if (id == 0) {
        return nullptr;
    }
    auto [it, inserted] = impl_->slots.try_emplace({id, type});
    Slot& slot = it->second;
    if (inserted) {
        slot.entry = std::make_shared<detail::ResourceEntry>();
        slot.entry->id = id;
        slot.entry->type = type;
        impl_->start(slot);
    }
    slot.idle_updates = 0;
    return slot.entry;
}

detail::ResourcePtr ResourceManager::find_entry(AssetId id, detail::TypeKey type) const {
    const auto found = impl_->slots.find({id, type});
    return found != impl_->slots.end() ? found->second.entry : nullptr;
}

detail::ResourcePtr ResourceManager::add_entry(AssetId id, detail::TypeKey type,
                                               std::shared_ptr<void> value) {
    Slot& slot = impl_->slots[{id, type}];
    if (!slot.entry) {
        slot.entry = std::make_shared<detail::ResourceEntry>();
        slot.entry->id = id;
        slot.entry->type = type;
    }
    // Ein laufendes Laden aus einer Datei ist damit veraltet.
    ++slot.request;
    slot.pending.reset();
    slot.removed = false;
    slot.entry->generated = true;
    slot.entry->value = std::move(value);
    slot.entry->state = ResourceState::Ready;
    slot.entry->error.clear();
    ++slot.entry->version;
    return slot.entry;
}

void ResourceManager::remove(AssetId id) {
    for (auto& [key, slot] : impl_->slots) {
        if (key.first == id && slot.entry->generated) {
            slot.removed = true;
        }
    }
}

void ResourceManager::reload(AssetId id) {
    for (auto& [key, slot] : impl_->slots) {
        if (key.first == id && !slot.entry->generated) {
            impl_->start(slot);
            ++impl_->reloads;
        }
    }
}

usize ResourceManager::reload_files(std::span<const std::string> files) {
    const std::unordered_set<std::string> changed{files.begin(), files.end()};
    usize count = 0;
    for (auto& [key, slot] : impl_->slots) {
        if (slot.entry->generated) {
            continue;
        }
        const bool affected = std::ranges::any_of(
            slot.files, [&](const std::string& file) { return changed.contains(file); });
        if (affected) {
            impl_->start(slot);
            ++impl_->reloads;
            ++count;
        }
    }
    return count;
}

void ResourceManager::update() {
    NGINE_PROFILE_ZONE("ResourceManager::update");
    std::vector<Completion> completed;
    {
        const std::scoped_lock lock{impl_->mutex};
        completed.swap(impl_->completed);
    }
    for (Completion& done : completed) {
        Slot* slot = impl_->slot_of(done.entry);
        if (slot == nullptr || done.request != slot->request) {
            continue; // veraltet oder freigegeben
        }
        slot->in_flight = false;
        detail::ResourceEntry& entry = *done.entry;
        Result<std::shared_ptr<void>> value =
            done.data ? impl_->loaders.at(entry.type).finish(done.data->get())
                      : std::unexpected(std::move(done.data.error()));
        // Abhängigkeiten auch bei einem Fehler merken: Wer die Datei repariert, lädt neu.
        slot->files = std::move(done.files);
        if (!value) {
            entry.error = std::move(value.error());
            if (entry.value == nullptr) {
                entry.state = ResourceState::Failed;
                log::error("Laden: {}", entry.error);
            } else {
                log::error("Neu laden gescheitert, der alte Stand bleibt: {}", entry.error);
            }
            continue;
        }
        // Ein neuerer Wert ersetzt einen, der noch auf ready wartet.
        slot->pending = std::move(*value);
    }

    // Werte eintauschen, sobald ihr Lader sie für bereit hält. Bis dahin zeigen Handles auf den
    // alten Wert (TD-30) bzw. beim ersten Laden auf nichts.
    for (auto& [key, slot] : impl_->slots) {
        if (slot.pending == nullptr) {
            continue;
        }
        const Loader& loader = impl_->loaders.at(key.second);
        if (loader.ready && !loader.ready(slot.pending.get())) {
            continue;
        }
        detail::ResourceEntry& entry = *slot.entry;
        const bool again = entry.value != nullptr;
        entry.value = std::move(slot.pending);
        entry.state = ResourceState::Ready;
        entry.error.clear();
        ++entry.version;
        if (again) {
            log::info("Neu geladen: Asset {}", entry.id);
        }
    }

    // Ressourcen, die niemand mehr hält, nach einer Weile freigeben. Ein laufendes Laden hält eine
    // Kopie des Eintrags und verhindert das.
    for (auto it = impl_->slots.begin(); it != impl_->slots.end();) {
        Slot& slot = it->second;
        const bool unused = slot.entry.use_count() == 1 && !slot.in_flight &&
                            slot.pending == nullptr && (!slot.entry->generated || slot.removed);
        if (!unused) {
            slot.idle_updates = 0;
            ++it;
            continue;
        }
        if (++slot.idle_updates > keep_updates || slot.removed) {
            it = impl_->slots.erase(it);
        } else {
            ++it;
        }
    }
}

void ResourceManager::wait() {
    if (impl_->jobs != nullptr) {
        impl_->jobs->wait(impl_->counter);
    }
    update();
}

ResourceStats ResourceManager::stats() const {
    ResourceStats stats;
    for (const auto& [key, slot] : impl_->slots) {
        switch (slot.entry->state) {
        case ResourceState::Ready: ++stats.resident; break;
        case ResourceState::Loading: ++stats.loading; break;
        case ResourceState::Failed: ++stats.failed; break;
        }
    }
    stats.reloads = impl_->reloads;
    for (const auto& [key, slot] : impl_->slots) {
        stats.pending += slot.pending != nullptr ? 1 : 0;
    }
    return stats;
}

} // namespace ngine::asset
