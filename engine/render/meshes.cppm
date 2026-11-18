module;

#include <optional>

#include <memory>
#include <span>
#include <vector>

export module ngine.render:meshes;

import ngine.core;
import ngine.math;
import ngine.rhi;
import ngine.asset;
import ngine.meshlet;
import :geometry;

export namespace ngine::render {

// Ein Teilnetz zum Hochladen: Eckpunkte, Indizes relativ zu diesen Eckpunkten, Material aus dem
// MaterialStore. Seit R3.5 mit Meshlets (ngine.meshlet), zu denen die Indizes umsortiert sind;
// fehlen sie oder passen nicht, baut add() sie auf einer Kopie der Indizes.
struct SubmeshData {
    std::span<const asset::Vertex> vertices;
    std::span<const u32> indices;
    u32 material = 0;
    std::span<const meshlet::Meshlet> meshlets;
    // Gelenke und Gewichte je Eckpunkt (R4.6); leer bei starren Netzen. Alle Teilnetze eines
    // verformten Netzes haben sie.
    std::span<const asset::SkinVertex> skin;
};

struct Submesh {
    // Erster Index im Indexpuffer des Blocks (schon um den Anfang des Bereichs verschoben).
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    // Meshlets des Teilnetzes in den Meshlet-Daten des Netzes.
    u32 first_meshlet = 0;
    u32 meshlet_count = 0;
};

// Mesh auf der GPU: alle Teilnetze als je ein Bereich für Eckpunkte und Indizes in den
// gemeinsamen Geometriepuffern (TD-23). Der Shader liest die Eckpunkte über die Adresse
// (Vertex Pulling, ADR 0022); die Indizes zählen ab dieser Adresse und sind schon um den Anfang
// des Teilnetzes verschoben.
struct GpuMesh {
    GeometryRange vertices;
    GeometryRange indices;
    // Meshlets aller Teilnetze mit ihren lokalen Daten (pack_meshlets(), R3.5).
    GeometryRange meshlet_data;
    // Adresse des ersten Eckpunkts und Puffer der Indizes; 0 bzw. nullptr ohne Dreiecke.
    u64 vertex_address = 0;
    const rhi::Buffer* index_buffer = nullptr;
    u64 meshlet_address = 0;
    // Verformte Netze (R4.6): Strom der Gewichte parallel zu den Eckpunkten, Skin im Modell und
    // Zahl seiner Gelenke. skin_address ist 0 bei starren Netzen.
    GeometryRange skin;
    u64 skin_address = 0;
    std::optional<u32> skin_index;
    u32 joint_count = 0;
    u32 vertex_count = 0;
    std::vector<Submesh> submeshes;
    // Achsenparallele Hülle in Objektkoordinaten; bei verformten Netzen über alle Hüllkugeln der
    // Meshlets, die beim Kochen jede Pose der Clips umschließen.
    math::Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    math::Vec3 bounds_max{0.0f, 0.0f, 0.0f};
};

// Besitzt die Geometrie aller Meshes (REND-07). MeshRenderer verweisen über den Index darauf.
class MeshStore {
public:
    explicit MeshStore(GeometryBuffer& geometry) : geometry_{&geometry} {}

    // Lädt ein Mesh hoch (über Device::upload(), also vor der nächsten Abgabe) und liefert seinen
    // Index. Ein Mesh ohne Dreiecke ist erlaubt und wird nie gezeichnet. Mit ticket laufen die
    // Kopien im Hintergrund (Device::upload_async(), TD-17); gezeichnet werden darf das Mesh erst,
    // wenn das Ticket erledigt ist.
    // skin: Skin des Netzes im Modell (R4.6), mit joint_count Gelenken; nur wenn alle Teilnetze
    // Gewichte haben, wird das Netz verformbar.
    [[nodiscard]] rhi::Result<u32> add(std::span<const SubmeshData> submeshes,
                                       rhi::UploadTicket* ticket = nullptr,
                                       std::optional<u32> skin = std::nullopt,
                                       u32 joint_count = 0);
    // Gibt ein Mesh frei (seine Bereiche, sobald die Frames im Flug fertig sind); add() vergibt
    // den Index neu.
    void remove(u32 index);
    [[nodiscard]] const GpuMesh* get(u32 index) const {
        return index < meshes_.size() ? &meshes_[index] : nullptr;
    }
    [[nodiscard]] usize size() const noexcept {
        return meshes_.size();
    }
    // Belegte Meshes (ohne freigegebene).
    [[nodiscard]] usize count() const noexcept {
        return meshes_.size() - free_.size();
    }

private:
    GeometryBuffer* geometry_;
    std::vector<GpuMesh> meshes_;
    std::vector<u32> free_;
};

} // namespace ngine::render
