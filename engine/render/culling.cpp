module;

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

module ngine.render;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.meshlet;

namespace ngine::render {

std::array<math::Vec4, 6> frustum_planes(const math::Mat4& view_projection) {
    // Gribb und Hartmann: Zeilen der Matrix kombiniert.
    const math::Mat4 rows = math::transpose(view_projection);
    const math::Vec4 r0 = rows[0];
    const math::Vec4 r1 = rows[1];
    const math::Vec4 r2 = rows[2];
    const math::Vec4 r3 = rows[3];
    std::array<math::Vec4, 6> planes{r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
    for (math::Vec4& plane : planes) {
        const f32 length = math::length(math::xyz(plane));
        if (length < 1e-12f) {
            plane = {0.0f, 0.0f, 0.0f, 1.0f};
        } else {
            plane = plane * (1.0f / length);
        }
    }
    return planes;
}

std::vector<u32> pack_meshlets(std::span<const MeshletPart> parts) {
    usize total = 0;
    for (const MeshletPart& part : parts) {
        total += part.meshlets.size();
    }
    constexpr usize record_words = sizeof(GpuMeshlet) / sizeof(u32);
    std::vector<u32> words(total * record_words);
    usize record = 0;
    for (const MeshletPart& part : parts) {
        meshlet::LocalData computed;
        if (part.local == nullptr) {
            computed = meshlet::local_data(part.meshlets, part.indices);
        }
        const meshlet::LocalData& local = part.local != nullptr ? *part.local : computed;
        const auto vertex_start = static_cast<u32>(words.size());
        words.insert(words.end(), local.vertices.begin(), local.vertices.end());
        const auto triangle_start = static_cast<u32>(words.size());
        words.insert(words.end(), local.triangles.begin(), local.triangles.end());
        for (usize i = 0; i < part.meshlets.size(); ++i) {
            const meshlet::Meshlet& m = part.meshlets[i];
            const GpuMeshlet gpu{
                .sphere = math::extend(m.center, m.radius),
                .cone = math::extend(m.cone_axis, m.cone_cutoff),
                .vertex_offset = vertex_start + local.vertex_offsets[i],
                .triangle_offset = triangle_start + m.first_triangle,
                .first_triangle = m.first_triangle,
                .counts = (m.vertex_count & 0xFFu) | ((m.triangle_count & 0xFFu) << 8),
            };
            std::memcpy(words.data() + record * record_words, &gpu, sizeof(gpu));
            ++record;
        }
    }
    return words;
}

rhi::Extent2D hiz_extent(rhi::Extent2D depth) noexcept {
    return {std::bit_floor(std::max(depth.width, 1u)), std::bit_floor(std::max(depth.height, 1u))};
}

u32 hiz_level_count(rhi::Extent2D hiz) noexcept {
    const u32 largest = std::max({hiz.width, hiz.height, 1u});
    return std::min(static_cast<u32>(std::bit_width(largest)), max_hiz_levels);
}

} // namespace ngine::render
