module;

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module ngine.world:commands;

import ngine.core;
import ngine.reflect;
import :ecs;
import :prefab;

export namespace ngine::world {

// Befehlspuffer (WORLD-09): sammelt Strukturänderungen, die ein System während Schedule::run()
// nicht selbst ausführen darf, und führt sie danach im Hauptthread aus. Ein System bekommt ihn,
// wenn seine Funktion Commands& als ersten Parameter nimmt:
//
//   schedule.add<const Health>("Sterben", [](Commands& commands, Entity entity, const Health& h) {
//       if (h.value <= 0) {
//           commands.destroy(entity);
//       }
//   });
//
// Angelegte Entities bekommen sofort einen vorläufigen Griff. Spätere Befehle desselben Puffers
// dürfen ihn benutzen (set(), set_parent()); beim Ausführen wird er durch die echte Kennung
// ersetzt. Der vorläufige Griff selbst taugt für nichts anderes. Werte liegen als Bytes mit ihrem
// TypeInfo im Puffer (wie die Befehle des Editors, ADR 0030).
//
// Ein Puffer gehört einem Job und ist nicht threadsicher. Der Schedule gibt jedem Stück eines
// Systems einen eigenen Puffer und führt sie in fester Reihenfolge aus (System, Chunk, Zeile),
// egal wie die Jobs verteilt waren (ADR 0044).
class Commands {
public:
    explicit Commands(World& world) noexcept : world_{&world} {}
    ~Commands();
    Commands(Commands&& other) noexcept;
    Commands& operator=(Commands&& other) noexcept;
    Commands(const Commands&) = delete;
    Commands& operator=(const Commands&) = delete;

    // Legt eine Entity an; mit name bekommt sie die Komponente Name.
    [[nodiscard]] Entity create();
    [[nodiscard]] Entity create(std::string_view name);
    // Legt eine Instanz der Vorlage an (wie world::instantiate()). prefab muss bis zum Ausführen
    // leben. Der Griff zeigt auf die Wurzel der Instanz.
    [[nodiscard]] Entity instantiate(const Prefab& prefab, u64 prefab_id, Entity parent = {});
    void destroy(Entity entity);
    // Fügt eine Komponente hinzu oder ersetzt sie.
    template <typename T>
    void set(Entity entity, T value) {
        const reflect::TypeInfo& type = world_->types().get<T>();
        void* storage = push_set(entity, type);
        type.ops.move_construct(storage, &value);
    }
    template <typename T>
    void remove(Entity entity) {
        push_remove(entity, world_->types().get<T>());
    }
    // Ein leerer parent löst die Entity vom Elternteil.
    void set_parent(Entity child, Entity parent);

    [[nodiscard]] usize size() const noexcept {
        return records_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return records_.empty();
    }

    // Führt alle Befehle in der Reihenfolge ihres Eintrags aus und leert den Puffer. Läuft im
    // Hauptthread, nicht während Systeme laufen. Befehle für Entities, die inzwischen zerstört
    // sind, entfallen (ein anderes System kann sie im selben Lauf zerstört haben).
    void apply();
    // Verwirft alle Befehle. Der Speicher bleibt für den nächsten Lauf.
    void clear() noexcept;

    // Ob id ein vorläufiger Griff eines Befehlspuffers ist.
    [[nodiscard]] static bool is_pending(u64 id) noexcept {
        return (id & pending_bit) != 0;
    }

private:
    // flecs nutzt das oberste Bit nur für Paare, nie für Entities.
    static constexpr u64 pending_bit = u64{1} << 63;

    enum class Op : u8 { Create, Instantiate, Destroy, Set, Remove, SetParent };

    struct Record {
        Op op;
        u64 entity = 0;
        u64 other = 0;
        const reflect::TypeInfo* type = nullptr;
        void* value = nullptr;
        const Prefab* prefab = nullptr;
        std::string name;
        u64 prefab_id = 0;
    };

    void* push_set(Entity entity, const reflect::TypeInfo& type);
    void push_remove(Entity entity, const reflect::TypeInfo& type);
    void* allocate(usize size, usize alignment);
    [[nodiscard]] u64 resolve(u64 id) const noexcept;
    void destroy_values() noexcept;

    World* world_;
    std::vector<Record> records_;
    // Echte Kennungen der angelegten Entities, Index wie im vorläufigen Griff.
    std::vector<u64> created_;
    u32 pending_count_ = 0;
    // Speicher für Werte in Blöcken, die nie umziehen: Werte wie std::string dürfen nicht byteweise
    // verschoben werden. Die Blöcke bleiben über clear() hinaus erhalten.
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::vector<usize> block_sizes_;
    usize block_ = 0;
    usize offset_ = 0;
};

} // namespace ngine::world
