module;

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module ngine.replication:snapshot;

import ngine.core;
import ngine.math;
import ngine.reflect;
import ngine.world;
import :schema;

// Schnappschüsse als Delta (R5.2, ADR 0052), ohne Netzwerk, damit sie sich einzeln testen lassen.
//
// Der Server liest pro Schnappschuss einmal die Welt (SnapshotEncoder::capture()): für jede Entity
// mit replizierten Komponenten die kodierten Teile (EntityRecord). Pro Client merkt er sich, welche
// Stände er geschickt hat (Views der letzten 32 Ticks), und kodiert gegen den Stand, den der Client
// zuletzt bestätigt hat. Der Client (Mirror) hält dieselben Views, baut aus Basis und Delta den
// neuen Stand und gleicht seine Welt damit ab.
//
// Jede Nachricht beginnt mit einem Byte MessageKind. Ein Schnappschuss enthält (Zahlen in Little
// Endian, var = LEB128):
//   u32 Tick, u32 Basis-Tick (0: ohne Basis),
//   var Anzahl entfernter Entities, je u64 Kennung auf dem Server,
//   var Anzahl Einträge, je Eintrag
//     u64 Kennung, u8 Flags (1 neu, 2 Elternteil folgt), [u64 Elternteil auf dem Server],
//     var Anzahl entfernter Komponenten, je u16 Index im Schema,
//     var Anzahl geänderter Komponenten, je u16 Index, var Maske der Teile, je Teil var Länge
//     und Bytes.
export namespace ngine::replication {

template <typename T>
using Result = std::expected<T, std::string>;

// Schnappschüsse pro Sekunde; Ticks der Schnappschüsse zählen in diesem Takt.
inline constexpr f64 snapshot_rate = 30.0;

enum class MessageKind : u8 {
    Welcome = 1,
    Snapshot = 2,
    // Nutzdaten der Anwendung, z. B. Eingaben des Clients.
    Application = 3,
    // Client an Server: neuester angewendeter Tick (u32).
    Ack = 4,
};

// Auf dem Client trägt jede gespiegelte Entity die Kennung, die sie auf dem Server hat.
struct Replicated {
    u64 server_id = 0;
};

// Registriert Replicated in der Reflection; mehrfaches Aufrufen schadet nicht.
void register_types(reflect::TypeRegistry& types);

// Protokoll, Hash und Namen der Komponenten.
[[nodiscard]] std::vector<std::byte> encode_welcome(const Schema& schema);
// Prüft eine Willkommensnachricht (ohne das Byte MessageKind) gegen das eigene Schema.
[[nodiscard]] Result<void> check_welcome(std::span<const std::byte> payload, const Schema& schema);

[[nodiscard]] std::vector<std::byte> encode_ack(u32 tick);
// payload ohne das Byte MessageKind.
[[nodiscard]] std::optional<u32> decode_ack(std::span<const std::byte> payload);

// Stand einer Entity, wie ihn Server und Client für Deltas vergleichen: die kodierten Teile ihrer
// replizierten Komponenten. Unveränderlich, sobald er in einer View steht.
struct EntityRecord {
    u64 id = 0;
    // Elternteil auf dem Server, 0 ohne.
    u64 parent = 0;
    // Indizes im Schema, aufsteigend.
    std::vector<u16> components;
    // Pro Komponente der Index ihres ersten Teils.
    std::vector<u32> first_part;
    // Ende jedes Teils in bytes.
    std::vector<u32> part_ends;
    std::vector<std::byte> bytes;

    [[nodiscard]] std::span<const std::byte> part(usize index) const noexcept {
        const u32 begin = index == 0 ? 0 : part_ends[index - 1];
        return {bytes.data() + begin, part_ends[index] - begin};
    }
    // Platz der Komponente in components, -1 wenn die Entity sie nicht hat.
    [[nodiscard]] i32 slot(u16 component) const noexcept;
};

using RecordPtr = std::shared_ptr<const EntityRecord>;
// Stände aller Entities, die ein Client kennt, nach Kennung sortiert.
using View = std::vector<RecordPtr>;

struct EncoderSettings {
    // Entities weiter als so viele Meter von der Figur des Clients schickt der Server nicht.
    f64 relevance = 150.0;
    // Bis hierhin jeder Schnappschuss, darüber nur jeder far_interval-te.
    f64 near = 40.0;
    u32 far_interval = 3;
    // Höchstens so viele Bytes pro Schnappschuss und Client (64 KB/s bei 30 Hz).
    usize budget = 65536 / 30;
    // Gesendete Stände, gegen die ein Delta gehen darf (gut eine Sekunde).
    u32 history = 32;
};

struct EncoderClientStats {
    // Größe des letzten Schnappschusses.
    usize last_bytes = 0;
    // Bytes der Schnappschüsse der letzten Sekunde (30 Stück).
    usize bytes_per_second = 0;
    // Entities, die der Client nach dem letzten Schnappschuss kennt.
    u32 entities = 0;
    // Geänderte Entities, die das Budget oder die Abstufung warten ließ.
    u32 waiting = 0;
    // Schnappschüsse ohne Basis (erster, oder Bestätigung zu alt).
    u32 full = 0;
    u32 acked_tick = 0;
};

// Kodiert Schnappschüsse für viele Clients. Gehört dem Thread der Welt.
class SnapshotEncoder {
public:
    // world, schema und die Registry müssen den Encoder überleben.
    SnapshotEncoder(world::World& world, const Schema& schema, EncoderSettings settings = {});
    ~SnapshotEncoder();
    SnapshotEncoder(const SnapshotEncoder&) = delete;
    SnapshotEncoder& operator=(const SnapshotEncoder&) = delete;

    // Entities mit einer dieser Komponenten sind für jeden Client relevant (Terrain, Sonne).
    void set_global(std::span<const reflect::TypeInfo* const> types);

    void add_client(u32 client);
    void remove_client(u32 client);
    // Mittelpunkt der Relevanz (Figur des Clients); ohne Angabe ist alles relevant.
    void set_focus(u32 client, std::optional<math::DVec3> focus);
    // Der Client hat den Schnappschuss tick angewendet.
    void acknowledge(u32 client, u32 tick);
    // Ist entity jetzt für client relevant (Ereignisse der Skripte, R5.5)? Wie beim Schnappschuss:
    // ohne Mittelpunkt, ohne Lage oder mit globaler Komponente immer, sonst bis zur Reichweite.
    [[nodiscard]] bool relevant(u32 client, u64 entity) const;
    // Ist position für client in Reichweite radius (Hörweite eines Klangs)?
    [[nodiscard]] bool near(u32 client, const math::DVec3& position, f64 radius) const;

    // Liest die Welt für den Schnappschuss tick (aufsteigend, ab 1).
    void capture(u32 tick);
    // Schnappschuss für client mit dem Byte MessageKind, nach capture().
    [[nodiscard]] std::vector<std::byte> encode(u32 client);

    [[nodiscard]] EncoderClientStats stats(u32 client) const;
    // Replizierte Entities beim letzten capture().
    [[nodiscard]] usize entity_count() const noexcept;
    // Stand aller replizierten Entities beim letzten capture() (Vergleich in Tests).
    [[nodiscard]] View captured() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct MirrorStats {
    // Schnappschüsse, deren Basis der Client nicht (mehr) kennt.
    u32 missing_base = 0;
    // Zu spät angekommene (älter als der letzte angewendete).
    u32 stale = 0;
    // Wie weit die gezeichnete Zeit hinter dem neuesten Schnappschuss liegt, in Ticks.
    f64 buffer_ticks = 0.0;
    // Entities, deren Transform interpoliert wird.
    u32 interpolated = 0;
    // Bilder, in denen die Zeit über den neuesten Schnappschuss hinaus lief.
    u32 extrapolated = 0;
};

// Spiegel der Server-Welt auf dem Client. Legt Entities an, setzt und entfernt Komponenten,
// hängt sie um und zerstört Entities, die im Stand fehlen. Andere Entities der Welt bleiben
// unberührt. Verweise auf Entities (reflect::Hint::Entity) übersetzt er in eigene Kennungen.
class Mirror {
public:
    // world, schema und die Registry müssen den Spiegel überleben.
    Mirror(world::World& world, const Schema& schema);
    ~Mirror();
    Mirror(const Mirror&) = delete;
    Mirror& operator=(const Mirror&) = delete;

    // payload ohne das Byte MessageKind. false, wenn der Schnappschuss verworfen wurde: älter als
    // der zuletzt angewendete oder gegen eine Basis, die der Client nicht kennt.
    [[nodiscard]] Result<bool> apply(std::span<const std::byte> payload);

    // Zeichnet Transforms bewegter Entities interpoliert, delay Ticks hinter dem neuesten
    // Schnappschuss; dt ist die Bildzeit in Sekunden. Einmal pro Bild nach apply().
    void interpolate(f64 dt, f64 delay = 3.0);

    // Eigene, vorhergesagte Figur (R5.3): Die Komponenten in owned setzt der Client für diese
    // Entity selbst, sobald sie da sind (z. B. Transform und CharacterMotion); ihr Transform wird
    // nicht interpoliert. Die übrigen Komponenten spiegelt er weiter. server_id 0 hebt es auf.
    void set_predicted(u64 server_id, std::span<const reflect::TypeInfo* const> owned);

    // Lokale Kennung zur Kennung auf dem Server, 0 wenn unbekannt.
    [[nodiscard]] u64 local_id(u64 server_id) const noexcept;
    [[nodiscard]] usize entity_count() const noexcept;
    [[nodiscard]] u32 last_tick() const noexcept;
    // Gezeichnete Zeit in Ticks des Servers (nach interpolate(); Takt der Ereignisse, R5.5).
    [[nodiscard]] f64 render_tick() const noexcept;
    [[nodiscard]] MirrorStats stats() const noexcept;
    // Der zuletzt angewendete Stand (z. B. für Vergleiche mit dem Server in Tests).
    [[nodiscard]] const View& view() const noexcept;
    // Zerstört alle gespiegelten Entities, z. B. nach dem Trennen.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ngine::replication
