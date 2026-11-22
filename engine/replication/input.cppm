module;

#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Nachrichten des Spiels zwischen Client und Server (R5.1, R5.3), als Nutzdaten der Replikation
// (Received, Client::send(), Server::send()). Das erste Byte ist die Art (GameMessage).
//
//   Commands: u32 Prüfsumme der Belegung (input::ActionMap::hash()), u16 Zahl der Aktionen, u8
//             Zahl der Befehle (1 bis max_commands_per_message), dann je Befehl u32 Nummer, f32
//             yaw, f32 pitch und je Aktion ein Byte (1 gehalten, 2 gedrückt, 4 losgelassen,
//             8 Wert folgt), bei 8 dazu f32 x, f32 y.
//   Possess:  u64 Kennung der Figur auf dem Server
//   Owner:    u32 letzter verarbeiteter Befehl, u8 Befehle in der Warteschlange, f64 x, y, z,
//             f32 vx, vy, vz, u8 Boden, u8 geduckt: Stand der eigenen Figur nach diesem Befehl.
//
// Zahlen stehen in Little Endian. Bis R5.2 gingen pro Bild die Aktionen (Actions) und der Blick
// (View) einzeln; seit R5.3 ist ein Befehl alles, was eine Figur in einem festen Schritt braucht,
// nummeriert und in jedem Paket mit den beiden davor wiederholt (ADR 0053).
export module ngine.replication:input;

import ngine.core;
import ngine.input;
import ngine.math;
import :snapshot;

export namespace ngine::replication {

enum class GameMessage : u8 {
    // Client an Server: die neuesten Befehle.
    Commands = 1,
    // Server an Client: Diese Figur steuerst du.
    Possess = 2,
    // Server an Client: Stand deiner Figur nach deinem letzten verarbeiteten Befehl.
    Owner = 3,
};

// So viele Befehle trägt eine Nachricht höchstens; der Client schickt die neuesten drei.
inline constexpr usize max_commands_per_message = 8;
inline constexpr usize commands_per_message = 3;

// Eingabe einer Figur für einen festen Schritt (R5.3).
struct Command {
    // Zählt der Client ab 1, einer pro festem Schritt.
    u32 number = 0;
    // Blick der Figur (Bogenmaß).
    f32 yaw = 0.0f;
    f32 pitch = 0.0f;
    // Werte und Flanken aller Aktionen in der Reihenfolge der Belegung.
    std::vector<input::ActionValue> actions;
};

// Art einer Nachricht des Spiels; nullopt bei leeren oder unbekannten.
[[nodiscard]] std::optional<GameMessage> game_message(std::span<const std::byte> data) noexcept;

// Höchstens max_commands_per_message Befehle (die neuesten zuletzt) mit gleich vielen Aktionen.
[[nodiscard]] std::vector<std::byte> encode_commands(u32 map_hash,
                                                     std::span<const Command> commands);
// Prüft Art, Länge, Prüfsumme der Belegung und endliche Werte.
[[nodiscard]] Result<std::vector<Command>> decode_commands(std::span<const std::byte> data,
                                                           u32 map_hash);

// Hält einen Befehl in den Grenzen der Belegung (Länge von Achsen bis Action::limit, Tasten 0 bis
// 1) und pitch in ±max_pitch. Liefert die Zahl der geänderten Werte (0: alles in Ordnung).
u32 limit_command(Command& command, const input::ActionMap& map, f32 max_pitch);

[[nodiscard]] std::vector<std::byte> encode_possess(u64 entity);
[[nodiscard]] Result<u64> decode_possess(std::span<const std::byte> data);

// Stand der eigenen Figur, wie ihn Server und Client vergleichen.
struct OwnerState {
    u32 command = 0;
    // Befehle, die beim Server nach diesem noch warten (Takt des Clients).
    u8 queued = 0;
    math::DVec3 position{0.0, 0.0, 0.0};
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    // physics::GroundState als Zahl.
    u8 ground = 0;
    bool crouched = false;
};

[[nodiscard]] std::vector<std::byte> encode_owner(const OwnerState& state);
[[nodiscard]] Result<OwnerState> decode_owner(std::span<const std::byte> data);

struct CommandQueueStats {
    u64 received = 0;
    u64 processed = 0;
    // Fehlte der nächste Befehl, galt der letzte noch einmal (ohne Flanken).
    u64 repeated = 0;
    // Doppelt, zu spät oder älter als der zuletzt verarbeitete.
    u64 dropped = 0;
    // Wegen voller Warteschlange verworfen.
    u64 overflow = 0;
};

// Befehle eines Spielers auf dem Server (R5.3): sortiert nach Nummer, einer pro festem Schritt.
// Die Abarbeitung beginnt, sobald target Befehle warten (Puffer gegen Schwankungen). Fehlt der
// nächste, gilt der letzte ohne Flanken noch einmal unter der fehlenden Nummer; kommt er später,
// wird er verworfen. Über capacity verwirft die Schlange die ältesten.
class CommandQueue {
public:
    explicit CommandQueue(usize target = 2, usize capacity = 8);

    void push(Command command);
    // Befehl für diesen Schritt; nullopt, solange die Abarbeitung nicht begonnen hat.
    [[nodiscard]] std::optional<Command> next();

    [[nodiscard]] usize size() const noexcept {
        return queue_.size();
    }
    [[nodiscard]] bool started() const noexcept {
        return started_;
    }
    // Nummer des zuletzt verarbeiteten Befehls (0 vor dem ersten).
    [[nodiscard]] u32 last() const noexcept {
        return last_ ? last_->number : 0;
    }
    [[nodiscard]] const CommandQueueStats& stats() const noexcept {
        return stats_;
    }

private:
    usize target_;
    usize capacity_;
    std::deque<Command> queue_;
    std::optional<Command> last_;
    u32 next_ = 0;
    bool started_ = false;
    CommandQueueStats stats_;
};

} // namespace ngine::replication
