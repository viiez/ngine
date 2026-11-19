module;

// Einzige Übersetzungseinheit, die fastgltf kennt (ADR 0027).
#include <ngine/profile.hpp>

#include <algorithm>
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

// Gelenke und Gewichte eines Teilnetzes (JOINTS_n, WEIGHTS_n). Mehr als vier Einflüsse kürzt der
// Import auf die vier stärksten; Gewichte werden auf die Summe 1 normiert und als u16 gespeichert.
Converted<std::vector<SkinVertex>> convert_skin_attributes(const fastgltf::Asset& gltf,
                                                           const fastgltf::Primitive& source,
                                                           std::size_t count,
                                                           std::string_view what) {
    struct Influence {
        u32 joint = 0;
        f32 weight = 0.0f;
    };
    std::vector<std::vector<Influence>> influences;
    for (u32 set = 0;; ++set) {
        const auto* joints = source.findAttribute(std::format("JOINTS_{}", set));
        const auto* weights = source.findAttribute(std::format("WEIGHTS_{}", set));
        if (joints == source.attributes.end() || weights == source.attributes.end()) {
            break;
        }
        for (const auto* attribute : {joints, weights}) {
            if (auto ok =
                    check_accessor(gltf, attribute->accessorIndex, fastgltf::AccessorType::Vec4, what);
                !ok) {
                return std::unexpected(ok.error());
            }
            if (gltf.accessors[attribute->accessorIndex].count != count) {
                return std::unexpected(Failure{
                    std::format("{}: Gelenke und Gewichte passen nicht zu POSITION", what)});
            }
        }
        if (set == 0) {
            influences.resize(count);
        }
        std::vector<fastgltf::math::fvec4> joint_values(count);
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            gltf, gltf.accessors[joints->accessorIndex],
            [&](fastgltf::math::fvec4 j, std::size_t i) { joint_values[i] = j; });
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
            gltf, gltf.accessors[weights->accessorIndex],
            [&](fastgltf::math::fvec4 w, std::size_t i) {
                for (std::size_t c = 0; c < 4; ++c) {
                    if (w[c] > 0.0f) {
                        influences[i].push_back(
                            {static_cast<u32>(joint_values[i][c] + 0.5f), w[c]});
                    }
                }
            });
    }
    std::vector<SkinVertex> skin;
    if (influences.empty()) {
        return skin;
    }
    skin.resize(count);
    bool truncated = false;
    for (std::size_t i = 0; i < count; ++i) {
        auto& list = influences[i];
        std::sort(list.begin(), list.end(),
                  [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
        if (list.size() > 4) {
            list.resize(4);
            truncated = true;
        }
        f32 sum = 0.0f;
        for (const Influence& influence : list) {
            if (influence.joint >= max_joints) {
                return std::unexpected(
                    Failure{std::format("{}: Gelenk {} ist zu groß", what, influence.joint)});
            }
            sum += influence.weight;
        }
        if (sum <= 0.0f) {
            skin[i].weights[0] = 65535; // ohne Gewicht: starr am ersten Gelenk
            continue;
        }
        // Auf 65535 runden und den Rest dem stärksten geben, damit die Summe genau stimmt.
        u32 total = 0;
        for (std::size_t c = 0; c < list.size(); ++c) {
            const u32 weight = static_cast<u32>(list[c].weight / sum * 65535.0f + 0.5f);
            skin[i].joints[c] = static_cast<u16>(list[c].joint);
            skin[i].weights[c] = static_cast<u16>(weight > 65535 ? 65535 : weight);
            total += skin[i].weights[c];
        }
        skin[i].weights[0] = static_cast<u16>(static_cast<i64>(skin[i].weights[0]) + 65535 -
                                              static_cast<i64>(total));
    }
    if (truncated) {
        log::warn("glTF, {}: mehr als vier Gelenke je Eckpunkt, auf die vier stärksten gekürzt",
                  what);
    }
    return skin;
}

Converted<Skin> convert_skin(const fastgltf::Asset& gltf, const fastgltf::Skin& source,
                             usize number) {
    const std::string what = std::format("Skin {} ({})", number, source.name);
    Skin skin;
    skin.name = std::string{source.name};
    if (source.joints.empty() || source.joints.size() > max_joints) {
        return std::unexpected(
            Failure{std::format("{}: {} Gelenke (1 bis {} erlaubt)", what, source.joints.size(),
                                max_joints)});
    }
    for (const std::size_t joint : source.joints) {
        if (joint >= gltf.nodes.size()) {
            return std::unexpected(Failure{std::format("{}: Knoten {} fehlt", what, joint)});
        }
        skin.joints.push_back(static_cast<u32>(joint));
    }
    skin.inverse_bind.assign(skin.joints.size(), math::mat4_identity());
    if (source.inverseBindMatrices.has_value()) {
        const std::size_t accessor = source.inverseBindMatrices.value();
        if (auto ok = check_accessor(gltf, accessor, fastgltf::AccessorType::Mat4, what); !ok) {
            return std::unexpected(ok.error());
        }
        if (gltf.accessors[accessor].count != skin.joints.size()) {
            return std::unexpected(
                Failure{std::format("{}: Zahl der Bindematrizen passt nicht", what)});
        }
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fmat4x4>(
            gltf, gltf.accessors[accessor], [&](const fastgltf::math::fmat4x4& m, std::size_t i) {
                math::Mat4& target = skin.inverse_bind[i];
                for (std::size_t c = 0; c < 4; ++c) {
                    target[c] = {m.col(c)[0], m.col(c)[1], m.col(c)[2], m.col(c)[3]};
                }
            });
    }
    return skin;
}

// Hermite-Spline zwischen zwei Schlüsseln (glTF CUBICSPLINE), Werte je width Zahlen.
void hermite(const std::vector<f32>& data, usize width, usize key, f32 dt, f32 t, f32* out) {
    // Je Schlüssel: Tangente rein, Wert, Tangente raus.
    const f32* v0 = &data[(key * 3 + 1) * width];
    const f32* b0 = &data[(key * 3 + 2) * width];
    const f32* a1 = &data[((key + 1) * 3 + 0) * width];
    const f32* v1 = &data[((key + 1) * 3 + 1) * width];
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    for (usize c = 0; c < width; ++c) {
        out[c] = (2.0f * t3 - 3.0f * t2 + 1.0f) * v0[c] + (t3 - 2.0f * t2 + t) * dt * b0[c] +
                 (-2.0f * t3 + 3.0f * t2) * v1[c] + (t3 - t2) * dt * a1[c];
    }
}

Converted<Animation> convert_animation(const fastgltf::Asset& gltf,
                                       const fastgltf::Animation& source, usize number) {
    const std::string what = std::format("Animation {} ({})", number, source.name);
    Animation animation;
    animation.name = std::string{source.name};
    for (const fastgltf::AnimationChannel& channel : source.channels) {
        if (!channel.nodeIndex.has_value()) {
            continue;
        }
        AnimationChannel converted;
        switch (channel.path) {
        case fastgltf::AnimationPath::Translation:
            converted.path = AnimationPath::Translation;
            break;
        case fastgltf::AnimationPath::Rotation: converted.path = AnimationPath::Rotation; break;
        case fastgltf::AnimationPath::Scale: converted.path = AnimationPath::Scale; break;
        default:
            log::warn("glTF, {}: Morph Targets werden nicht unterstützt, Spur übergangen", what);
            continue;
        }
        if (channel.nodeIndex.value() >= gltf.nodes.size() ||
            channel.samplerIndex >= source.samplers.size()) {
            return std::unexpected(Failure{std::format("{}: Spur verweist ins Leere", what)});
        }
        converted.node = static_cast<u32>(channel.nodeIndex.value());
        const fastgltf::AnimationSampler& sampler = source.samplers[channel.samplerIndex];
        const usize width = converted.path == AnimationPath::Rotation ? 4 : 3;
        const auto value_type = width == 4 ? fastgltf::AccessorType::Vec4
                                           : fastgltf::AccessorType::Vec3;
        if (auto ok = check_accessor(gltf, sampler.inputAccessor, fastgltf::AccessorType::Scalar,
                                     what);
            !ok) {
            return std::unexpected(ok.error());
        }
        if (auto ok = check_accessor(gltf, sampler.outputAccessor, value_type, what); !ok) {
            return std::unexpected(ok.error());
        }
        const fastgltf::Accessor& input = gltf.accessors[sampler.inputAccessor];
        const fastgltf::Accessor& output = gltf.accessors[sampler.outputAccessor];
        const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
        if (input.count == 0 || output.count != input.count * (cubic ? 3 : 1)) {
            return std::unexpected(
                Failure{std::format("{}: Zeiten und Werte passen nicht zusammen", what)});
        }
        std::vector<f32> times(input.count);
        fastgltf::copyFromAccessor<f32>(gltf, input, times.data());
        for (usize i = 1; i < times.size(); ++i) {
            if (!(times[i] >= times[i - 1])) {
                return std::unexpected(
                    Failure{std::format("{}: Zeiten sind nicht aufsteigend", what)});
            }
        }
        std::vector<f32> values(output.count * width);
        if (width == 4) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                gltf, output, [&](fastgltf::math::fvec4 v, std::size_t i) {
                    for (usize c = 0; c < 4; ++c) {
                        values[i * 4 + c] = v[c];
                    }
                });
        } else {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                gltf, output, [&](fastgltf::math::fvec3 v, std::size_t i) {
                    for (usize c = 0; c < 3; ++c) {
                        values[i * 3 + c] = v[c];
                    }
                });
        }
        if (cubic) {
            // Linear auf 30 Hz abtasten.
            constexpr f32 rate = 30.0f;
            const f32 start = times.front();
            const f32 end = times.back();
            const usize steps = static_cast<usize>((end - start) * rate) + 1;
            usize key = 0;
            for (usize step = 0; step <= steps; ++step) {
                const f32 time = step == steps ? end : start + static_cast<f32>(step) / rate;
                while (key + 2 < times.size() && times[key + 1] <= time) {
                    ++key;
                }
                f32 out[4] = {};
                if (times.size() == 1) {
                    std::copy_n(&values[width], width, out);
                } else {
                    const f32 dt = times[key + 1] - times[key];
                    const f32 t = dt > 0.0f ? std::clamp((time - times[key]) / dt, 0.0f, 1.0f)
                                            : 0.0f;
                    hermite(values, width, key, dt, t, out);
                }
                if (width == 4) {
                    const math::Quat q =
                        math::normalize(math::Quat{out[0], out[1], out[2], out[3]});
                    out[0] = q.x;
                    out[1] = q.y;
                    out[2] = q.z;
                    out[3] = q.w;
                }
                converted.times.push_back(time);
                converted.values.insert(converted.values.end(), out, out + width);
                if (step == steps || time >= end) {
                    break;
                }
            }
            converted.interpolation = Interpolation::Linear;
        } else {
            converted.times = std::move(times);
            converted.values = std::move(values);
            converted.interpolation = sampler.interpolation == fastgltf::AnimationInterpolation::Step
                                          ? Interpolation::Step
                                          : Interpolation::Linear;
        }
        animation.duration = std::max(animation.duration, converted.times.back());
        animation.channels.push_back(std::move(converted));
    }
    return animation;
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

    if (auto skin = convert_skin_attributes(gltf, source, positions.count, what); !skin) {
        return std::unexpected(skin.error());
    } else {
        primitive.skin = std::move(*skin);
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
        primitive.generated_tangents = true;
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

    for (usize i = 0; i < gltf.skins.size(); ++i) {
        auto skin = convert_skin(gltf, gltf.skins[i], i);
        if (!skin) {
            return std::unexpected(skin.error());
        }
        model.skins.push_back(std::move(*skin));
    }
    for (usize i = 0; i < gltf.nodes.size(); ++i) {
        const fastgltf::Node& source = gltf.nodes[i];
        if (!source.skinIndex.has_value()) {
            continue;
        }
        const u32 skin = static_cast<u32>(source.skinIndex.value());
        if (skin >= model.skins.size()) {
            return std::unexpected(Failure{std::format("Knoten {}: Skin {} fehlt", i, skin)});
        }
        model.nodes[i].skin = skin;
        if (const auto mesh = model.nodes[i].mesh) {
            Mesh& target = model.meshes[*mesh];
            if (!target.skin) {
                target.skin = skin;
            } else if (*target.skin != skin) {
                log::warn("glTF: Mesh {} ({}) hat mehrere Skins, nur der erste gilt", *mesh,
                          target.name);
            }
        }
    }
    // Gelenke in SkinVertex müssen im Skin des Netzes liegen; Netze ohne Skin bleiben starr.
    for (usize m = 0; m < model.meshes.size(); ++m) {
        Mesh& mesh = model.meshes[m];
        for (Primitive& primitive : mesh.primitives) {
            if (!mesh.skin) {
                primitive.skin.clear();
                continue;
            }
            const usize joints = model.skins[*mesh.skin].joints.size();
            for (const SkinVertex& vertex : primitive.skin) {
                for (usize c = 0; c < 4; ++c) {
                    if (vertex.weights[c] != 0 && vertex.joints[c] >= joints) {
                        return std::unexpected(Failure{std::format(
                            "Mesh {} ({}): Gelenk {} liegt außerhalb des Skins", m, mesh.name,
                            vertex.joints[c])});
                    }
                }
            }
        }
        if (mesh.skin && std::ranges::any_of(mesh.primitives, [](const Primitive& p) {
                return p.skin.empty();
            })) {
            log::warn("glTF: Mesh {} ({}) hat einen Skin, aber Teile ohne Gewichte", m,
                      mesh.name);
        }
    }
    for (usize i = 0; i < gltf.animations.size(); ++i) {
        auto animation = convert_animation(gltf, gltf.animations[i], i);
        if (!animation) {
            return std::unexpected(animation.error());
        }
        model.animations.push_back(std::move(*animation));
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
