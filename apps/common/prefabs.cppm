module;

#include <expected>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

// Prefabs aus dem Asset-Ordner für Sandbox und Editor (WORLD-04, R2.5): Der Ressourcen-Manager lädt
// .prefab-Dateien wie andere Assets (auch neu nach Änderungen); die Bibliothek merkt sich, auf
// welchem Stand die Instanzen in der Welt sind, und zieht sie nach, wenn sich eine Vorlage ändert.
export module app.prefabs;

import ngine.core;
import ngine.asset;
import ngine.world;

namespace app {

using namespace ngine;

export class PrefabLibrary {
public:
    // Meldet den Lader für world::Prefab an; resources muss die Bibliothek überleben.
    explicit PrefabLibrary(asset::ResourceManager& resources) : resources_{&resources} {
        resources.add_loader<world::Prefab, world::Prefab>(
            [](const asset::LoadContext& context) -> asset::Result<world::Prefab> {
                return world::load_prefab_file(context.path);
            },
            [](world::Prefab& prefab) -> asset::Result<world::Prefab> {
                return std::move(prefab);
            });
    }

    // Vorlage zu einer Kennung; lädt sie bei Bedarf sofort (wartet auf den Ressourcen-Manager).
    // nullptr, wenn es sie nicht gibt oder sie nicht lesbar ist.
    [[nodiscard]] const world::Prefab* get(u64 id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) {
            Entry entry{.handle = resources_->load<world::Prefab>(id), .version = 0, .applied = {}};
            if (entry.handle.get() == nullptr &&
                entry.handle.state() == asset::ResourceState::Loading) {
                resources_->wait();
            }
            if (entry.handle.get() == nullptr) {
                return nullptr;
            }
            entry.version = entry.handle.version();
            entry.applied = std::make_shared<world::Prefab>(*entry.handle.get());
            it = entries_.emplace(id, std::move(entry)).first;
        }
        return it->second.applied.get();
    }

    // Für Szenen und Dokument: Vorlagen über diese Bibliothek.
    [[nodiscard]] world::PrefabLookup lookup() {
        return [this](u64 id) { return get(id); };
    }

    // Vorlagen, die sich geändert haben (Hot Reload), auf ihre Instanzen in world übertragen.
    // Liefert, wie viele Instanzen nachgezogen wurden.
    usize update(world::World& world) {
        usize updated = 0;
        for (auto& [id, entry] : entries_) {
            const world::Prefab* current = entry.handle.get();
            if (current == nullptr || entry.handle.version() == entry.version) {
                continue;
            }
            const usize count = world::update_instances(world, id, *entry.applied, *current);
            log::info("Prefab {}: {} Instanzen nachgezogen", id, count);
            updated += count;
            entry.version = entry.handle.version();
            entry.applied = std::make_shared<world::Prefab>(*current);
        }
        return updated;
    }

    // Vergisst alle Stände (z. B. nach dem Öffnen einer anderen Szene); Handles bleiben bis dahin.
    void clear() {
        entries_.clear();
    }

private:
    struct Entry {
        asset::Handle<world::Prefab> handle;
        u32 version = 0;
        // Stand, auf dem die Instanzen in der Welt sind.
        std::shared_ptr<world::Prefab> applied;
    };

    asset::ResourceManager* resources_;
    std::unordered_map<u64, Entry> entries_;
};

} // namespace app
