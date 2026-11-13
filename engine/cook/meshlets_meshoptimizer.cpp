module;

// Einzige Übersetzungseinheit, die meshoptimizer kennt (R3.5, ADR 0043).
#include <ngine/profile.hpp>

#include <cstddef>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <meshoptimizer.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

module ngine.cook;

import ngine.core;
import ngine.profile;
import ngine.asset;
import ngine.meshlet;

namespace ngine::cook {

void build_meshlets(asset::Primitive& primitive) {
    NGINE_PROFILE_ZONE("cook::build_meshlets");
    primitive.meshlets.clear();
    if (primitive.indices.empty()) {
        return;
    }
    const usize index_count = primitive.indices.size();
    const usize vertex_count = primitive.vertices.size();

    // Erst die Reihenfolge für den Cache der Eckpunkte (hilft dem Rückfallpfad mit Indexpuffer),
    // dann Meshlets mit Rücksicht auf ähnliche Normalen (Kegel).
    std::vector<u32> optimized(index_count);
    meshopt_optimizeVertexCache(optimized.data(), primitive.indices.data(), index_count,
                                vertex_count);

    const usize bound =
        meshopt_buildMeshletsBound(index_count, meshlet::max_vertices, meshlet::max_triangles);
    std::vector<meshopt_Meshlet> meshlets(bound);
    std::vector<unsigned int> meshlet_vertices(bound * meshlet::max_vertices);
    std::vector<unsigned char> meshlet_triangles(bound * meshlet::max_triangles * 3);
    const usize count = meshopt_buildMeshlets(
        meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(), optimized.data(),
        index_count, &primitive.vertices[0].position.x, vertex_count, sizeof(asset::Vertex),
        meshlet::max_vertices, meshlet::max_triangles, 0.25f);

    // In das Format von ngine.meshlet: Dreiecke jedes Meshlets hintereinander im Indexpuffer.
    std::vector<u32> sorted;
    sorted.reserve(index_count);
    primitive.meshlets.reserve(count);
    for (usize m = 0; m < count; ++m) {
        const meshopt_Meshlet& source = meshlets[m];
        meshlet::Meshlet target{.first_triangle = static_cast<u32>(sorted.size() / 3),
                                .triangle_count = source.triangle_count,
                                .vertex_count = source.vertex_count};
        for (usize i = 0; i < usize{source.triangle_count} * 3; ++i) {
            sorted.push_back(meshlet_vertices[source.vertex_offset +
                                              meshlet_triangles[source.triangle_offset + i]]);
        }
        primitive.meshlets.push_back(target);
    }
    primitive.indices = std::move(sorted);
    const meshlet::Positions positions =
        meshlet::positions_of(std::span<const asset::Vertex>{primitive.vertices});
    for (meshlet::Meshlet& target : primitive.meshlets) {
        meshlet::compute_bounds(positions, primitive.indices, target);
    }
    // meshoptimizer zählt Eckpunkte wie ngine.meshlet; sonst lieber das eigene Verfahren.
    if (!meshlet::valid(primitive.meshlets, primitive.indices)) {
        primitive.meshlets = meshlet::build(positions, primitive.indices);
    }
}

} // namespace ngine::cook
