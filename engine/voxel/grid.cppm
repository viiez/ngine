module;

#include <functional>
#include <span>
#include <vector>

export module ngine.voxel:grid;

import ngine.core;
import ngine.math;

export namespace ngine::voxel {

// Positionen im Gitter in Voxeln, Ursprung in der Ecke des Gitters.
using math::Vec3;

// Anzahl der Samples pro Achse.
struct Extent3 {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;

    [[nodiscard]] constexpr usize count() const noexcept {
        return usize{x} * y * z;
    }
    friend constexpr bool operator==(Extent3, Extent3) noexcept = default;
};

// Material 0 heißt Luft; alles andere ist fester Stoff mit einer Nummer aus der Materialtabelle.
using Material = u8;
inline constexpr Material air = 0;

// Ein Sample des Dichtefelds: vorzeichenbehafteter Abstand zur Oberfläche, positiv im Inneren,
// gespeichert in 8 Bit (ADR 0021). Ein Schritt entspricht 1/density_scale Voxel, der Wertebereich
// also rund ±4 Voxel. Weiter weg zählt nur das Vorzeichen.
struct Sample {
    i8 density = -127;
    Material material = air;

    friend constexpr bool operator==(Sample, Sample) noexcept = default;
};

inline constexpr f32 density_scale = 32.0f;
inline constexpr f32 max_distance = 127.0f / density_scale;

[[nodiscard]] i8 quantize(f32 distance) noexcept;
[[nodiscard]] constexpr f32 dequantize(i8 density) noexcept {
    return static_cast<f32>(density) / density_scale;
}

// Achsenparalleler Bereich von Samples, min inklusive, max exklusive.
struct Box {
    u32 min_x = 0, min_y = 0, min_z = 0;
    u32 max_x = 0, max_y = 0, max_z = 0;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return min_x >= max_x || min_y >= max_y || min_z >= max_z;
    }
};

// Dichtefeld mit Material pro Sample. Koordinaten sind Sample-Indizes; ein Voxel ist der Würfel
// zwischen acht benachbarten Samples. Ein Chunk mit 32 Voxeln pro Achse hat also 33 Samples.
class Grid {
public:
    explicit Grid(Extent3 size);

    [[nodiscard]] Extent3 size() const noexcept {
        return size_;
    }
    [[nodiscard]] Box bounds() const noexcept {
        return {0, 0, 0, size_.x, size_.y, size_.z};
    }

    [[nodiscard]] usize index(u32 x, u32 y, u32 z) const noexcept {
        return (usize{z} * size_.y + y) * size_.x + x;
    }
    [[nodiscard]] Sample sample(u32 x, u32 y, u32 z) const noexcept {
        return samples_[index(x, y, z)];
    }
    void set(u32 x, u32 y, u32 z, Sample sample) noexcept {
        samples_[index(x, y, z)] = sample;
    }
    // Abstand in Voxeln, positiv im Inneren.
    [[nodiscard]] f32 distance(u32 x, u32 y, u32 z) const noexcept {
        return dequantize(samples_[index(x, y, z)].density);
    }
    // Trilinear interpoliert; Punkte außerhalb werden an den Rand geklemmt.
    [[nodiscard]] f32 distance_at(Vec3 p) const noexcept;
    // Gradient der Dichte (zeigt nach innen), aus zentralen Differenzen, trilinear interpoliert.
    [[nodiscard]] Vec3 gradient_at(Vec3 p) const noexcept;

    [[nodiscard]] std::span<const Sample> samples() const noexcept {
        return samples_;
    }
    [[nodiscard]] std::span<Sample> samples() noexcept {
        return samples_;
    }

    // Erhöht sich bei jeder Änderung über fill() oder die Pinsel; so erkennt der Aufrufer, ob ein
    // neues Netz nötig ist.
    [[nodiscard]] u64 version() const noexcept {
        return version_;
    }
    void touch() noexcept {
        ++version_;
    }

private:
    Extent3 size_;
    std::vector<Sample> samples_;
    u64 version_ = 0;
};

// Füllt das Feld aus einer Abstandsfunktion (in Voxeln, positiv im Inneren). Das Material liefert
// eine zweite Funktion und gilt nur für Samples im Inneren.
using DistanceFunction = std::function<f32(Vec3)>;
using MaterialFunction = std::function<Material(Vec3)>;
void fill(Grid& grid, const DistanceFunction& distance, const MaterialFunction& material);

// Ein glatter Hügel auf einer ebenen Grundfläche, für den Durchstich D1. Höhe und Radius in Voxeln.
struct HillDesc {
    f32 ground = 6.0f;
    f32 height = 14.0f;
    f32 radius = 9.0f;
};
void fill_hill(Grid& grid, const HillDesc& desc = {});

} // namespace ngine::voxel
