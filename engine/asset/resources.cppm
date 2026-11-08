module;

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.asset:resources;

import ngine.core;
import ngine.jobs;
import :image;
import :database;

// Ressourcen-Manager (ASSET-04, ADR 0033): lädt Assets über ihre Kennung, zählt Verweise über
// Handles und gibt Ressourcen frei, die eine Weile niemand mehr hält. Laden läuft in zwei
// Schritten: read() liest und dekodiert im Hintergrund auf dem Job-System, finish() macht im
// Hauptthread daraus die fertige Ressource (z. B. GPU-Upload). Hot Reload (PLAT-06) liest neu
// und tauscht den Wert aus; Handles zeigen danach auf den neuen.

namespace ngine::asset::detail {

// Schlüssel eines C++-Typs innerhalb eines Programmlaufs (ohne RTTI).
using TypeKey = const void*;
template <typename T>
inline constexpr char type_tag = 0;
template <typename T>
[[nodiscard]] constexpr TypeKey type_key() noexcept {
    return &type_tag<T>;
}

struct ResourceEntry;

} // namespace ngine::asset::detail

export namespace ngine::asset {

enum class ResourceState : u8 {
    // Wird gelesen; ein Wert ist noch nicht da.
    Loading,
    Ready,
    // Erstes Laden gescheitert (error() sagt warum). Ein gescheitertes Neuladen behält den alten
    // Wert und bleibt Ready.
    Failed,
};

// Was read() außer dem Pfad bekommt.
struct LoadContext {
    // Kennung des Assets (z. B. für den Cache gekochter Daten).
    AssetId id = 0;
    // Datei des Assets, absolut und normalisiert.
    std::string_view path;
    // read() trägt weitere Dateien ein, die es gelesen hat (externe Puffer und Bilder einer
    // glTF-Datei); ändert sich eine davon, lädt der Manager neu. Die Datei selbst steht schon da.
    std::vector<std::string>* dependencies = nullptr;
};

} // namespace ngine::asset

namespace ngine::asset::detail {

struct ResourceEntry {
    AssetId id = 0;
    TypeKey type = nullptr;
    ResourceState state = ResourceState::Loading;
    std::shared_ptr<void> value;
    std::string error;
    // Zählt erfolgreiche Ladevorgänge; ändert sich beim Neuladen.
    u32 version = 0;
    // Erzeugt (ResourceManager::add()): ohne Datei, bleibt bis remove().
    bool generated = false;
};

using ResourcePtr = std::shared_ptr<ResourceEntry>;

} // namespace ngine::asset::detail

export namespace ngine::asset {

// Verweis auf eine Ressource. Solange ein Handle lebt, bleibt sie geladen. Handles sind billig zu
// kopieren; benutzt werden sie nur im Hauptthread.
template <typename T>
class Handle {
public:
    Handle() = default;

    // Der Wert, sobald er geladen ist; sonst nullptr. Nach einem Neuladen ein anderer Zeiger.
    [[nodiscard]] const T* get() const noexcept {
        return entry_ && entry_->value ? static_cast<const T*>(entry_->value.get()) : nullptr;
    }
    [[nodiscard]] ResourceState state() const noexcept {
        return entry_ ? entry_->state : ResourceState::Failed;
    }
    [[nodiscard]] const std::string& error() const noexcept {
        static const std::string none;
        return entry_ ? entry_->error : none;
    }
    [[nodiscard]] u32 version() const noexcept {
        return entry_ ? entry_->version : 0;
    }
    [[nodiscard]] AssetId id() const noexcept {
        return entry_ ? entry_->id : 0;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return entry_ != nullptr;
    }

private:
    friend class ResourceManager;
    explicit Handle(detail::ResourcePtr entry) noexcept : entry_{std::move(entry)} {}
    detail::ResourcePtr entry_;
};

struct ResourceStats {
    usize resident = 0;
    usize loading = 0;
    usize failed = 0;
    // Wie oft neu geladen wurde (Hot Reload), seit dem Start.
    usize reloads = 0;
    // Fertige Werte, die auf ready warten (Uploads laufen noch).
    usize pending = 0;
};

class ResourceManager {
public:
    // database liefert die Pfade zu Kennungen (nullptr: nur erzeugte Ressourcen). Mit jobs liest
    // read() im Hintergrund, sonst sofort im Aufruf. Beide müssen den Manager überleben.
    explicit ResourceManager(const AssetDatabase* database = nullptr,
                             jobs::JobSystem* jobs = nullptr);
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    void set_database(const AssetDatabase* database) noexcept;

    // Wie Ressourcen vom Typ T entstehen. read läuft im Hintergrund und darf nichts anfassen,
    // was andere Threads benutzen; finish läuft im Hauptthread in update(). Mit ready wird ein
    // fertiger Wert erst sichtbar, wenn ready ihn für benutzbar hält (z. B. Uploads auf die GPU
    // erledigt); bis dahin bleibt beim Neuladen der alte Wert stehen (TD-30).
    template <typename T, typename Cpu>
    void add_loader(std::function<Result<Cpu>(const LoadContext&)> read,
                    std::function<Result<T>(Cpu&)> finish,
                    std::function<bool(const T&)> ready = {}) {
        Loader loader{
            .read = [read = std::move(read)](
                        const LoadContext& context) -> Result<std::shared_ptr<void>> {
                auto data = read(context);
                if (!data) {
                    return std::unexpected(std::move(data.error()));
                }
                return std::shared_ptr<void>{std::make_shared<Cpu>(std::move(*data))};
            },
            .finish = [finish = std::move(finish)](void* cpu) -> Result<std::shared_ptr<void>> {
                auto value = finish(*static_cast<Cpu*>(cpu));
                if (!value) {
                    return std::unexpected(std::move(value.error()));
                }
                return std::shared_ptr<void>{std::make_shared<T>(std::move(*value))};
            },
            .ready = {}};
        if (ready) {
            loader.ready = [ready = std::move(ready)](const void* value) {
                return ready(*static_cast<const T*>(value));
            };
        }
        set_loader(detail::type_key<T>(), std::move(loader));
    }

    // Ressource zu einer Kennung; beginnt das Laden, wenn sie noch nicht da ist.
    template <typename T>
    [[nodiscard]] Handle<T> load(AssetId id) {
        return Handle<T>{load_entry(id, detail::type_key<T>())};
    }
    // Schon geladene oder ladende Ressource, ohne ein Laden zu beginnen.
    template <typename T>
    [[nodiscard]] Handle<T> find(AssetId id) const {
        return Handle<T>{find_entry(id, detail::type_key<T>())};
    }
    // Erzeugte Ressource ohne Datei (z. B. Meshes der Standardszene); ersetzt eine vorhandene.
    // Sie bleibt, bis remove() sie freigibt.
    template <typename T>
    Handle<T> add(AssetId id, T value) {
        return Handle<T>{add_entry(id, detail::type_key<T>(),
                                   std::shared_ptr<void>{std::make_shared<T>(std::move(value))})};
    }
    // Gibt eine erzeugte Ressource frei, sobald niemand mehr ein Handle hält.
    void remove(AssetId id);

    // Liest alle Ressourcen dieser Kennung neu.
    void reload(AssetId id);
    // Liest alle Ressourcen neu, deren Datei oder Abhängigkeit in files steht (normalisierte
    // Pfade, z. B. vom Dateibeobachter). Liefert, wie viele es waren.
    usize reload_files(std::span<const std::string> files);

    // Hauptthread, einmal pro Frame: macht fertig gelesene Ressourcen fertig, tauscht bereite
    // Werte ein und gibt Ressourcen frei, die seit keep_updates Aufrufen niemand mehr hält.
    void update();
    // Wartet, bis alles gelesen ist, und ruft update() auf (Tests, Laden einer Szene). Werte, die
    // ihr Lader noch nicht für bereit hält, bleiben ausstehend.
    void wait();

    [[nodiscard]] ResourceStats stats() const;

    // Wie viele update() eine Ressource ohne Handle noch geladen bleibt (z. B. 60 Frames), damit
    // Löschen und Undo sie nicht sofort neu laden.
    u32 keep_updates = 120;

private:
    struct Loader {
        std::function<Result<std::shared_ptr<void>>(const LoadContext&)> read;
        std::function<Result<std::shared_ptr<void>>(void*)> finish;
        std::function<bool(const void*)> ready;
    };

    void set_loader(detail::TypeKey type, Loader loader);
    [[nodiscard]] detail::ResourcePtr load_entry(AssetId id, detail::TypeKey type);
    [[nodiscard]] detail::ResourcePtr find_entry(AssetId id, detail::TypeKey type) const;
    detail::ResourcePtr add_entry(AssetId id, detail::TypeKey type, std::shared_ptr<void> value);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::asset
