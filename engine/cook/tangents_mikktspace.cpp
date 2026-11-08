module;

// Einzige Übersetzungseinheit, die MikkTSpace kennt (TD-21, ADR 0036).
#include <ngine/profile.hpp>

#include <bit>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <mikktspace.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

module ngine.cook;

import ngine.core;
import ngine.math;
import ngine.profile;
import ngine.asset;

namespace ngine::cook {

namespace {

// MikkTSpace arbeitet auf Ecken (Dreieck, Ecke) und liefert für jede eine Tangente.
struct Corners {
    const asset::Primitive* primitive = nullptr;
    std::vector<math::Vec4> tangents;

    [[nodiscard]] const asset::Vertex& vertex(int face, int corner) const {
        return primitive->vertices[primitive->indices[static_cast<usize>(face) * 3 +
                                                      static_cast<usize>(corner)]];
    }
};

Corners& corners_of(const SMikkTSpaceContext* context) {
    return *static_cast<Corners*>(context->m_pUserData);
}

int face_count(const SMikkTSpaceContext* context) {
    return static_cast<int>(corners_of(context).primitive->indices.size() / 3);
}

int vertices_of_face(const SMikkTSpaceContext* /*context*/, int /*face*/) {
    return 3;
}

void position(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    const math::Vec3 p = corners_of(context).vertex(face, corner).position;
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void normal(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    const math::Vec3 n = corners_of(context).vertex(face, corner).normal;
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void texcoord(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    const math::Vec2 uv = corners_of(context).vertex(face, corner).uv;
    out[0] = uv.x;
    out[1] = uv.y;
}

void set_tangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face,
                 int corner) {
    // Gleiche Konvention wie glTF: Bitangente = sign * cross(Normale, Tangente).
    corners_of(context).tangents[static_cast<usize>(face) * 3 + static_cast<usize>(corner)] = {
        tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
}

// Schlüssel für das Zusammenführen: alter Eckpunkt und die Bits der Tangente.
struct Key {
    u32 vertex = 0;
    u32 bits[4] = {};
    friend bool operator==(const Key&, const Key&) = default;
};

struct KeyHash {
    usize operator()(const Key& key) const noexcept {
        u64 hash = 14695981039346656037ull ^ key.vertex;
        for (const u32 b : key.bits) {
            hash = (hash ^ b) * 1099511628211ull;
        }
        return static_cast<usize>(hash);
    }
};

} // namespace

void generate_tangents(asset::Primitive& primitive) {
    NGINE_PROFILE_ZONE("cook::generate_tangents");
    if (primitive.indices.size() < 3 || primitive.indices.size() % 3 != 0) {
        return;
    }
    Corners corners{.primitive = &primitive, .tangents = {}};
    corners.tangents.assign(primitive.indices.size(), math::Vec4{1.0f, 0.0f, 0.0f, 1.0f});
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = face_count;
    callbacks.m_getNumVerticesOfFace = vertices_of_face;
    callbacks.m_getPosition = position;
    callbacks.m_getNormal = normal;
    callbacks.m_getTexCoord = texcoord;
    callbacks.m_setTSpaceBasic = set_tangent;
    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &corners;
    if (genTangSpaceDefault(&context) == 0) {
        log::warn("MikkTSpace: Tangenten ließen sich nicht berechnen, die alten bleiben");
        return;
    }

    // Ecken mit gleichem Eckpunkt und gleicher Tangente wieder zusammenführen.
    std::vector<asset::Vertex> vertices;
    vertices.reserve(primitive.vertices.size());
    std::vector<u32> indices(primitive.indices.size());
    std::unordered_map<Key, u32, KeyHash> merged;
    merged.reserve(primitive.indices.size());
    for (usize corner = 0; corner < primitive.indices.size(); ++corner) {
        const u32 source = primitive.indices[corner];
        const math::Vec4 tangent = corners.tangents[corner];
        Key key{.vertex = source, .bits = {}};
        key.bits[0] = std::bit_cast<u32>(tangent.x);
        key.bits[1] = std::bit_cast<u32>(tangent.y);
        key.bits[2] = std::bit_cast<u32>(tangent.z);
        key.bits[3] = std::bit_cast<u32>(tangent.w);
        const auto [it, inserted] = merged.try_emplace(key, static_cast<u32>(vertices.size()));
        if (inserted) {
            asset::Vertex vertex = primitive.vertices[source];
            vertex.tangent = tangent;
            vertices.push_back(vertex);
        }
        indices[corner] = it->second;
    }
    primitive.vertices = std::move(vertices);
    primitive.indices = std::move(indices);
    primitive.generated_tangents = false;
}

} // namespace ngine::cook
