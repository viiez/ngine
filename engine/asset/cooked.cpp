module;

#include <ngine/profile.hpp>

#include <bit>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module ngine.asset;

import ngine.core;
import ngine.math;
import ngine.filesystem;
import ngine.profile;
import ngine.serialize;

namespace ngine::asset {

namespace {

using serialize::BinaryReader;
using serialize::BinaryWriter;

// Eckpunkte gehen als rohe Bytes in die Datei; das Format ist Little Endian wie jede Zielplattform.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(Vertex) == 48 && std::is_trivially_copyable_v<Vertex>);

constexpr u32 cooked_magic = 0x444D474Eu; // „NGMD“

// --- Schreiben ---

void write_vec(BinaryWriter& w, std::span<const f32> values) {
    for (const f32 v : values) {
        w.write(v);
    }
}

void write_optional(BinaryWriter& w, const std::optional<u32>& value) {
    w.write(value.has_value());
    w.write(value.value_or(0));
}

template <typename T>
void write_raw(BinaryWriter& w, const std::vector<T>& values) {
    w.write(static_cast<u64>(values.size()));
    w.write_bytes(std::as_bytes(std::span{values}));
}

void write_material(BinaryWriter& w, const Material& m) {
    w.write_string(m.name);
    write_vec(w, {&m.base_color.x, 4});
    w.write(m.metallic);
    w.write(m.roughness);
    write_vec(w, {&m.emissive.x, 3});
    w.write(m.normal_scale);
    w.write(m.occlusion_strength);
    for (const auto* texture : {&m.base_color_texture, &m.metallic_roughness_texture,
                                &m.normal_texture, &m.occlusion_texture, &m.emissive_texture}) {
        write_optional(w, *texture);
    }
    w.write(static_cast<u8>(m.alpha_mode));
    w.write(m.alpha_cutoff);
    w.write(m.double_sided);
}

void write_model(BinaryWriter& w, const Model& model) {
    w.write(static_cast<u32>(model.meshes.size()));
    for (const Mesh& mesh : model.meshes) {
        w.write_string(mesh.name);
        w.write(static_cast<u32>(mesh.primitives.size()));
        for (const Primitive& primitive : mesh.primitives) {
            write_optional(w, primitive.material);
            w.write(primitive.generated_tangents);
            write_raw(w, primitive.vertices);
            write_raw(w, primitive.indices);
        }
    }
    w.write(static_cast<u32>(model.materials.size()));
    for (const Material& material : model.materials) {
        write_material(w, material);
    }
    w.write(static_cast<u32>(model.textures.size()));
    for (const Texture& texture : model.textures) {
        const SamplerDesc& s = texture.sampler;
        w.write(texture.image);
        w.write(static_cast<u8>(s.mag));
        w.write(static_cast<u8>(s.min));
        w.write(static_cast<u8>(s.mip));
        w.write(s.mipmaps);
        w.write(static_cast<u8>(s.wrap_u));
        w.write(static_cast<u8>(s.wrap_v));
    }
    w.write(static_cast<u32>(model.images.size()));
    for (const Image& image : model.images) {
        w.write_string(image.name);
        w.write(image.width);
        w.write(image.height);
    }
    w.write(static_cast<u32>(model.nodes.size()));
    for (const Node& node : model.nodes) {
        w.write_string(node.name);
        write_vec(w, {&node.translation.x, 3});
        write_vec(w, {&node.rotation.x, 4});
        write_vec(w, {&node.scale.x, 3});
        write_optional(w, node.mesh);
        write_optional(w, node.parent);
        write_raw(w, node.children);
    }
    write_raw(w, model.roots);
}

// --- Lesen ---

// Liest der Reihe nach und merkt sich den ersten Fehler; danach liefert alles nur noch Nullen.
class Input {
public:
    explicit Input(std::span<const std::byte> data) : reader_{data} {}

    template <typename T>
    T get() {
        T value{};
        if (ok_ && !reader_.read(value)) {
            ok_ = false;
        }
        return value;
    }
    std::string text() {
        std::string value;
        if (ok_ && !reader_.read_string(value)) {
            ok_ = false;
        }
        return value;
    }
    void floats(f32* target, usize count) {
        for (usize i = 0; i < count; ++i) {
            target[i] = get<f32>();
        }
    }
    std::optional<u32> optional() {
        const bool present = get<bool>();
        const u32 value = get<u32>();
        return present ? std::optional<u32>{value} : std::nullopt;
    }
    template <typename T>
    std::vector<T> raw() {
        const u64 count = get<u64>();
        std::vector<T> values;
        if (!ok_ || count > reader_.remaining() / sizeof(T)) {
            ok_ = false;
            return values;
        }
        values.resize(count);
        if (!reader_.read_bytes(std::as_writable_bytes(std::span{values}))) {
            ok_ = false;
        }
        return values;
    }
    std::vector<std::byte> blob() {
        return raw<std::byte>();
    }
    // Anzahl von Elementen, mindestens min_bytes groß; schützt vor riesigen Reservierungen.
    u32 count(usize min_bytes) {
        const u32 value = get<u32>();
        if (ok_ && u64{value} * min_bytes > reader_.remaining()) {
            ok_ = false;
        }
        return ok_ ? value : 0;
    }
    template <typename E>
    E enumeration(u8 last) {
        const u8 value = get<u8>();
        if (value > last) {
            ok_ = false;
            return E{};
        }
        return static_cast<E>(value);
    }
    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }
    [[nodiscard]] usize remaining() const noexcept {
        return reader_.remaining();
    }

private:
    BinaryReader reader_;
    bool ok_ = true;
};

Material read_material(Input& in) {
    Material m;
    m.name = in.text();
    in.floats(&m.base_color.x, 4);
    m.metallic = in.get<f32>();
    m.roughness = in.get<f32>();
    in.floats(&m.emissive.x, 3);
    m.normal_scale = in.get<f32>();
    m.occlusion_strength = in.get<f32>();
    for (auto* texture : {&m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture,
                          &m.occlusion_texture, &m.emissive_texture}) {
        *texture = in.optional();
    }
    m.alpha_mode = in.enumeration<AlphaMode>(static_cast<u8>(AlphaMode::Blend));
    m.alpha_cutoff = in.get<f32>();
    m.double_sided = in.get<bool>();
    return m;
}

Model read_model(Input& in) {
    Model model;
    model.meshes.resize(in.count(8));
    for (Mesh& mesh : model.meshes) {
        mesh.name = in.text();
        mesh.primitives.resize(in.count(16));
        for (Primitive& primitive : mesh.primitives) {
            primitive.material = in.optional();
            primitive.generated_tangents = in.get<bool>();
            primitive.vertices = in.raw<Vertex>();
            primitive.indices = in.raw<u32>();
        }
    }
    model.materials.resize(in.count(16));
    for (Material& material : model.materials) {
        material = read_material(in);
    }
    model.textures.resize(in.count(10));
    for (Texture& texture : model.textures) {
        texture.image = in.get<u32>();
        texture.sampler.mag = in.enumeration<Filter>(static_cast<u8>(Filter::Linear));
        texture.sampler.min = in.enumeration<Filter>(static_cast<u8>(Filter::Linear));
        texture.sampler.mip = in.enumeration<Filter>(static_cast<u8>(Filter::Linear));
        texture.sampler.mipmaps = in.get<bool>();
        texture.sampler.wrap_u = in.enumeration<Wrap>(static_cast<u8>(Wrap::ClampToEdge));
        texture.sampler.wrap_v = in.enumeration<Wrap>(static_cast<u8>(Wrap::ClampToEdge));
    }
    model.images.resize(in.count(12));
    for (Image& image : model.images) {
        image.name = in.text();
        image.width = in.get<u32>();
        image.height = in.get<u32>();
    }
    model.nodes.resize(in.count(48));
    for (Node& node : model.nodes) {
        node.name = in.text();
        in.floats(&node.translation.x, 3);
        in.floats(&node.rotation.x, 4);
        in.floats(&node.scale.x, 3);
        node.mesh = in.optional();
        node.parent = in.optional();
        node.children = in.raw<u32>();
    }
    model.roots = in.raw<u32>();
    return model;
}

// Prüft Verweise zwischen den Teilen, damit ein kaputter Cache nie außerhalb der Felder liest.
Result<void> validate(const CookedModel& cooked) {
    const Model& model = cooked.model;
    for (const Mesh& mesh : model.meshes) {
        for (const Primitive& primitive : mesh.primitives) {
            if (primitive.material && *primitive.material >= model.materials.size()) {
                return std::unexpected(std::string{"Material eines Teilnetzes fehlt"});
            }
            for (const u32 index : primitive.indices) {
                if (index >= primitive.vertices.size()) {
                    return std::unexpected(std::string{"Index außerhalb der Eckpunkte"});
                }
            }
        }
    }
    for (const Material& material : model.materials) {
        for (const auto* texture :
             {&material.base_color_texture, &material.metallic_roughness_texture,
              &material.normal_texture, &material.occlusion_texture, &material.emissive_texture}) {
            if (*texture && **texture >= model.textures.size()) {
                return std::unexpected(std::string{"Textur eines Materials fehlt"});
            }
        }
    }
    for (const Texture& texture : model.textures) {
        if (texture.image >= model.images.size()) {
            return std::unexpected(std::string{"Bild einer Textur fehlt"});
        }
    }
    for (const Node& node : model.nodes) {
        if ((node.mesh && *node.mesh >= model.meshes.size()) ||
            (node.parent && *node.parent >= model.nodes.size())) {
            return std::unexpected(std::string{"Knoten verweist ins Leere"});
        }
        for (const u32 child : node.children) {
            if (child >= model.nodes.size()) {
                return std::unexpected(std::string{"Kindknoten fehlt"});
            }
        }
    }
    for (const u32 root : model.roots) {
        if (root >= model.nodes.size()) {
            return std::unexpected(std::string{"Wurzelknoten fehlt"});
        }
    }
    for (const CookedTexture& texture : cooked.textures) {
        if (texture.image >= model.images.size()) {
            return std::unexpected(std::string{"Gekochte Textur ohne Bild"});
        }
        const TextureData& bc7 = texture.bc7;
        if (!bc7.data.empty() &&
            (bc7.width == 0 || bc7.height == 0 || bc7.levels == 0 || bc7.levels > 32 ||
             bc7.data.size() !=
                 texture_size(TextureFormat::BC7, bc7.width, bc7.height, bc7.levels))) {
            return std::unexpected(std::string{"BC7-Fassung einer Textur hat die falsche Größe"});
        }
    }
    return {};
}

std::string hex(AssetId id) {
    return std::format("{:016x}", id);
}

} // namespace

u64 texture_size(TextureFormat format, u32 width, u32 height, u32 levels) noexcept {
    u64 size = 0;
    for (u32 level = 0; level < levels; ++level) {
        size += format == TextureFormat::BC7 ? u64{(width + 3) / 4} * ((height + 3) / 4) * 16
                                             : u64{width} * height * 4;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return size;
}

TextureData texture_from_image(const Image& image, bool srgb, bool mipmaps) {
    TextureData texture{.format = TextureFormat::RGBA8,
                        .srgb = srgb,
                        .width = image.width,
                        .height = image.height,
                        .levels = mipmaps ? mip_count(image.width, image.height) : 1,
                        .data = {}};
    texture.data = mip_chain(image, srgb, texture.levels);
    return texture;
}

u64 content_hash(std::span<const std::byte> data) noexcept {
    u64 hash = 14695981039346656037ull;
    for (const std::byte b : data) {
        hash ^= std::to_integer<u64>(b);
        hash *= 1099511628211ull;
    }
    return hash;
}

Result<SourceStamp> stamp_file(std::string_view path) {
    const auto status = filesystem::status(path);
    if (!status) {
        return std::unexpected(std::format("Datei '{}' gibt es nicht", path));
    }
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    return SourceStamp{.path = filesystem::normalize(path),
                       .size = status->size,
                       .modified = status->modified,
                       .hash = content_hash(*bytes)};
}

bool stamps_current(std::vector<SourceStamp>& stamps, bool* refreshed) {
    bool touched = false;
    for (SourceStamp& stamp : stamps) {
        const auto status = filesystem::status(stamp.path);
        if (!status || status->size != stamp.size) {
            return false;
        }
        if (status->modified == stamp.modified) {
            continue;
        }
        auto bytes = filesystem::read_file(stamp.path);
        if (!bytes || content_hash(*bytes) != stamp.hash) {
            return false;
        }
        stamp.modified = status->modified;
        touched = true;
    }
    if (refreshed != nullptr) {
        *refreshed = touched;
    }
    return true;
}

std::vector<std::byte> write_cooked_model(const CookedModel& cooked) {
    NGINE_PROFILE_ZONE("asset::write_cooked_model");
    BinaryWriter w;
    w.write(cooked_magic);
    w.write(cooked_model_version);
    w.write(static_cast<u32>(cooked.sources.size()));
    for (const SourceStamp& stamp : cooked.sources) {
        w.write_string(stamp.path);
        w.write(stamp.size);
        w.write(stamp.modified);
        w.write(stamp.hash);
    }
    write_model(w, cooked.model);
    w.write(static_cast<u32>(cooked.textures.size()));
    for (const CookedTexture& texture : cooked.textures) {
        w.write(texture.image);
        w.write(texture.srgb);
        write_raw(w, texture.ktx2);
        w.write(texture.bc7.width);
        w.write(texture.bc7.height);
        w.write(texture.bc7.levels);
        write_raw(w, texture.bc7.data);
    }
    return w.take();
}

Result<CookedModel> read_cooked_model(std::span<const std::byte> data) {
    NGINE_PROFILE_ZONE("asset::read_cooked_model");
    Input in{data};
    if (in.get<u32>() != cooked_magic) {
        return std::unexpected(std::string{"keine gekochte Modelldatei"});
    }
    if (const u32 version = in.get<u32>(); version != cooked_model_version) {
        return std::unexpected(std::format("Version {} statt {}", version, cooked_model_version));
    }
    CookedModel cooked;
    cooked.sources.resize(in.count(28));
    for (SourceStamp& stamp : cooked.sources) {
        stamp.path = in.text();
        stamp.size = in.get<u64>();
        stamp.modified = in.get<i64>();
        stamp.hash = in.get<u64>();
    }
    cooked.model = read_model(in);
    cooked.textures.resize(in.count(33));
    for (CookedTexture& texture : cooked.textures) {
        texture.image = in.get<u32>();
        texture.srgb = in.get<bool>();
        texture.ktx2 = in.blob();
        texture.bc7.format = TextureFormat::BC7;
        texture.bc7.srgb = texture.srgb;
        texture.bc7.width = in.get<u32>();
        texture.bc7.height = in.get<u32>();
        texture.bc7.levels = in.get<u32>();
        texture.bc7.data = in.blob();
    }
    if (!in.ok() || in.remaining() != 0) {
        return std::unexpected(std::string{"gekochte Modelldatei ist abgeschnitten oder kaputt"});
    }
    if (auto valid = validate(cooked); !valid) {
        return std::unexpected("gekochte Modelldatei: " + valid.error());
    }
    return cooked;
}

AssetCache::AssetCache(std::string directory)
    : directory_{directory.empty() ? std::string{} : filesystem::normalize(directory)} {}

std::string AssetCache::model_path(AssetId id) const {
    return filesystem::join(directory_, hex(id) + ".nmodel");
}

Result<std::optional<CookedModel>> AssetCache::load_model(AssetId id) const {
    NGINE_PROFILE_ZONE("AssetCache::load_model");
    if (!enabled()) {
        return std::optional<CookedModel>{};
    }
    const std::string path = model_path(id);
    if (!filesystem::exists(path)) {
        return std::optional<CookedModel>{};
    }
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto cooked = read_cooked_model(*bytes);
    if (!cooked) {
        // Andere Version oder kaputt: wie veraltet behandeln, Kochen ersetzt die Datei.
        log::debug("Cache {}: {}", path, cooked.error());
        return std::optional<CookedModel>{};
    }
    bool refreshed = false;
    if (!stamps_current(cooked->sources, &refreshed)) {
        return std::optional<CookedModel>{};
    }
    if (refreshed) {
        // Nur Änderungszeiten anders (z. B. nach git checkout): Stempel erneuern, damit das
        // nächste Laden nicht wieder liest und rechnet.
        if (auto stored = store_model(id, *cooked); !stored) {
            log::debug("Cache {}: Stempel nicht erneuert: {}", path, stored.error());
        }
    }
    return std::optional<CookedModel>{*std::move(cooked)};
}

bool AssetCache::model_current(AssetId id) const {
    if (!enabled()) {
        return false;
    }
    auto bytes = filesystem::read_file(model_path(id));
    if (!bytes || bytes->size() < 12) {
        return false;
    }
    // Nur der Kopf mit den Stempeln: Modell und Texturen braucht die Prüfung nicht.
    Input in{*bytes};
    if (in.get<u32>() != cooked_magic || in.get<u32>() != cooked_model_version) {
        return false;
    }
    std::vector<SourceStamp> stamps(in.count(28));
    for (SourceStamp& stamp : stamps) {
        stamp.path = in.text();
        stamp.size = in.get<u64>();
        stamp.modified = in.get<i64>();
        stamp.hash = in.get<u64>();
    }
    return in.ok() && !stamps.empty() && stamps_current(stamps);
}

Result<void> AssetCache::store_model(AssetId id, const CookedModel& model) const {
    if (!enabled()) {
        return std::unexpected(std::string{"kein Cache-Ordner"});
    }
    // write_file() schreibt über eine temporäre Datei und legt Ordner an.
    return filesystem::write_file(model_path(id), write_cooked_model(model));
}

std::string default_cache_dir(std::string_view asset_root) {
    return filesystem::join(filesystem::normalize(asset_root), ".cache");
}

} // namespace ngine::asset
