module;

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

module ngine.voxel;

import ngine.core;
import ngine.math;

namespace ngine::voxel {

// --- Chunk ----------------------------------------------------------------------------------

void Chunk::set(u32 x, u32 y, u32 z, Sample sample) {
    if (samples_.empty()) {
        if (sample == uniform_) {
            return;
        }
        samples_.assign(chunk_sample_count, uniform_);
    }
    samples_[index(x, y, z)] = sample;
}

void Chunk::fill(Sample sample) noexcept {
    uniform_ = sample;
    samples_.clear();
    samples_.shrink_to_fit();
}

std::span<Sample> Chunk::expand() {
    if (samples_.empty()) {
        samples_.assign(chunk_sample_count, uniform_);
    }
    return samples_;
}

bool Chunk::compact() {
    if (samples_.empty()) {
        return true;
    }
    const Sample first = samples_.front();
    if (!std::ranges::all_of(samples_, [first](Sample s) { return s == first; })) {
        return false;
    }
    fill(first);
    return true;
}

// --- ChunkStore -----------------------------------------------------------------------------

const Chunk* ChunkStore::find(ChunkCoord coord) const noexcept {
    const auto it = chunks_.find(coord);
    return it != chunks_.end() ? it->second.get() : nullptr;
}

Chunk* ChunkStore::find(ChunkCoord coord) noexcept {
    const auto it = chunks_.find(coord);
    return it != chunks_.end() ? it->second.get() : nullptr;
}

Chunk& ChunkStore::get_or_create(ChunkCoord coord) {
    auto& slot = chunks_[coord];
    if (!slot) {
        slot = std::make_unique<Chunk>(outside_);
    }
    return *slot;
}

void ChunkStore::remove(ChunkCoord coord) {
    chunks_.erase(coord);
}

void ChunkStore::clear() {
    chunks_.clear();
    dirty_.clear();
    ++version_;
}

std::vector<ChunkCoord> ChunkStore::coords() const {
    std::vector<ChunkCoord> result;
    result.reserve(chunks_.size());
    for (const auto& entry : chunks_) {
        result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

usize ChunkStore::memory_bytes() const noexcept {
    usize bytes = 0;
    for (const auto& entry : chunks_) {
        bytes += entry.second->memory_bytes();
    }
    return bytes;
}

usize ChunkStore::uniform_count() const noexcept {
    usize count = 0;
    for (const auto& entry : chunks_) {
        count += entry.second->uniform() ? 1 : 0;
    }
    return count;
}

Sample ChunkStore::sample(i64 x, i64 y, i64 z) const noexcept {
    const ChunkCoord coord = chunk_of(x, y, z);
    const Chunk* chunk = find(coord);
    if (chunk == nullptr) {
        return outside_;
    }
    return chunk->sample(static_cast<u32>(x - chunk_origin(coord.x)),
                         static_cast<u32>(y - chunk_origin(coord.y)),
                         static_cast<u32>(z - chunk_origin(coord.z)));
}

bool ChunkStore::set(i64 x, i64 y, i64 z, Sample sample) {
    const ChunkCoord coord = chunk_of(x, y, z);
    const auto lx = static_cast<u32>(x - chunk_origin(coord.x));
    const auto ly = static_cast<u32>(y - chunk_origin(coord.y));
    const auto lz = static_cast<u32>(z - chunk_origin(coord.z));
    Chunk* chunk = find(coord);
    if (chunk == nullptr) {
        if (sample == outside_) {
            return false;
        }
        chunk = &get_or_create(coord);
    }
    if (chunk->sample(lx, ly, lz) == sample) {
        return false;
    }
    chunk->set(lx, ly, lz, sample);
    return true;
}

f32 ChunkStore::distance_at(math::Vec3 p) const noexcept {
    const f32 fx = std::floor(p.x);
    const f32 fy = std::floor(p.y);
    const f32 fz = std::floor(p.z);
    const auto x = static_cast<i64>(fx);
    const auto y = static_cast<i64>(fy);
    const auto z = static_cast<i64>(fz);
    const f32 tx = p.x - fx;
    const f32 ty = p.y - fy;
    const f32 tz = p.z - fz;
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    const f32 c00 = lerp(distance(x, y, z), distance(x + 1, y, z), tx);
    const f32 c10 = lerp(distance(x, y + 1, z), distance(x + 1, y + 1, z), tx);
    const f32 c01 = lerp(distance(x, y, z + 1), distance(x + 1, y, z + 1), tx);
    const f32 c11 = lerp(distance(x, y + 1, z + 1), distance(x + 1, y + 1, z + 1), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
}

void ChunkStore::mark_changed(const SampleBox& box) {
    if (box.empty()) {
        return;
    }
    // Chunk c liest die Samples [32c - 2, 32c + 34); ein Sample g betrifft also die Chunks von
    // floor((g - 2) / 32) bis floor((g + 2) / 32).
    auto first = [](i64 min) { return floor_div(min - border_after, chunk_voxels); };
    auto last = [](i64 max) { return floor_div(max - 1 + border_before, chunk_voxels); };
    for (i64 z = first(box.min_z); z <= last(box.max_z); ++z) {
        for (i64 y = first(box.min_y); y <= last(box.max_y); ++y) {
            for (i64 x = first(box.min_x); x <= last(box.max_x); ++x) {
                dirty_.insert({static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(z), 0});
            }
        }
    }
    ++version_;
}

void ChunkStore::mark_changed(ChunkCoord coord) {
    const i64 x = chunk_origin(coord.x);
    const i64 y = chunk_origin(coord.y);
    const i64 z = chunk_origin(coord.z);
    mark_changed(SampleBox{x, y, z, x + chunk_voxels, y + chunk_voxels, z + chunk_voxels});
}

std::vector<ChunkCoord> ChunkStore::take_changes() {
    std::vector<ChunkCoord> result{dirty_.begin(), dirty_.end()};
    dirty_.clear();
    std::sort(result.begin(), result.end());
    return result;
}

// --- Randschicht ----------------------------------------------------------------------------

namespace {

// Teil einer Achse des gesammelten Bereichs, der aus einem Nachbarn kommt: d = -1, 0 oder 1.
struct Span {
    u32 out_begin; // Index im gesammelten Gitter
    u32 source;    // erstes Sample im Nachbarn
    u32 count;
};

Span axis_span(i32 d) noexcept {
    switch (d) {
    case -1: return {0, chunk_voxels - border_before, border_before};
    case 0: return {border_before, 0, chunk_voxels};
    default: return {border_before + chunk_voxels, 0, border_after};
    }
}

} // namespace

void gather(const ChunkStore& store, ChunkCoord coord, Grid& out) {
    verify(out.size() == Extent3{padded_samples, padded_samples, padded_samples},
           "voxel::gather: Gitter muss padded_samples³ groß sein");
    verify(coord.level == 0, "voxel::gather: nur Stufe 0");
    std::span<Sample> target = out.samples();
    for (i32 dz = -1; dz <= 1; ++dz) {
        const Span sz = axis_span(dz);
        for (i32 dy = -1; dy <= 1; ++dy) {
            const Span sy = axis_span(dy);
            for (i32 dx = -1; dx <= 1; ++dx) {
                const Span sx = axis_span(dx);
                const Chunk* chunk =
                    store.find({coord.x + dx, coord.y + dy, coord.z + dz, coord.level});
                const bool uniform = chunk == nullptr || chunk->uniform();
                const Sample value = chunk == nullptr ? store.outside() : chunk->uniform_value();
                const std::span<const Sample> source =
                    uniform ? std::span<const Sample>{} : chunk->samples();
                for (u32 z = 0; z < sz.count; ++z) {
                    for (u32 y = 0; y < sy.count; ++y) {
                        Sample* row = target.data() +
                                      out.index(sx.out_begin, sy.out_begin + y, sz.out_begin + z);
                        if (uniform) {
                            std::fill_n(row, sx.count, value);
                        } else {
                            const Sample* from =
                                source.data() +
                                Chunk::index(sx.source, sy.source + y, sz.source + z);
                            std::copy_n(from, sx.count, row);
                        }
                    }
                }
            }
        }
    }
    out.touch();
}

bool trivially_empty(const ChunkStore& store, ChunkCoord coord) {
    int sign = 0;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const Chunk* chunk =
                    store.find({coord.x + dx, coord.y + dy, coord.z + dz, coord.level});
                Sample value = store.outside();
                if (chunk != nullptr) {
                    if (!chunk->uniform()) {
                        return false;
                    }
                    value = chunk->uniform_value();
                }
                const int s = value.density > 0 ? 1 : -1;
                if (sign != 0 && s != sign) {
                    return false;
                }
                sign = s;
            }
        }
    }
    return true;
}

} // namespace ngine::voxel
