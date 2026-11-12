module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.profile;

namespace ngine::voxel {

namespace {

// Zwischengespeicherte Chunks der Grundwelt; ein Pinselstrich berührt selten mehr als 27.
constexpr usize base_cache_limit = 64;

void write_varint(std::vector<std::byte>& out, usize value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::byte>(value));
}

// Liest ein Varint mit Prüfung; nullopt bei abgeschnittenen oder zu langen Werten.
std::optional<usize> checked_varint(std::span<const std::byte> bytes, usize& pos) {
    usize value = 0;
    for (u32 shift = 0; shift < 35; shift += 7) {
        if (pos >= bytes.size()) {
            return std::nullopt;
        }
        const auto b = static_cast<u8>(bytes[pos++]);
        value |= static_cast<usize>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            return value;
        }
    }
    return std::nullopt;
}

void write_u32(std::vector<std::byte>& out, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}

std::optional<u32> read_u32(std::span<const std::byte> bytes, usize& pos) {
    if (pos + 4 > bytes.size()) {
        return std::nullopt;
    }
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) {
        value |= static_cast<u32>(bytes[pos + i]) << (8 * i);
    }
    pos += 4;
    return value;
}

Sample sample_at(const Chunk& chunk, usize index) {
    return chunk.uniform() ? chunk.uniform_value() : chunk.samples()[index];
}

// Ein geändertes Sample der Stufe 0 auf dem Raster eines groben Chunks mit Schrittweite step
// (siehe apply_deltas()).
Sample coarse_sample(Sample base, Sample edited, i64 step) {
    const auto fstep = static_cast<f32>(step);
    const f32 e = dequantize(edited.density);
    i8 density = 0;
    if (edited.density > -127 && edited.density < 127) {
        density = quantize(e / fstep);
    } else {
        const bool solid = edited.density > 0;
        const f32 own = dequantize(base.density) * fstep;
        const bool same_side = (base.density > 0) == solid;
        density = same_side && (solid ? own >= e : own <= e)
                      ? base.density
                      : quantize((solid ? max_distance : -max_distance) / fstep);
    }
    if (density <= 0) {
        return {density, air};
    }
    if (edited.material != air) {
        return {density, edited.material};
    }
    return {density, base.material != air ? base.material : rock};
}

bool overlaps(const SampleBox& a, const SampleBox& b) noexcept {
    return a.min_x < b.max_x && b.min_x < a.max_x && a.min_y < b.max_y && b.min_y < a.max_y &&
           a.min_z < b.max_z && b.min_z < a.max_z;
}

SampleBox level0_box(ChunkCoord c) noexcept {
    const i64 x = chunk_origin(c.x, c.level);
    const i64 y = chunk_origin(c.y, c.level);
    const i64 z = chunk_origin(c.z, c.level);
    const i64 e = i64{chunk_voxels} << c.level;
    return {x, y, z, x + e, y + e, z + e};
}

SampleBox merge(const SampleBox& a, const SampleBox& b) noexcept {
    if (a.empty()) {
        return b;
    }
    return {std::min(a.min_x, b.min_x), std::min(a.min_y, b.min_y), std::min(a.min_z, b.min_z),
            std::max(a.max_x, b.max_x), std::max(a.max_y, b.max_y), std::max(a.max_z, b.max_z)};
}

} // namespace

// --- ChunkDelta ------------------------------------------------------------------------------

usize ChunkDelta::read_varint(usize& pos) const noexcept {
    usize value = 0;
    for (u32 shift = 0; pos < bytes_.size(); shift += 7) {
        const auto b = static_cast<u8>(bytes_[pos++]);
        value |= static_cast<usize>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            break;
        }
    }
    return value;
}

ChunkDelta ChunkDelta::diff(const Chunk& base, const Chunk& edited) {
    ChunkDelta delta;
    if (base.uniform() && edited.uniform() && base.uniform_value() == edited.uniform_value()) {
        return delta;
    }
    usize unchanged = 0;
    usize index = 0;
    while (index < chunk_sample_count) {
        if (sample_at(base, index) == sample_at(edited, index)) {
            ++unchanged;
            ++index;
            continue;
        }
        usize end = index;
        while (end < chunk_sample_count && sample_at(base, end) != sample_at(edited, end)) {
            ++end;
        }
        write_varint(delta.bytes_, unchanged);
        write_varint(delta.bytes_, end - index);
        for (; index < end; ++index) {
            const Sample s = sample_at(edited, index);
            delta.bytes_.push_back(static_cast<std::byte>(s.density));
            delta.bytes_.push_back(static_cast<std::byte>(s.material));
            ++delta.changed_;
        }
        unchanged = 0;
    }
    delta.bytes_.shrink_to_fit();
    return delta;
}

Result<ChunkDelta> ChunkDelta::decode(std::span<const std::byte> bytes) {
    ChunkDelta delta;
    usize pos = 0;
    usize index = 0;
    while (pos < bytes.size()) {
        const auto skip = checked_varint(bytes, pos);
        const auto count = skip ? checked_varint(bytes, pos) : std::nullopt;
        if (!count || *count == 0 || index + *skip + *count > chunk_sample_count ||
            pos + *count * 2 > bytes.size()) {
            return std::unexpected(std::string{"Delta eines Chunks ist beschädigt"});
        }
        index += *skip + *count;
        pos += *count * 2;
        delta.changed_ += static_cast<u32>(*count);
    }
    delta.bytes_.assign(bytes.begin(), bytes.end());
    return delta;
}

void ChunkDelta::apply(Chunk& chunk) const {
    if (empty()) {
        return;
    }
    const std::span<Sample> samples = chunk.expand();
    for_each([&](usize index, Sample s) { samples[index] = s; });
    chunk.compact();
}

// --- Einrechnen in alle Stufen ------------------------------------------------------------------

void apply_deltas(const DeltaMap& deltas, ChunkCoord coord, Chunk& chunk) {
    if (deltas.empty()) {
        return;
    }
    if (coord.level == 0) {
        if (const auto it = deltas.find(coord); it != deltas.end()) {
            it->second->apply(chunk);
        }
        return;
    }
    const SampleBox box = level0_box(coord);
    const i64 step = i64{1} << coord.level;
    std::span<Sample> samples;
    for (const auto& [c, delta] : deltas) {
        if (!overlaps(level0_box(c), box)) {
            continue;
        }
        const i64 ox = chunk_origin(c.x);
        const i64 oy = chunk_origin(c.y);
        const i64 oz = chunk_origin(c.z);
        delta->for_each([&](usize index, Sample edited) {
            const auto i = static_cast<i64>(index);
            const i64 x = ox + i % chunk_voxels - box.min_x;
            const i64 y = oy + (i / chunk_voxels) % chunk_voxels - box.min_y;
            const i64 z = oz + i / (i64{chunk_voxels} * chunk_voxels) - box.min_z;
            if (x % step != 0 || y % step != 0 || z % step != 0) {
                return;
            }
            if (samples.empty()) {
                samples = chunk.expand();
            }
            const usize target = Chunk::index(
                static_cast<u32>(x / step), static_cast<u32>(y / step), static_cast<u32>(z / step));
            samples[target] = coarse_sample(samples[target], edited, step);
        });
    }
    if (!samples.empty()) {
        chunk.compact();
    }
}

// --- TerrainDeltas -------------------------------------------------------------------------------

TerrainDeltas::TerrainDeltas(const GeneratorDesc& generator)
    : generator_{generator}, map_{std::make_shared<DeltaMap>()} {}

void TerrainDeltas::set_generator(const GeneratorDesc& generator) {
    generator_ = Generator{generator};
    base_cache_.clear();
    base_order_.clear();
    ++version_;
}

void TerrainDeltas::assign(std::shared_ptr<const DeltaMap> deltas) {
    // Eigene Tabelle; die Einträge teilen sich beide.
    map_ = deltas ? std::make_shared<DeltaMap>(*deltas) : std::make_shared<DeltaMap>();
    std::erase_if(*map_, [](const auto& entry) { return !entry.second || entry.second->empty(); });
    ++version_;
}

void TerrainDeltas::clear() {
    map_ = std::make_shared<DeltaMap>();
    ++version_;
}

std::shared_ptr<const ChunkDelta> TerrainDeltas::find(ChunkCoord coord) const {
    const auto it = map_->find(coord);
    return it == map_->end() ? nullptr : it->second;
}

usize TerrainDeltas::encoded_bytes() const noexcept {
    usize bytes = 0;
    for (const auto& entry : *map_) {
        bytes += entry.second->bytes().size();
    }
    return bytes;
}

DeltaMap& TerrainDeltas::writable() {
    // Hält ein Job oder Befehl noch eine Momentaufnahme, bleibt sie unverändert. Kopien der
    // Momentaufnahme entstehen nur in diesem Thread, also kann use_count() höchstens zu groß sein.
    if (map_.use_count() > 1) {
        map_ = std::make_shared<DeltaMap>(*map_);
    }
    return *map_;
}

void TerrainDeltas::put(ChunkCoord coord, std::shared_ptr<const ChunkDelta> delta) {
    if (delta && !delta->empty()) {
        writable().insert_or_assign(coord, std::move(delta));
    } else if (map_->contains(coord)) {
        writable().erase(coord);
    }
}

const Chunk& TerrainDeltas::base(ChunkCoord coord) {
    if (const auto it = base_cache_.find(coord); it != base_cache_.end()) {
        return it->second;
    }
    if (base_order_.size() >= base_cache_limit) {
        base_cache_.erase(base_order_.front());
        base_order_.erase(base_order_.begin());
    }
    Chunk& chunk = base_cache_[coord];
    generator_.fill(coord, chunk);
    base_order_.push_back(coord);
    return chunk;
}

void TerrainDeltas::build(ChunkCoord coord, Chunk& out) const {
    generator_.fill(coord, out);
    apply_deltas(*map_, coord, out);
}

TerrainEdit TerrainDeltas::apply(ChunkStore& store, const Brush& brush, bool loaded_only) {
    NGINE_PROFILE_ZONE("voxel::TerrainDeltas::apply");
    TerrainEdit edit;
    if (brush.radius <= 0.0f) {
        return edit;
    }
    // Gerechnet wird in einer Kopie der berührten Chunks samt Nachbarn, die Glätten und Ebnen
    // lesen; fehlende entstehen aus Grundwelt und Deltas.
    const SampleBox box = brush_box(brush);
    const ChunkCoord low = chunk_of(box.min_x - 1, box.min_y - 1, box.min_z - 1);
    const ChunkCoord high = chunk_of(box.max_x, box.max_y, box.max_z);
    ChunkStore work{store.outside()};
    for (i32 z = low.z; z <= high.z; ++z) {
        for (i32 y = low.y; y <= high.y; ++y) {
            for (i32 x = low.x; x <= high.x; ++x) {
                const ChunkCoord c{x, y, z, 0};
                if (const Chunk* loaded = store.find(c)) {
                    work.get_or_create(c) = *loaded;
                } else if (!loaded_only) {
                    build(c, work.get_or_create(c));
                }
            }
        }
    }
    const SampleBox changed = voxel::apply(work, brush);
    if (changed.empty()) {
        return edit;
    }
    const ChunkCoord first = chunk_of(changed.min_x, changed.min_y, changed.min_z);
    const ChunkCoord last = chunk_of(changed.max_x - 1, changed.max_y - 1, changed.max_z - 1);
    for (i32 z = first.z; z <= last.z; ++z) {
        for (i32 y = first.y; y <= last.y; ++y) {
            for (i32 x = first.x; x <= last.x; ++x) {
                const ChunkCoord c{x, y, z, 0};
                Chunk* edited = work.find(c);
                Chunk* loaded = store.find(c);
                if (edited == nullptr || (loaded_only && loaded == nullptr)) {
                    continue;
                }
                auto delta = std::make_shared<const ChunkDelta>(ChunkDelta::diff(base(c), *edited));
                std::shared_ptr<const ChunkDelta> before = find(c);
                const bool same = before ? *before == *delta : delta->empty();
                if (same) {
                    continue;
                }
                std::shared_ptr<const ChunkDelta> after =
                    delta->empty() ? nullptr : std::shared_ptr<const ChunkDelta>{std::move(delta)};
                put(c, after);
                edit.changes.push_back({c, std::move(before), std::move(after)});
                if (loaded != nullptr) {
                    *loaded = std::move(*edited);
                }
            }
        }
    }
    if (!edit.changes.empty()) {
        store.mark_changed(changed);
        edit.box = changed;
        ++version_;
    }
    return edit;
}

SampleBox TerrainDeltas::restore(ChunkStore& store, std::span<const DeltaChange> changes,
                                 bool after) {
    SampleBox box;
    for (const DeltaChange& change : changes) {
        const std::shared_ptr<const ChunkDelta>& target = after ? change.after : change.before;
        if (find(change.coord) == target) {
            continue;
        }
        put(change.coord, target);
        box = merge(box, level0_box(change.coord));
        if (Chunk* loaded = store.find(change.coord)) {
            Chunk rebuilt = base(change.coord);
            if (target) {
                target->apply(rebuilt);
            }
            *loaded = std::move(rebuilt);
        }
    }
    if (!box.empty()) {
        store.mark_changed(box);
        ++version_;
    }
    return box;
}

// --- Regionen ----------------------------------------------------------------------------------

std::string region_file_name(RegionCoord region) {
    return "r." + std::to_string(region.x) + "." + std::to_string(region.y) + "." +
           std::to_string(region.z) + std::string{region_extension};
}

std::optional<RegionCoord> parse_region_file_name(std::string_view name) {
    if (!name.starts_with("r.") || !name.ends_with(region_extension)) {
        return std::nullopt;
    }
    std::string_view rest = name.substr(2, name.size() - 2 - region_extension.size());
    std::array<i32, 3> values{};
    for (usize i = 0; i < 3; ++i) {
        const usize dot = rest.find('.');
        const std::string_view part = i < 2 ? rest.substr(0, dot) : rest;
        if ((i < 2 && dot == std::string_view::npos) || part.empty()) {
            return std::nullopt;
        }
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), values[i]);
        if (ec != std::errc{} || end != part.data() + part.size()) {
            return std::nullopt;
        }
        rest = i < 2 ? rest.substr(dot + 1) : std::string_view{};
    }
    return RegionCoord{values[0], values[1], values[2]};
}

std::vector<std::pair<RegionCoord, std::vector<std::byte>>> encode_regions(const DeltaMap& deltas) {
    std::vector<ChunkCoord> coords;
    coords.reserve(deltas.size());
    for (const auto& [coord, delta] : deltas) {
        if (delta && !delta->empty() && coord.level == 0) {
            coords.push_back(coord);
        }
    }
    // Nach Region, dann nach Koordinate: dieselben Deltas ergeben dieselben Dateien.
    const auto region_less = [](RegionCoord a, RegionCoord b) {
        if (a.z != b.z) {
            return a.z < b.z;
        }
        if (a.y != b.y) {
            return a.y < b.y;
        }
        return a.x < b.x;
    };
    std::sort(coords.begin(), coords.end(), [&](ChunkCoord a, ChunkCoord b) {
        const RegionCoord ra = region_of(a);
        const RegionCoord rb = region_of(b);
        return ra != rb ? region_less(ra, rb) : a < b;
    });
    std::vector<std::pair<RegionCoord, std::vector<std::byte>>> regions;
    for (usize begin = 0; begin < coords.size();) {
        const RegionCoord region = region_of(coords[begin]);
        usize end = begin;
        while (end < coords.size() && region_of(coords[end]) == region) {
            ++end;
        }
        std::vector<std::byte> bytes{std::byte{'N'}, std::byte{'G'}, std::byte{'D'},
                                     std::byte{'R'}};
        write_u32(bytes, region_version);
        write_u32(bytes, static_cast<u32>(end - begin));
        for (usize i = begin; i < end; ++i) {
            const ChunkCoord c = coords[i];
            bytes.push_back(static_cast<std::byte>(c.x - region.x * region_chunks));
            bytes.push_back(static_cast<std::byte>(c.y - region.y * region_chunks));
            bytes.push_back(static_cast<std::byte>(c.z - region.z * region_chunks));
            const std::span<const std::byte> data = deltas.at(c)->bytes();
            write_u32(bytes, static_cast<u32>(data.size()));
            bytes.insert(bytes.end(), data.begin(), data.end());
        }
        regions.emplace_back(region, std::move(bytes));
        begin = end;
    }
    return regions;
}

Result<usize> decode_region(RegionCoord region, std::span<const std::byte> bytes, DeltaMap& out) {
    if (bytes.size() < 12 || bytes[0] != std::byte{'N'} || bytes[1] != std::byte{'G'} ||
        bytes[2] != std::byte{'D'} || bytes[3] != std::byte{'R'}) {
        return std::unexpected(std::string{"Keine Regionen-Datei mit Deltas"});
    }
    usize pos = 4;
    const std::optional<u32> version = read_u32(bytes, pos);
    if (!version || *version != region_version) {
        return std::unexpected(std::string{"Unbekannte Version der Regionen-Datei"});
    }
    const std::optional<u32> count = read_u32(bytes, pos);
    DeltaMap loaded;
    for (u32 i = 0; count && i < *count; ++i) {
        if (pos + 3 > bytes.size()) {
            return std::unexpected(std::string{"Regionen-Datei ist abgeschnitten"});
        }
        const auto lx = static_cast<i32>(bytes[pos]);
        const auto ly = static_cast<i32>(bytes[pos + 1]);
        const auto lz = static_cast<i32>(bytes[pos + 2]);
        pos += 3;
        const std::optional<u32> size = read_u32(bytes, pos);
        if (lx >= region_chunks || ly >= region_chunks || lz >= region_chunks || !size ||
            pos + *size > bytes.size()) {
            return std::unexpected(std::string{"Regionen-Datei ist beschädigt"});
        }
        auto delta = ChunkDelta::decode(bytes.subspan(pos, *size));
        if (!delta) {
            return std::unexpected(std::move(delta.error()));
        }
        pos += *size;
        if (!delta->empty()) {
            loaded.insert_or_assign(ChunkCoord{region.x * region_chunks + lx,
                                               region.y * region_chunks + ly,
                                               region.z * region_chunks + lz, 0},
                                    std::make_shared<const ChunkDelta>(*std::move(delta)));
        }
    }
    if (!count) {
        return std::unexpected(std::string{"Regionen-Datei ist abgeschnitten"});
    }
    const usize n = loaded.size();
    for (auto& entry : loaded) {
        out.insert_or_assign(entry.first, std::move(entry.second));
    }
    return n;
}

} // namespace ngine::voxel
