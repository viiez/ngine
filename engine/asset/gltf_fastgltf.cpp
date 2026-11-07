module;

// Einzige Übersetzungseinheit, die fastgltf kennt (ADR 0027).
#include <ngine/profile.hpp>

#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

module ngine.asset;

import ngine.core;
import ngine.filesystem;
import ngine.math;
import ngine.profile;

namespace ngine::asset {

namespace {

// Fehler beim Umwandeln; die Meldung bekommt später den Pfad vorangestellt.
struct Failure {
    std::string message;
};

template <typename T>
using Converted = std::expected<T, Failure>;

std::filesystem::path to_path(std::string_view utf8) {
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

std::optional<u32> index(const fastgltf::Optional<std::size_t>& value) {
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<u32>(value.value());
}

// Bytes einer Datenquelle, wenn sie schon im Speicher liegen.
std::optional<std::span<const std::byte>> bytes_of(const fastgltf::DataSource& source) {
    return std::visit(
        fastgltf::visitor{
            [](const fastgltf::sources::Array& array) -> std::optional<std::span<const std::byte>> {
                return std::span<const std::byte>{array.bytes.data(), array.bytes.size_bytes()};
            },
            [](const fastgltf::sources::Vector& vector)
                -> std::optional<std::span<const std::byte>> { return vector.bytes; },
            [](const fastgltf::sources::ByteView& view)
                -> std::optional<std::span<const std::byte>> {
                return std::span<const std::byte>{view.bytes.data(), view.bytes.size()};
            },
            [](const auto&) -> std::optional<std::span<const std::byte>> { return std::nullopt; }},
        source);
}

// fastgltf prüft beim Iterieren keine Grenzen. Darum vorher: Liegt jedes Element des Accessors
// in seiner Buffer View und die View in ihrem geladenen Puffer?
Converted<void> check_accessor(const fastgltf::Asset& gltf, std::size_t accessor_index,
                               fastgltf::AccessorType type, std::string_view what) {
    if (accessor_index >= gltf.accessors.size()) {
        return std::unexpected(Failure{std::format("{}: Accessor {} fehlt", what, accessor_index)});
    }
    const fastgltf::Accessor& accessor = gltf.accessors[accessor_index];
    if (accessor.type != type) {
        return std::unexpected(
            Failure{std::format("{}: Accessor {} hat den falschen Typ", what, accessor_index)});
    }
    if (accessor.sparse.has_value()) {
        return std::unexpected(
            Failure{std::format("{}: dünn besetzte Accessoren werden nicht unterstützt", what)});
    }
    if (!accessor.bufferViewIndex.has_value()) {
        return {}; // nur Nullen, fastgltf liefert sie ohne Pufferzugriff
    }
    const std::size_t view_index = accessor.bufferViewIndex.value();
    if (view_index >= gltf.bufferViews.size()) {
        return std::unexpected(Failure{std::format("{}: Buffer View {} fehlt", what, view_index)});
    }
    const fastgltf::BufferView& view = gltf.bufferViews[view_index];
    if (view.meshoptCompression) {
        return std::unexpected(
            Failure{std::format("{}: komprimierte Puffer werden nicht unterstützt", what)});
    }
    if (view.bufferIndex >= gltf.buffers.size()) {
        return std::unexpected(Failure{std::format("{}: Puffer {} fehlt", what, view.bufferIndex)});
    }
    const auto buffer = bytes_of(gltf.buffers[view.bufferIndex].data);
    if (!buffer || view.byteOffset > buffer->size() ||
        view.byteLength > buffer->size() - view.byteOffset) {
        return std::unexpected(Failure{
            std::format("{}: Puffer {} ist nicht geladen oder zu kurz", what, view.bufferIndex)});
    }
    const std::size_t element = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
    const std::size_t stride = view.byteStride.has_value() ? view.byteStride.value() : element;
    if (accessor.count > 0) {
        const std::size_t needed = accessor.byteOffset + (accessor.count - 1) * stride + element;
        if (element == 0 || needed > view.byteLength) {
            return std::unexpected(Failure{std::format(
                "{}: Accessor {} reicht über seine Buffer View hinaus", what, accessor_index)});
        }
    }
    return {};
}

Converted<Primitive> convert_primitive(const fastgltf::Asset& gltf,
                                       const fastgltf::Primitive& source, std::string_view what) {
    Primitive primitive;
    primitive.material = index(source.materialIndex);
    if (primitive.material && *primitive.material >= gltf.materials.size()) {
        return std::unexpected(
            Failure{std::format("{}: Material {} fehlt", what, *primitive.material)});
    }

    const auto* position = source.findAttribute("POSITION");
    if (position == source.attributes.end()) {
        return std::unexpected(Failure{std::format("{}: ohne POSITION", what)});
    }
    if (auto ok = check_accessor(gltf, position->accessorIndex, fastgltf::AccessorType::Vec3, what);
        !ok) {
        return std::unexpected(ok.error());
    }
    const fastgltf::Accessor& positions = gltf.accessors[position->accessorIndex];
    primitive.vertices.resize(positions.count);
    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
        gltf, positions, [&](fastgltf::math::fvec3 p, std::size_t i) {
            primitive.vertices[i].position = {p.x(), p.y(), p.z()};
        });

    // Weitere Attribute; jedes muss so viele Elemente haben wie POSITION.
    const auto attribute =
        [&](std::string_view name,
            fastgltf::AccessorType type) -> Converted<const fastgltf::Accessor*> {
        const auto* found = source.findAttribute(name);
        if (found == source.attributes.end()) {
            return nullptr;
        }
        if (auto ok = check_accessor(gltf, found->accessorIndex, type, what); !ok) {
            return std::unexpected(ok.error());
        }
        const fastgltf::Accessor& accessor = gltf.accessors[found->accessorIndex];
        if (accessor.count != positions.count) {
            return std::unexpected(Failure{std::format("{}: {} hat {} statt {} Elemente", what,
                                                       name, accessor.count, positions.count)});
        }
        return &accessor;
    };

    const auto normals = attribute("NORMAL", fastgltf::AccessorType::Vec3);
    const auto tangents = attribute("TANGENT", fastgltf::AccessorType::Vec4);
    const auto uvs = attribute("TEXCOORD_0", fastgltf::AccessorType::Vec2);
    for (const auto* result : {&normals, &tangents, &uvs}) {
        if (!*result) {
            return std::unexpected(result->error());
        }
    }
    if (*uvs) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
            gltf, **uvs, [&](fastgltf::math::fvec2 uv, std::size_t i) {
                primitive.vertices[i].uv = {uv.x(), uv.y()};
            });
    }

    if (source.indicesAccessor.has_value()) {
        const std::size_t indices = source.indicesAccessor.value();
        if (auto ok = check_accessor(gltf, indices, fastgltf::AccessorType::Scalar, what); !ok) {
            return std::unexpected(ok.error());
        }
        primitive.indices.resize(gltf.accessors[indices].count);
        fastgltf::copyFromAccessor<u32>(gltf, gltf.accessors[indices], primitive.indices.data());
        for (const u32 i : primitive.indices) {
            if (i >= primitive.vertices.size()) {
                return std::unexpected(
                    Failure{std::format("{}: Index {} liegt außerhalb der Eckpunkte", what, i)});
            }
        }
    } else {
        primitive.indices.resize(primitive.vertices.size());
        for (u32 i = 0; i < primitive.indices.size(); ++i) {
            primitive.indices[i] = i;
        }
    }
    if (primitive.indices.size() % 3 != 0) {
        return std::unexpected(Failure{std::format("{}: {} Indizes sind keine ganzen Dreiecke",
                                                   what, primitive.indices.size())});
    }

    if (*normals) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
            gltf, **normals, [&](fastgltf::math::fvec3 n, std::size_t i) {
                primitive.vertices[i].normal = math::normalize(math::Vec3{n.x(), n.y(), n.z()});
            });
    } else {
        generate_normals(primitive.vertices, primitive.indices);
    }
    if (*tangents) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            gltf, **tangents, [&](fastgltf::math::fvec4 t, std::size_t i) {
                primitive.vertices[i].tangent = {t.x(), t.y(), t.z(), t.w() < 0.0f ? -1.0f : 1.0f};
            });
    } else {
        generate_tangents(primitive.vertices, primitive.indices);
    }
    return primitive;
}

Filter filter(fastgltf::Filter value) {
    switch (value) {
    case fastgltf::Filter::Nearest:
    case fastgltf::Filter::NearestMipMapNearest:
    case fastgltf::Filter::NearestMipMapLinear: return Filter::Nearest;
    default: return Filter::Linear;
    }
}

Wrap wrap(fastgltf::Wrap value) {
    switch (value) {
    case fastgltf::Wrap::ClampToEdge: return Wrap::ClampToEdge;
    case fastgltf::Wrap::MirroredRepeat: return Wrap::MirroredRepeat;
    default: return Wrap::Repeat;
    }
}

SamplerDesc convert_sampler(const fastgltf::Sampler& sampler) {
    SamplerDesc desc;
    desc.wrap_u = wrap(sampler.wrapS);
    desc.wrap_v = wrap(sampler.wrapT);
    if (sampler.magFilter.has_value()) {
        desc.mag = filter(sampler.magFilter.value());
    }
    if (sampler.minFilter.has_value()) {
        const fastgltf::Filter min = sampler.minFilter.value();
        desc.min = filter(min);
        desc.mipmaps = min != fastgltf::Filter::Nearest && min != fastgltf::Filter::Linear;
        desc.mip = min == fastgltf::Filter::NearestMipMapNearest ||
                           min == fastgltf::Filter::LinearMipMapNearest
                       ? Filter::Nearest
                       : Filter::Linear;
    }
    return desc;
}

Converted<Image> convert_image(const fastgltf::Asset& gltf, const fastgltf::Image& source,
                               usize number) {
    std::optional<std::span<const std::byte>> bytes;
    if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&source.data)) {
        if (view->bufferViewIndex < gltf.bufferViews.size()) {
            const fastgltf::BufferView& buffer_view = gltf.bufferViews[view->bufferViewIndex];
            if (buffer_view.bufferIndex < gltf.buffers.size()) {
                const auto buffer = bytes_of(gltf.buffers[buffer_view.bufferIndex].data);
                if (buffer && buffer_view.byteOffset <= buffer->size() &&
                    buffer_view.byteLength <= buffer->size() - buffer_view.byteOffset) {
                    bytes = buffer->subspan(buffer_view.byteOffset, buffer_view.byteLength);
                }
            }
        }
    } else {
        bytes = bytes_of(source.data);
    }
    if (!bytes) {
        return std::unexpected(
            Failure{std::format("Bild {} ({}): Daten fehlen", number, source.name)});
    }
    auto image = decode_image(*bytes);
    if (!image) {
        return std::unexpected(
            Failure{std::format("Bild {} ({}): {}", number, source.name, image.error())});
    }
    image->name = std::string{source.name};
    return std::move(*image);
}

Converted<Model> convert(const fastgltf::Asset& gltf) {
    Model model;

    model.images.reserve(gltf.images.size());
    for (usize i = 0; i < gltf.images.size(); ++i) {
        auto image = convert_image(gltf, gltf.images[i], i);
        if (!image) {
            return std::unexpected(image.error());
        }
        model.images.push_back(std::move(*image));
    }

    for (usize i = 0; i < gltf.textures.size(); ++i) {
        const fastgltf::Texture& source = gltf.textures[i];
        if (!source.imageIndex.has_value() || source.imageIndex.value() >= model.images.size()) {
            return std::unexpected(Failure{std::format("Textur {}: ohne gültiges Bild", i)});
        }
        Texture texture{.image = static_cast<u32>(source.imageIndex.value())};
        if (source.samplerIndex.has_value()) {
            if (source.samplerIndex.value() >= gltf.samplers.size()) {
                return std::unexpected(Failure{std::format("Textur {}: Sampler fehlt", i)});
            }
            texture.sampler = convert_sampler(gltf.samplers[source.samplerIndex.value()]);
        }
        model.textures.push_back(texture);
    }

    for (usize i = 0; i < gltf.materials.size(); ++i) {
        const fastgltf::Material& source = gltf.materials[i];
        const std::string what = std::format("Material {} ({})", i, source.name);
        std::optional<std::string> error;
        const auto texture = [&](const auto& info) -> std::optional<u32> {
            if (!info.has_value()) {
                return std::nullopt;
            }
            if (info->textureIndex >= model.textures.size()) {
                error = std::format("{}: Textur {} fehlt", what, info->textureIndex);
                return std::nullopt;
            }
            if (info->texCoordIndex != 0) {
                log::warn("glTF, {}: nur TEXCOORD_0 wird unterstützt", what);
            }
            return static_cast<u32>(info->textureIndex);
        };
        Material material;
        material.name = std::string{source.name};
        const auto& pbr = source.pbrData;
        material.base_color = {pbr.baseColorFactor.x(), pbr.baseColorFactor.y(),
                               pbr.baseColorFactor.z(), pbr.baseColorFactor.w()};
        material.metallic = pbr.metallicFactor;
        material.roughness = pbr.roughnessFactor;
        material.emissive = {source.emissiveFactor.x(), source.emissiveFactor.y(),
                             source.emissiveFactor.z()};
        material.base_color_texture = texture(pbr.baseColorTexture);
        material.metallic_roughness_texture = texture(pbr.metallicRoughnessTexture);
        material.normal_texture = texture(source.normalTexture);
        material.occlusion_texture = texture(source.occlusionTexture);
        material.emissive_texture = texture(source.emissiveTexture);
        if (source.normalTexture.has_value()) {
            material.normal_scale = source.normalTexture->scale;
        }
        if (source.occlusionTexture.has_value()) {
            material.occlusion_strength = source.occlusionTexture->strength;
        }
        switch (source.alphaMode) {
        case fastgltf::AlphaMode::Mask: material.alpha_mode = AlphaMode::Mask; break;
        case fastgltf::AlphaMode::Blend: material.alpha_mode = AlphaMode::Blend; break;
        default: material.alpha_mode = AlphaMode::Opaque; break;
        }
        material.alpha_cutoff = source.alphaCutoff;
        material.double_sided = source.doubleSided;
        if (error) {
            return std::unexpected(Failure{std::move(*error)});
        }
        model.materials.push_back(std::move(material));
    }

    for (usize m = 0; m < gltf.meshes.size(); ++m) {
        const fastgltf::Mesh& source = gltf.meshes[m];
        Mesh mesh{.name = std::string{source.name}, .primitives = {}};
        for (usize p = 0; p < source.primitives.size(); ++p) {
            const fastgltf::Primitive& primitive = source.primitives[p];
            const std::string what = std::format("Mesh {} ({}), Teil {}", m, source.name, p);
            if (primitive.type != fastgltf::PrimitiveType::Triangles) {
                log::warn("glTF, {}: nur Dreieckslisten werden unterstützt, übersprungen", what);
                continue;
            }
            auto converted = convert_primitive(gltf, primitive, what);
            if (!converted) {
                return std::unexpected(converted.error());
            }
            mesh.primitives.push_back(std::move(*converted));
        }
        model.meshes.push_back(std::move(mesh));
    }

    model.nodes.resize(gltf.nodes.size());
    for (usize i = 0; i < gltf.nodes.size(); ++i) {
        const fastgltf::Node& source = gltf.nodes[i];
        Node& node = model.nodes[i];
        node.name = std::string{source.name};
        node.mesh = index(source.meshIndex);
        if (node.mesh && *node.mesh >= model.meshes.size()) {
            return std::unexpected(Failure{std::format("Knoten {}: Mesh {} fehlt", i, *node.mesh)});
        }
        // Matrizen hat fastgltf schon zerlegt (DecomposeNodeMatrices).
        if (const auto* trs = std::get_if<fastgltf::TRS>(&source.transform)) {
            node.translation = {trs->translation.x(), trs->translation.y(), trs->translation.z()};
            node.rotation = {trs->rotation.x(), trs->rotation.y(), trs->rotation.z(),
                             trs->rotation.w()};
            node.scale = {trs->scale.x(), trs->scale.y(), trs->scale.z()};
        }
        for (const std::size_t child : source.children) {
            if (child >= gltf.nodes.size() || child == i) {
                return std::unexpected(
                    Failure{std::format("Knoten {}: Kind {} ungültig", i, child)});
            }
            node.children.push_back(static_cast<u32>(child));
        }
    }
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        for (const u32 child : model.nodes[i].children) {
            if (model.nodes[child].parent) {
                return std::unexpected(
                    Failure{std::format("Knoten {} hat mehr als einen Elternknoten", child)});
            }
            model.nodes[child].parent = i;
        }
    }
    // Zyklen: Von jedem Knoten aus muss die Kette der Eltern enden.
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        std::optional<u32> current = model.nodes[i].parent;
        for (usize steps = 0; current; ++steps) {
            if (steps > model.nodes.size()) {
                return std::unexpected(Failure{std::format("Knoten {} liegt in einem Zyklus", i)});
            }
            current = model.nodes[*current].parent;
        }
    }

    std::optional<std::size_t> scene;
    if (gltf.defaultScene.has_value() && gltf.defaultScene.value() < gltf.scenes.size()) {
        scene = gltf.defaultScene.value();
    } else if (!gltf.scenes.empty()) {
        scene = 0;
    }
    if (scene) {
        for (const std::size_t root : gltf.scenes[*scene].nodeIndices) {
            if (root >= model.nodes.size()) {
                return std::unexpected(Failure{std::format("Szene: Knoten {} fehlt", root)});
            }
            model.roots.push_back(static_cast<u32>(root));
        }
    } else {
        for (u32 i = 0; i < model.nodes.size(); ++i) {
            if (!model.nodes[i].parent) {
                model.roots.push_back(i);
            }
        }
    }

    if (!gltf.skins.empty() || !gltf.animations.empty()) {
        log::warn("glTF: Skins und Animationen werden noch nicht unterstützt");
    }
    return model;
}

// Lokale Dateien, auf die Puffer und Bilder verweisen (für Hot Reload). Liest nur das JSON.
void collect_files(fastgltf::Parser& parser, fastgltf::GltfDataBuffer& data,
                   const std::filesystem::path& directory, std::vector<std::string>& files) {
    auto gltf = parser.loadGltf(data, directory, fastgltf::Options::None);
    if (gltf.error() != fastgltf::Error::None) {
        return;
    }
    const auto add = [&](const fastgltf::DataSource& source) {
        if (const auto* uri = std::get_if<fastgltf::sources::URI>(&source)) {
            if (uri->uri.isLocalPath()) {
                const std::u8string text = (directory / uri->uri.fspath()).u8string();
                files.push_back(filesystem::normalize(std::string{text.begin(), text.end()}));
            }
        }
    };
    for (const fastgltf::Buffer& buffer : gltf->buffers) {
        add(buffer.data);
    }
    for (const fastgltf::Image& image : gltf->images) {
        add(image.data);
    }
}

} // namespace

Result<Model> load_gltf(std::string_view path, std::vector<std::string>* dependencies) {
    NGINE_PROFILE_ZONE("asset::load_gltf");
    auto bytes = filesystem::read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto fail = [&](std::string_view message) -> Result<Model> {
        return std::unexpected(std::format("glTF {}: {}", path, message));
    };

    try {
        auto data = fastgltf::GltfDataBuffer::FromBytes(bytes->data(), bytes->size());
        if (data.error() != fastgltf::Error::None) {
            return fail(fastgltf::getErrorMessage(data.error()));
        }
        fastgltf::Parser parser{fastgltf::Extensions::None};
        if (dependencies != nullptr) {
            collect_files(parser, data.get(), to_path(path).parent_path(), *dependencies);
        }
        constexpr auto options = fastgltf::Options::LoadExternalBuffers |
                                 fastgltf::Options::LoadExternalImages |
                                 fastgltf::Options::DecomposeNodeMatrices;
        auto gltf = parser.loadGltf(data.get(), to_path(path).parent_path(), options);
        if (gltf.error() != fastgltf::Error::None) {
            return fail(fastgltf::getErrorMessage(gltf.error()));
        }
        if (const fastgltf::Error error = fastgltf::validate(gltf.get());
            error != fastgltf::Error::None) {
            return fail(fastgltf::getErrorMessage(error));
        }
        auto model = convert(gltf.get());
        if (!model) {
            return fail(model.error().message);
        }
        return std::move(*model);
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}

} // namespace ngine::asset
