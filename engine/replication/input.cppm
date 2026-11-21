module;

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Nachrichten des Spiels zwischen Client und Server (R5.1), als Nutzdaten der Replikation
// (Received, Client::send(), Server::send()). Das erste Byte ist die Art (GameMessage).
//
//   Actions: u32 Prüfsumme der Belegung (input::ActionMap::hash()), u16 Anzahl, dann je Aktion
//            f32 x, f32 y, u8 Flanken (1 gehalten, 2 gedrückt, 4 losgelassen)
//   Possess: u64 Kennung der Figur auf dem Server
//
// Zahlen stehen in Little Endian. Die Eingabe geht in R5.1 jedes Bild unzuverlässig an den
// Server; Nummern pro festem Schritt und Wiederholung gegen Verlust kommen mit der Vorhersage
// (R5.3).
export module ngine.replication:input;

import ngine.core;
import ngine.input;
import :snapshot;

export namespace ngine::replication {

enum class GameMessage : u8 {
    // Client an Server: Werte aller Aktionen eines Bildes.
    Actions = 1,
    // Server an Client: Diese Figur steuerst du.
    Possess = 2,
};

// Art einer Nachricht des Spiels; nullopt bei leeren oder unbekannten.
[[nodiscard]] std::optional<GameMessage> game_message(std::span<const std::byte> data) noexcept;

[[nodiscard]] std::vector<std::byte> encode_actions(u32 map_hash,
                                                    std::span<const input::ActionValue> values);
// Prüft Art, Länge und Prüfsumme der Belegung.
[[nodiscard]] Result<std::vector<input::ActionValue>>
decode_actions(std::span<const std::byte> data, u32 map_hash);

[[nodiscard]] std::vector<std::byte> encode_possess(u64 entity);
[[nodiscard]] Result<u64> decode_possess(std::span<const std::byte> data);

} // namespace ngine::replication
