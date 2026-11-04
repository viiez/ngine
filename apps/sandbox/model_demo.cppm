module;

#include <ngine/profile.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Standardansicht der Sandbox seit R1.3: ein glTF-Modell (--model) oder ein Raster aus Kugeln mit
// steigendem Metallic- und Roughness-Wert, gezeichnet vom Renderer aus der Welt und mit einer
// freien Kamera begehbar.
export module sandbox.model_demo;

import ngine.core;
import ngine.asset;
import ngine.filesystem;
import ngine.math;
import ngine.profile;
import ngine.reflect;
import ngine.render;
import ngine.rhi;
import ngine.rhi.vulkan;
import ngine.world;

namespace sandbox {

using namespace ngine;
using math::DVec3;
using math::Vec3;

// Zustand der Eingabe für die freie Kamera, unabhängig von der Plattformschicht.
export struct FlyInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool fast = false;
    // Mausbewegung seit dem letzten update() in Pixeln.
    f32 look_x = 0.0f;
    f32 look_y = 0.0f;
};

namespace {

constexpr u32 grid_size = 7;
constexpr f32 grid_spacing = 2.5f;

// Hülle aller MeshRenderer in Weltkoordinaten (relativ zu origin, damit 32 Bit reichen).
std::optional<std::pair<Vec3, Vec3>> world_bounds(world::World& world, render::MeshStore& meshes,
                                                  DVec3 origin) {
    constexpr f32 big = std::numeric_limits<f32>::max();
    Vec3 low{big, big, big};
    Vec3 high{-big, -big, -big};
    bool any = false;
    world.each<const world::WorldTransform, const render::MeshRenderer>(
        [&](const world::WorldTransform& transform, const render::MeshRenderer& renderer) {
            const render::GpuMesh* mesh = meshes.get(renderer.mesh);
            if (mesh == nullptr || mesh->submeshes.empty()) {
                return;
            }
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3 local{(corner & 1) != 0 ? mesh->bounds_max.x : mesh->bounds_min.x,
                                 (corner & 2) != 0 ? mesh->bounds_max.y : mesh->bounds_min.y,
                                 (corner & 4) != 0 ? mesh->bounds_max.z : mesh->bounds_min.z};
                const Vec3 p =
                    math::to_f32(transform.value.transform_point(math::to_f64(local)) - origin);
                low = math::min(low, p);
                high = math::max(high, p);
                any = true;
            }
        });
    if (!any) {
        return std::nullopt;
    }
    return std::make_pair(low, high);
}

} // namespace

export class ModelScene {
public:
    // Ohne model_path das Kugelraster. origin verschiebt alles, auch die Kamera (--far).
    [[nodiscard]] static rhi::Result<std::unique_ptr<ModelScene>>
    create(rhi::Device& device, rhi::Format target_format,
           const std::optional<std::string>& model_path, DVec3 origin) {
        auto renderer = render::Renderer::create(device, target_format);
        if (!renderer) {
            return std::unexpected(renderer.error());
        }
        auto scene = std::unique_ptr<ModelScene>{new ModelScene{*std::move(renderer), origin}};
        if (auto result = model_path ? scene->load_model(*model_path) : scene->build_spheres();
            !result) {
            return std::unexpected(result.error());
        }
        scene->frame_camera();
        return scene;
    }

    // Bewegt die freie Kamera: W/S vor und zurück, A/D seitwärts, E/Q hoch und runter, Shift
    // schneller; die Maus dreht.
    void update(f32 dt, const FlyInput& input) {
        yaw_ -= input.look_x * 0.0025f;
        pitch_ = std::clamp(pitch_ - input.look_y * 0.0025f, -1.5f, 1.5f);
        const math::Quat rotation = math::angle_axis(yaw_, {0.0f, 1.0f, 0.0f}) *
                                    math::angle_axis(pitch_, {1.0f, 0.0f, 0.0f});
        const Vec3 forward = rotation * Vec3{0.0f, 0.0f, -1.0f};
        const Vec3 right = rotation * Vec3{1.0f, 0.0f, 0.0f};
        Vec3 move{0.0f, 0.0f, 0.0f};
        move = move + forward * (static_cast<f32>(input.forward) - static_cast<f32>(input.back));
        move = move + right * (static_cast<f32>(input.right) - static_cast<f32>(input.left));
        move.y += static_cast<f32>(input.up) - static_cast<f32>(input.down);
        const f32 speed = speed_ * (input.fast ? 4.0f : 1.0f);
        position_ = position_ + math::to_f64(math::normalize(move) * (speed * dt));
        camera_.set(world::Transform{.position = position_, .rotation = rotation});
    }

    void add_passes(render::RenderGraph& graph, render::TextureHandle target) {
        NGINE_PROFILE_ZONE("ModelScene::add_passes");
        world::update_transforms(world_);
        renderer_->add_passes(graph, world_, target);
    }

    [[nodiscard]] std::string stats() const {
        const render::RenderStats& stats = renderer_->stats();
        return std::format("{} Draws, {} Dreiecke, {} Meshes, {} Materialien", stats.draws,
                           stats.triangles, renderer_->meshes().size(),
                           renderer_->materials().size());
    }

private:
    static reflect::TypeRegistry make_types() {
        reflect::TypeRegistry types;
        world::register_types(types);
        render::register_types(types);
        return types;
    }

    ModelScene(std::unique_ptr<render::Renderer> renderer, DVec3 origin)
        : types_{make_types()}, world_{types_}, renderer_{std::move(renderer)}, origin_{origin} {
        camera_ = world_.create("Kamera");
        camera_.set(world::Transform{}).set(render::Camera{});
    }

    rhi::Result<void> load_model(const std::string& path) {
        auto model = asset::load_gltf(path);
        if (!model) {
            return std::unexpected(model.error());
        }
        auto instance = render::upload_model(*renderer_, *model);
        if (!instance) {
            return std::unexpected(instance.error());
        }
        world::Entity root = render::spawn_model(world_, *model, *instance, "Modell");
        root.set(world::Transform{.position = origin_});
        log::info("Modell geladen: {} ({} Meshes, {} Materialien, {} Bilder)", path,
                  model->meshes.size(), model->materials.size(), model->images.size());
        return {};
    }

    // 7 × 7 Kugeln: Roughness wächst nach rechts, Metallic nach oben.
    rhi::Result<void> build_spheres() {
        const asset::Primitive sphere = asset::make_sphere(1.0f, 64, 32);
        render::MaterialStore& materials = renderer_->materials();
        std::vector<u32> row_materials;
        for (u32 row = 0; row < grid_size; ++row) {
            for (u32 column = 0; column < grid_size; ++column) {
                const f32 metallic = static_cast<f32>(row) / static_cast<f32>(grid_size - 1);
                const f32 roughness =
                    std::max(static_cast<f32>(column) / static_cast<f32>(grid_size - 1), 0.05f);
                row_materials.push_back(materials.add({.base_color = {0.9f, 0.45f, 0.2f, 1.0f},
                                                       .metallic = metallic,
                                                       .roughness = roughness}));
            }
        }
        const std::vector<render::SubmeshData> submeshes{
            {.vertices = sphere.vertices, .indices = sphere.indices, .material = 0}};
        auto mesh = renderer_->meshes().add(submeshes);
        if (!mesh) {
            return std::unexpected(mesh.error());
        }
        world::Entity root = world_.create("Kugelraster");
        root.set(world::Transform{.position = origin_});
        const f32 half = static_cast<f32>(grid_size - 1) * grid_spacing * 0.5f;
        for (u32 row = 0; row < grid_size; ++row) {
            for (u32 column = 0; column < grid_size; ++column) {
                world::Entity entity = world_.create(std::format("Kugel {} {}", row, column));
                entity
                    .set(world::Transform{
                        .position = {static_cast<f64>(column) * grid_spacing - half,
                                     static_cast<f64>(row) * grid_spacing - half, 0.0}})
                    .set(render::MeshRenderer{.mesh = *mesh,
                                              .material = row_materials[row * grid_size + column]});
                entity.set_parent(root);
            }
        }
        return {};
    }

    // Kamera vor die Szene stellen, sodass alles ins Bild passt.
    void frame_camera() {
        world::update_transforms(world_);
        const auto bounds = world_bounds(world_, renderer_->meshes(), origin_);
        Vec3 center{0.0f, 0.0f, 0.0f};
        f32 radius = 1.0f;
        if (bounds) {
            center = (bounds->first + bounds->second) * 0.5f;
            radius = std::max(math::length(bounds->second - bounds->first) * 0.5f, 0.01f);
        }
        const f32 fov = render::Camera{}.fov_y;
        const f32 distance = radius / std::tan(fov * 0.5f);
        position_ = origin_ + math::to_f64(center + Vec3{0.0f, 0.0f, distance});
        speed_ = std::max(radius, 0.5f);
        camera_.get_mut<render::Camera>()->near_plane = std::max(radius * 0.01f, 0.01f);
        update(0.0f, {});
    }

    reflect::TypeRegistry types_;
    world::World world_;
    std::unique_ptr<render::Renderer> renderer_;
    DVec3 origin_;
    world::Entity camera_;
    DVec3 position_{0.0, 0.0, 0.0};
    f32 yaw_ = 0.0f;
    f32 pitch_ = 0.0f;
    f32 speed_ = 3.0f;
};

namespace {

rhi::Result<void> write_ppm(const std::string& path, std::span<const u8> rgba,
                            rhi::Extent2D extent) {
    const std::string header = std::format("P6\n{} {}\n255\n", extent.width, extent.height);
    std::vector<std::byte> data;
    data.reserve(header.size() + usize{extent.width} * extent.height * 3);
    for (const char c : header) {
        data.push_back(static_cast<std::byte>(c));
    }
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        data.push_back(static_cast<std::byte>(rgba[i]));
        data.push_back(static_cast<std::byte>(rgba[i + 1]));
        data.push_back(static_cast<std::byte>(rgba[i + 2]));
    }
    return filesystem::write_file(path, data);
}

} // namespace

// Offscreen auf der GPU zeichnen und als PPM speichern (für die CI): Kugelraster oder Modell.
export int run_model_offscreen(const std::optional<std::string>& model_path,
                               const std::string& image_path, bool baseline, DVec3 origin) {
    auto device = rhi::vulkan::create_device({
        .application_name = "NGine Sandbox",
#ifdef NDEBUG
        .enable_validation = false,
#else
        .enable_validation = true,
#endif
        .disabled_features = {.mesh_shader = baseline, .ray_tracing = baseline},
    });
    if (!device) {
        log::fatal("{}", device.error());
        return 1;
    }
    int exit_code = 0;
    {
        constexpr rhi::Extent2D extent{960, 540};
        constexpr rhi::Format format = rhi::Format::RGBA8Srgb;
        auto target = (*device)->create_texture({.extent = extent, .format = format});
        if (!target) {
            log::fatal("{}", target.error());
            return 1;
        }
        auto scene = ModelScene::create(**device, format, model_path, origin);
        if (!scene) {
            log::fatal("{}", scene.error());
            return 1;
        }
        render::TexturePool pool{**device};
        render::RenderGraph graph;
        const auto output = graph.import_texture("Bild", **target, rhi::ResourceState::Undefined,
                                                 rhi::ResourceState::TransferSrc);
        (*scene)->add_passes(graph, output);
        (*device)->execute([&](rhi::CommandList& commands) { graph.execute(commands, pool); });
        log::info("Szene: {}", (*scene)->stats());
        const auto pixels = (*device)->read_texture(**target, rhi::ResourceState::TransferSrc);
        if (auto written = write_ppm(image_path, pixels, extent); !written) {
            log::error("Bild lässt sich nicht schreiben: {}", written.error());
            exit_code = 1;
        } else {
            log::info("Bild gespeichert: {}", image_path);
        }
    }
    if (const auto errors = (*device)->validation_errors(); errors > 0) {
        log::error("Die Validation Layers haben {} Fehler gemeldet", errors);
        exit_code = 1;
    }
    return exit_code;
}

} // namespace sandbox
