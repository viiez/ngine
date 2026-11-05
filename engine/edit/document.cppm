module;

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module ngine.edit:document;

import ngine.core;
import ngine.reflect;
import ngine.world;

export namespace ngine::edit {

template <typename T>
using Result = std::expected<T, std::string>;

// Stabile Kennung einer Entity im Dokument. Anders als die Kennung der Welt bleibt sie gleich,
// wenn Undo eine gelöschte Entity neu anlegt; Befehle merken sich nur sie. 0 heißt: keine.
using EntityId = u64;

// --- Ereignisse des Dokuments (über events::EventBus) ---------------------------------------

// Eine Entity hat sich geändert. component ist der Typ der geänderten Komponente, 0 bei Änderungen
// an der Entity selbst (angelegt, gelöscht, umgehängt, Komponente hinzu oder weg).
struct EntityChanged {
    EntityId entity = 0;
    reflect::TypeId component = 0;
};

// Das ganze Dokument ist neu: geöffnet, geleert oder der Verlauf ist verworfen.
struct DocumentReset {};

// Der Zustand gegenüber der Datei hat sich geändert (gespeichert oder erstmals verändert).
struct DirtyChanged {
    bool dirty = false;
};

class SceneDocument;

// Eine umkehrbare Änderung am Dokument (Muster „Befehl“, ADR 0030). Befehle verweisen über
// EntityId auf Entities und halten alte und neue Werte als Bytes im Binärformat der Reflection.
class Command {
public:
    Command() = default;
    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    virtual ~Command() = default;

    // Für Menü und Verlauf, z. B. „Transform.position ändern“.
    [[nodiscard]] virtual std::string label() const = 0;
    // Führt die Änderung (erneut) aus. Ein Fehler lässt das Dokument unverändert.
    [[nodiscard]] virtual Result<void> apply(SceneDocument& document) = 0;
    // Nimmt apply() zurück; läuft nur nach erfolgreichem apply().
    virtual void revert(SceneDocument& document) = 0;
    // Nimmt next in diesen Befehl auf, wenn beide dasselbe ändern (Ziehen eines Reglers über
    // viele Frames wird ein Schritt). next ist schon ausgeführt.
    [[nodiscard]] virtual bool merge(const Command& /*next*/) {
        return false;
    }
};

// Verlauf für Undo und Redo.
class CommandStack {
public:
    explicit CommandStack(usize limit = 1000) : limit_{limit} {}

    [[nodiscard]] bool can_undo() const noexcept {
        return cursor_ > 0;
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return cursor_ < commands_.size();
    }
    // Beschriftung des nächsten Undo bzw. Redo, leer ohne.
    [[nodiscard]] std::string undo_label() const;
    [[nodiscard]] std::string redo_label() const;
    [[nodiscard]] usize size() const noexcept {
        return commands_.size();
    }
    [[nodiscard]] usize position() const noexcept {
        return cursor_;
    }

    // Beendet das Zusammenfassen: Der nächste Befehl wird ein eigener Schritt.
    void seal() noexcept {
        sealed_ = true;
    }
    // Merkt sich den aktuellen Stand als gespeichert.
    void mark_saved() noexcept {
        saved_ = cursor_;
        saved_valid_ = true;
    }
    [[nodiscard]] bool at_saved() const noexcept {
        return saved_valid_ && saved_ == cursor_;
    }
    void clear() noexcept;

private:
    friend class SceneDocument;
    // Hängt einen ausgeführten Befehl an; verwirft Redo. Mit merge versucht er, ihn dem letzten
    // aufzunehmen.
    void push(std::unique_ptr<Command> command, bool merge);
    [[nodiscard]] Command* undo_step() noexcept;
    [[nodiscard]] Command* redo_step() noexcept;

    std::vector<std::unique_ptr<Command>> commands_;
    usize cursor_ = 0;
    usize limit_;
    usize saved_ = 0;
    bool saved_valid_ = true;
    bool sealed_ = true;
};

// Eine Szene im Editor: die Welt, stabile Kennungen ihrer Entities, der Verlauf und der Pfad der
// Datei. Alle Änderungen über execute() sind umkehrbar. Ändert jemand die Welt an execute()
// vorbei, stimmt der Verlauf danach nicht mehr; dafür gibt es reset_history().
class SceneDocument {
public:
    // Ereignisse gehen an bus, wenn gesetzt. world und bus müssen das Dokument überleben.
    explicit SceneDocument(world::World& world, events::EventBus* bus = nullptr);

    [[nodiscard]] world::World& world() noexcept {
        return *world_;
    }
    [[nodiscard]] const world::World& world() const noexcept {
        return *world_;
    }
    [[nodiscard]] const reflect::TypeRegistry& types() const noexcept {
        return world_->types();
    }

    // --- Kennungen ---
    // Stabile Kennung einer lebenden Entity der Welt; vergibt beim ersten Mal eine neue.
    [[nodiscard]] EntityId id_of(u64 entity);
    // Entity der Welt zu einer Kennung, 0 wenn es sie (gerade) nicht gibt.
    [[nodiscard]] u64 entity_of(EntityId id) const;
    [[nodiscard]] bool exists(EntityId id) const {
        return entity_of(id) != 0;
    }
    // Für Befehle: verknüpft eine Kennung mit einer (neuen) Entity bzw. löst sie.
    void bind(EntityId id, u64 entity);
    void unbind(EntityId id);
    // Neue, noch nie vergebene Kennung.
    [[nodiscard]] EntityId reserve_id() noexcept {
        return next_id_++;
    }

    // --- Verlauf ---
    // Führt command aus und legt ihn in den Verlauf. Mit merge darf er mit dem letzten Befehl
    // verschmelzen (siehe CommandStack::seal()).
    Result<void> execute(std::unique_ptr<Command> command, bool merge = false);
    bool undo();
    bool redo();
    [[nodiscard]] CommandStack& history() noexcept {
        return history_;
    }
    [[nodiscard]] const CommandStack& history() const noexcept {
        return history_;
    }
    void reset_history();

    // --- Datei ---
    // Leert die Welt, lädt die Szene und verwirft den Verlauf. Bei einem Fehler bleibt die Welt
    // leer.
    Result<usize> open(std::string_view path);
    // Speichert die Szene; danach ist das Dokument nicht mehr verändert.
    Result<void> save(std::string_view path);
    // Leert die Welt und verwirft den Verlauf.
    void clear();
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }
    // Verändert gegenüber der Datei bzw. dem Stand nach clear() oder open().
    [[nodiscard]] bool dirty() const noexcept {
        return !history_.at_saved();
    }

    // --- Ereignisse ---
    [[nodiscard]] events::EventBus* bus() const noexcept {
        return bus_;
    }
    template <typename E>
    void notify(const E& event) {
        if (bus_ != nullptr) {
            bus_->publish(event);
        }
    }

private:
    void notify_dirty(bool was_dirty);
    void destroy_all();

    world::World* world_;
    events::EventBus* bus_;
    CommandStack history_;
    std::unordered_map<EntityId, u64> entities_;
    std::unordered_map<u64, EntityId> ids_;
    EntityId next_id_ = 1;
    std::string path_;
};

} // namespace ngine::edit
