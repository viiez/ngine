module;

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

export module ngine.replication:snapshot;

import ngine.core;
import ngine.reflect;
import ngine.world;

// Nachrichten und Schnappschüsse ohne Netzwerk, damit sie sich einzeln testen lassen.
//
// Jede Nachricht beginnt mit einem Byte MessageKind. Ein Schnappschuss enthält:
//   u32 Tick, u32 Anzahl Entities, dann je Entity
//     u64 Kennung auf dem Server, u16 Anzahl Komponenten, dann je Komponente
//       u16 Index in der Komponentenliste, Felder wie serialize::write_binary
// Welche Komponenten repliziert werden, legen Server und Client gleich fest; die Willkommens-
// nachricht trägt die Namen, damit der Client Abweichungen erkennt.
export namespace ngine::replication {

template <typename T>
using Result = std::expected<T, std::string>;

inline constexpr u32 protocol_version = 1;

enum class MessageKind : u8 {
    Welcome = 1,
    Snapshot = 2,
    // Nutzdaten der Anwendung, z. B. Eingaben des Clients.
    Application = 3,
};

// Markiert auf dem Server, was repliziert wird. Auf dem Client trägt jede gespiegelte Entity die
// Kennung, die sie auf dem Server hat.
struct Replicated {
    u64 server_id = 0;
};

// Registriert Replicated in der Reflection; mehrfaches Aufrufen schadet nicht.
void register_types(reflect::TypeRegistry& types);

// Die replizierten Komponenten, in fester Reihenfolge. Höchstens 65535.
using ComponentList = std::span<const reflect::TypeInfo* const>;

[[nodiscard]] std::vector<std::byte> encode_welcome(ComponentList components);
// Prüft eine Willkommensnachricht (ohne das Byte MessageKind) gegen die eigene Liste.
[[nodiscard]] Result<void> check_welcome(std::span<const std::byte> payload,
                                         ComponentList components);

// Schreibt alle Entities mit Replicated und die Komponenten aus components, die sie haben.
[[nodiscard]] std::vector<std::byte> encode_snapshot(world::World& world, ComponentList components,
                                                     u32 tick);

// Spiegel der Server-Welt auf dem Client. Legt Entities an, die neu sind, setzt und entfernt
// Komponenten und zerstört Entities, die im Schnappschuss fehlen. Andere Entities der Welt
// bleiben unberührt.
class Mirror {
public:
    Mirror(world::World& world, ComponentList components);

    // payload ohne das Byte MessageKind. Ältere Schnappschüsse als der zuletzt angewendete werden
    // verworfen (unzuverlässige Nachrichten können sich überholen); dann ist das Ergebnis false.
    [[nodiscard]] Result<bool> apply(std::span<const std::byte> payload);

    // Lokale Kennung zur Kennung auf dem Server, 0 wenn unbekannt.
    [[nodiscard]] u64 local_id(u64 server_id) const noexcept;
    [[nodiscard]] usize entity_count() const noexcept {
        return entities_.size();
    }
    [[nodiscard]] u32 last_tick() const noexcept {
        return last_tick_;
    }
    // Zerstört alle gespiegelten Entities, z. B. nach dem Trennen.
    void clear();

private:
    world::World* world_;
    std::vector<const reflect::TypeInfo*> components_;
    std::vector<world::ComponentId> component_ids_;
    std::unordered_map<u64, u64> entities_;
    u32 last_tick_ = 0;
    bool has_tick_ = false;
};

} // namespace ngine::replication
