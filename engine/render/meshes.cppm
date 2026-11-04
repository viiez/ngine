module;

#include <memory>
#include <span>
#include <vector>

export module ngine.render:meshes;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;

export namespace ngine::render {

// Ein Teilnetz zum Hochladen: Eckpunkte, Indizes relativ zu diesen Eckpunkten, Material aus dem
// MaterialStore.
struct SubmeshData {
    std::span<const asset::Vertex> vertices;
    std::span<const u32> indices;
    u32 material = 0;
};

struct Submesh {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
};

// Mesh auf der GPU: alle Teilnetze in einem Eckpunkt- und einem Indexpuffer. Der Shader liest die
// Eckpunkte über die Adresse (Vertex Pulling, ADR 0022); die Indizes sind schon um den Anfang des
// Teilnetzes verschoben.
struct GpuMesh {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    std::vector<Submesh> submeshes;
    // Achsenparallele Hülle in Objektkoordinaten.
    math::Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    math::Vec3 bounds_max{0.0f, 0.0f, 0.0f};
};

// Besitzt die Geometrie aller Meshes (REND-07). MeshRenderer verweisen über den Index darauf.
class MeshStore {
public:
    explicit MeshStore(rhi::Device& device) : device_{&device} {}

    // Lädt ein Mesh hoch (über Device::upload(), also vor der nächsten Abgabe) und liefert seinen
    // Index. Ein Mesh ohne Dreiecke ist erlaubt und wird nie gezeichnet.
    [[nodiscard]] rhi::Result<u32> add(std::span<const SubmeshData> submeshes);
    [[nodiscard]] const GpuMesh* get(u32 index) const {
        return index < meshes_.size() ? &meshes_[index] : nullptr;
    }
    [[nodiscard]] usize size() const noexcept {
        return meshes_.size();
    }

private:
    rhi::Device* device_;
    std::vector<GpuMesh> meshes_;
};

} // namespace ngine::render
