module;

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Terrain übers Netz (R5.4, NET-06, VOX-06, ADR 0054). Der Server ist die Wahrheit: Er gräbt, wenn
// ein Befehl es verlangt, und schickt die geänderten Deltas der Chunks (ADR 0042) zuverlässig an
// die Clients, die in der Relevanz sofort, den Rest nach Abstand mit eigenem Budget. Der Client
// setzt sie in sein Terrain und zeigt seinen eigenen Pinsel sofort, bis der Server den Befehl
// bestätigt.
//
// Nachricht Terrain (Little Endian): u8 Art (GameMessage::Terrain), u64 Kennung der Terrain-Entity
// auf dem Server, u32 letzter verarbeiteter Befehl des Empfängers, u8 Flags (1 Umgebung
// vollständig), Varint Zahl der Chunks, dann je Chunk x, y, z (Stufe 0) als Varints mit
// Vorzeichen (ZigZag), Varint Länge und die Bytes des Deltas (voxel::ChunkDelta). Länge 0 heißt:
// kein Delta mehr, der Chunk ist wieder Grundwelt.
export module ngine.replication:terrain;

import ngine.core;
import ngine.math;
import ngine.world;
import :snapshot;

export namespace ngine::replication {

// Ein Chunk der Stufe 0 mit seinem Delta; nullptr heißt Grundwelt.
struct TerrainChunk {
    voxel::ChunkCoord coord;
    std::shared_ptr<const voxel::ChunkDelta> delta;
};

struct TerrainMessage {
    // Kennung der Terrain-Entity auf dem Server.
    u64 terrain = 0;
    // Letzter Befehl des Empfängers, den der Server verarbeitet hatte, als er die Nachricht
    // schrieb: Alle Änderungen bis dahin sind mit dieser oder einer früheren Nachricht gekommen.
    u32 acknowledged = 0;
    // In der Relevanz um die Figur des Empfängers fehlt nichts mehr.
    bool complete = false;
    std::vector<TerrainChunk> chunks;
};

[[nodiscard]] std::vector<std::byte> encode_terrain(const TerrainMessage& message);
// Prüft Art, Längen, Koordinaten und jedes Delta.
[[nodiscard]] Result<TerrainMessage> decode_terrain(std::span<const std::byte> data);

// --- Server ---------------------------------------------------------------------------------

struct TerrainSenderSettings {
    // Chunks näher an der Figur (Mittelpunkt, m) gehen sofort und ohne Budget.
    f64 relevance = 150.0;
    // Bytes pro Aufruf von encode() für alles außerhalb der Relevanz (128 KB/s bei 30 Hz).
    usize budget = 131072 / 30;
};

struct TerrainClientStats {
    // Geschickte Chunks und Bytes der Nachrichten insgesamt.
    u64 chunks = 0;
    u64 bytes = 0;
    // Bytes bis zur ersten Nachricht mit „Umgebung vollständig“ und ob sie schon ging.
    u64 join_bytes = 0;
    bool complete = false;
    // Noch vorgemerkte Chunks über alle Terrains.
    usize pending = 0;
};

// Merkt sich pro Client und Terrain, welches Delta jedes Chunks er zuletzt bekam (Zeiger: Deltas
// sind unveränderlich und geteilt), und schickt nur, was sich seitdem geändert hat.
class TerrainSender {
public:
    explicit TerrainSender(TerrainSenderSettings settings = {});

    void add_client(u32 client);
    void remove_client(u32 client);
    // Mittelpunkt der Relevanz (Figur); ohne gilt alles als fern.
    void set_focus(u32 client, std::optional<math::DVec3> focus);

    // Stand eines Terrains: Momentaufnahme seiner Deltas und Ursprung der Voxel in der Welt. Ist
    // die Momentaufnahme eine andere als zuletzt, merkt der Sender die geänderten Chunks bei allen
    // Clients vor.
    void update_terrain(u64 terrain, std::shared_ptr<const voxel::DeltaMap> deltas,
                        math::DVec3 origin);
    // Terrains, die nicht in keep stehen, vergisst der Sender.
    void retain_terrains(std::span<const u64> keep);

    // Nachrichten für einen Client, eine pro Terrain mit etwas zu schicken: vorgemerkte Chunks
    // (in der Relevanz alle, sonst nach Abstand im Budget), das erste „Umgebung vollständig“ oder,
    // mit force, nur die Bestätigung acknowledged (der Client hat gegraben).
    [[nodiscard]] std::vector<std::vector<std::byte>> encode(u32 client, u32 acknowledged,
                                                             bool force = false);

    [[nodiscard]] TerrainClientStats stats(u32 client) const;
    [[nodiscard]] const TerrainSenderSettings& settings() const noexcept {
        return settings_;
    }

private:
    struct TerrainEntry {
        std::shared_ptr<const voxel::DeltaMap> deltas;
        math::DVec3 origin{0.0, 0.0, 0.0};
    };
    struct ClientTerrain {
        std::unordered_map<voxel::ChunkCoord, std::shared_ptr<const voxel::ChunkDelta>,
                           voxel::ChunkCoordHash>
            sent;
        std::unordered_set<voxel::ChunkCoord, voxel::ChunkCoordHash> pending;
        bool complete = false;
    };
    struct ClientEntry {
        std::optional<math::DVec3> focus;
        std::unordered_map<u64, ClientTerrain> terrains;
        TerrainClientStats stats;
    };

    void mark_all(ClientTerrain& client, const voxel::DeltaMap& deltas) const;

    TerrainSenderSettings settings_;
    std::unordered_map<u64, TerrainEntry> terrains_;
    std::unordered_map<u32, ClientEntry> clients_;
};

// --- Client ---------------------------------------------------------------------------------

struct TerrainReceiverStats {
    u64 messages = 0;
    u64 chunks = 0;
    u64 bytes = 0;
    // Vorhergesagte Pinsel: offen, vom Server bestätigt, davon mit anderem Ergebnis als beim
    // Client, und ohne Bestätigung verfallen.
    usize predicted = 0;
    u64 confirmed = 0;
    u64 mismatched = 0;
    u64 expired = 0;
    // Nachrichten, die auf ihr Terrain warten.
    usize waiting = 0;
};

// Wendet die Deltas des Servers auf die Terrains der eigenen Welt an und hält den eigenen Pinsel
// vorhergesagt, bis der Server den Befehl bestätigt (R5.4). Gehört dem Thread der Welt.
class TerrainReceiver {
public:
    // Eine Nachricht des Servers (GameMessage::Terrain); Fehler bei kaputten.
    Result<void> receive(std::span<const std::byte> data);
    void receive(TerrainMessage message);

    // Wendet wartende Nachrichten an, deren Terrain der Spiegel kennt und das schon Laufzeitdaten
    // hat; die Reihenfolge pro Terrain bleibt. Liefert die Zahl der angewandten Nachrichten.
    usize apply(world::World& world, const Mirror& mirror);
    // Dasselbe mit eigener Übersetzung der Kennungen des Servers in eigene (0: unbekannt).
    usize apply(world::World& world, const std::function<u64(u64)>& local_id);

    // Eigener Pinsel mit Befehl command auf ein Terrain der eigenen Welt: sofort anwenden und
    // merken, bis eine Nachricht für dieses Terrain command bestätigt.
    void predict(world::Entity terrain, u32 command, const voxel::Brush& brush);
    // Pinsel, die der Server längst verarbeitet haben müsste (Befehl weit vor acknowledged, aus
    // dem Stand der eigenen Figur), gehen zurück auf den bestätigten Stand.
    void expire(world::World& world, u32 acknowledged, u32 margin = 60);

    // Eine Nachricht mit „Umgebung vollständig“ ist angekommen und angewandt.
    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }
    [[nodiscard]] TerrainReceiverStats stats() const noexcept;
    // Bestätigter Stand eines Chunks (vom Server) eines Terrains nach seiner Kennung auf dem
    // Server; nullptr: Grundwelt oder nie bekommen.
    [[nodiscard]] std::shared_ptr<const voxel::ChunkDelta> confirmed(u64 server_terrain,
                                                                    voxel::ChunkCoord coord) const;
    void clear();

private:
    using DeltaTable =
        std::unordered_map<voxel::ChunkCoord, std::shared_ptr<const voxel::ChunkDelta>,
                           voxel::ChunkCoordHash>;
    struct Prediction {
        // Kennung des Terrains auf dem Server.
        u64 terrain = 0;
        u32 command = 0;
        voxel::Brush brush;
        // Chunks, die der Pinsel beim letzten Anwenden änderte, und ihr Delta danach.
        std::vector<voxel::DeltaChange> changes;
    };
    // Ein Terrain des Servers: seine Entity in der eigenen Welt und der bestätigte Stand.
    struct Known {
        u64 local = 0;
        DeltaTable confirmed;
    };

    // Setzt die Chunks auf den bestätigten Stand, verwirft Pinsel bis acknowledged und wendet die
    // übrigen neu an; mit expiring zählen verworfene als verfallen.
    void reconcile(u64 server, world::Entity entity, std::span<const TerrainChunk> chunks,
                   std::optional<u32> acknowledged, bool expiring);

    std::vector<TerrainMessage> queue_;
    std::unordered_map<u64, Known> terrains_;
    std::vector<Prediction> predictions_;
    bool complete_ = false;
    TerrainReceiverStats stats_;
};

} // namespace ngine::replication
