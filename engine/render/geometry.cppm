module;

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

export module ngine.render:geometry;

import ngine.core;
import ngine.rhi;

export namespace ngine::render {

// Verwaltet freie Bereiche in einem Speicher fester Größe (erster passender Bereich, benachbarte
// freie Bereiche verschmelzen). Kennt keine GPU und lässt sich so ohne Treiber testen.
class RangeAllocator {
public:
    explicit RangeAllocator(u64 capacity);

    // Anfang eines Bereichs der Größe size mit der Ausrichtung alignment (Zweierpotenz), nichts,
    // wenn kein freier Bereich groß genug ist.
    [[nodiscard]] std::optional<u64> allocate(u64 size, u64 alignment = 1);
    // Gibt einen Bereich zurück, den allocate() geliefert hat.
    void free(u64 offset, u64 size);

    [[nodiscard]] u64 capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] u64 used() const noexcept {
        return used_;
    }
    [[nodiscard]] u64 largest_free() const noexcept;
    // Zahl der freien Bereiche (Maß für Zerstückelung).
    [[nodiscard]] usize free_ranges() const noexcept {
        return free_.size();
    }

private:
    u64 capacity_;
    u64 used_ = 0;
    // Anfang → Größe aller freien Bereiche.
    std::map<u64, u64> free_;
};

// Ein Bereich in einem GeometryBuffer.
struct GeometryRange {
    static constexpr u32 no_block = ~u32{0};
    u32 block = no_block;
    u64 offset = 0;
    u64 size = 0;

    [[nodiscard]] bool valid() const noexcept {
        return block != no_block;
    }
};

struct GeometryStats {
    u32 blocks = 0;
    u64 capacity = 0;
    u64 used = 0;
    u32 free_ranges = 0;
};

// Gemeinsame GPU-Puffer für die Geometrie aller Netze (TD-23, ADR 0038): Eckpunkte und Indizes
// liegen als Bereiche in wenigen großen Puffern (Blöcken) statt in je eigenen. Der Shader liest
// Eckpunkte über die Adresse des Bereichs, Indizes kommen aus dem Puffer des Blocks. Freigegebene
// Bereiche werden erst wiederverwendet, wenn kein Frame im Flug sie mehr liest (end_frame()).
// Das ist die Grundlage, auf der R3.5 alle Netze mit wenigen indirekten Draws zeichnet.
class GeometryBuffer {
public:
    // Neue Blöcke haben mindestens block_size Bytes.
    explicit GeometryBuffer(rhi::Device& device, u64 block_size = u64{32} << 20);
    GeometryBuffer(const GeometryBuffer&) = delete;
    GeometryBuffer& operator=(const GeometryBuffer&) = delete;

    // Bereich für size Bytes, ausgerichtet auf alignment; legt bei Bedarf einen Block an.
    [[nodiscard]] rhi::Result<GeometryRange> allocate(u64 size, u64 alignment = 64);
    // Gibt einen Bereich frei, sobald die Frames im Flug fertig sind.
    void free(GeometryRange range);
    // Ein Frame ist abgegeben; Bereiche, die seit genug Frames frei sind, werden wiederverwendet.
    void end_frame();

    // Kopiert data an den Anfang des Bereichs (Device::upload(), vor der nächsten Abgabe) bzw.
    // im Hintergrund (Device::upload_async()).
    void upload(GeometryRange range, std::span<const std::byte> data);
    [[nodiscard]] rhi::UploadTicket upload_async(GeometryRange range,
                                                 std::span<const std::byte> data);

    [[nodiscard]] rhi::Buffer& buffer(GeometryRange range) const;
    [[nodiscard]] u64 address(GeometryRange range) const;
    [[nodiscard]] GeometryStats stats() const noexcept;

private:
    struct Block {
        std::unique_ptr<rhi::Buffer> buffer;
        RangeAllocator ranges;
    };
    struct Pending {
        GeometryRange range;
        u64 frame = 0;
    };

    rhi::Device* device_;
    u64 block_size_;
    std::vector<Block> blocks_;
    std::vector<Pending> pending_;
    u64 frame_ = 0;
};

} // namespace ngine::render
