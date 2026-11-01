#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

import ngine.core;
import ngine.rhi;
import ngine.rhi.vulkan;

// Auswertung der Mindestanforderung und der optionalen Stufen (ADR 0017) ohne GPU.

namespace {

using namespace ngine::rhi;
using namespace ngine::rhi::vulkan;

// Eine GPU mit Vulkan 1.3, die alle Pflicht-Features kann, aber keine optionale Stufe.
DeviceSupport baseline_gpu() {
    return DeviceSupport{
        .api_version = make_api_version(1, 3),
        .extensions = {std::string{swapchain_extension}},
        .required_features = {{.name = "Dynamic Rendering", .supported = true},
                              {.name = "Synchronization2", .supported = true}},
    };
}

void add_mesh_shader(DeviceSupport& gpu) {
    gpu.extensions.emplace_back(mesh_shader_extension);
    gpu.task_shader = true;
    gpu.mesh_shader = true;
}

void add_ray_tracing(DeviceSupport& gpu) {
    gpu.extensions.emplace_back(acceleration_structure_extension);
    gpu.extensions.emplace_back(deferred_host_operations_extension);
    gpu.extensions.emplace_back(ray_query_extension);
    gpu.acceleration_structure = true;
    gpu.ray_query = true;
}

} // namespace

TEST_CASE("GPU mit Vulkan 1.2 wird abgelehnt", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    gpu.api_version = make_api_version(1, 2);

    const auto result = evaluate_device(gpu, false);
    CHECK_FALSE(result.suitable());
    REQUIRE(result.missing.size() == 1);
    CHECK(result.missing[0] == "Vulkan 1.3 (hat 1.2)");
}

TEST_CASE("GPU mit Vulkan 1.3 ohne optionale Stufen genuegt", "[rhi][vulkan]") {
    const auto result = evaluate_device(baseline_gpu(), true);
    CHECK(result.suitable());
    CHECK(result.api_version == make_api_version(1, 3));
    CHECK_FALSE(result.features.mesh_shader);
    CHECK_FALSE(result.features.ray_tracing);
    CHECK(describe(result.features) == "keine");
}

TEST_CASE("Fehlende Pflicht-Features werden einzeln gemeldet", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    gpu.required_features[1].supported = false;
    gpu.extensions.clear();

    const auto result = evaluate_device(gpu, true);
    CHECK(result.missing == std::vector<std::string>{"Synchronization2", "VK_KHR_swapchain"});
}

TEST_CASE("Swapchain ist nur mit Fenster Pflicht", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    gpu.extensions.clear();
    CHECK(evaluate_device(gpu, false).suitable());
    CHECK_FALSE(evaluate_device(gpu, true).suitable());
}

TEST_CASE("Vulkan-Version wird auf die neueste genutzte begrenzt", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    gpu.api_version = make_api_version(1, 5);
    CHECK(evaluate_device(gpu, false).api_version == newest_api_version);
}

TEST_CASE("Optionale Stufen werden erkannt", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    add_mesh_shader(gpu);
    add_ray_tracing(gpu);

    const auto result = evaluate_device(gpu, true);
    CHECK(result.suitable());
    CHECK(result.features.mesh_shader);
    CHECK(result.features.ray_tracing);
    CHECK(describe(result.features) == "Mesh Shader, Raytracing");
}

TEST_CASE("Stufen brauchen Extension und Feature", "[rhi][vulkan]") {
    SECTION("Mesh Shader ohne Task Shader") {
        DeviceSupport gpu = baseline_gpu();
        add_mesh_shader(gpu);
        gpu.task_shader = false;
        CHECK_FALSE(evaluate_device(gpu, false).features.mesh_shader);
    }
    SECTION("Raytracing ohne Ray Query") {
        DeviceSupport gpu = baseline_gpu();
        add_ray_tracing(gpu);
        gpu.extensions.pop_back();
        CHECK_FALSE(evaluate_device(gpu, false).features.ray_tracing);
    }
}

TEST_CASE("Abgeschaltete Stufen bleiben aus", "[rhi][vulkan]") {
    DeviceSupport gpu = baseline_gpu();
    add_mesh_shader(gpu);
    add_ray_tracing(gpu);

    const auto result = evaluate_device(gpu, false, {.mesh_shader = false, .ray_tracing = true});
    CHECK(result.features.mesh_shader);
    CHECK_FALSE(result.features.ray_tracing);
}

TEST_CASE("Dedizierte GPU schlaegt integrierte mit mehr Stufen", "[rhi][vulkan]") {
    const Features all{.mesh_shader = true, .ray_tracing = true};
    CHECK(device_score(DeviceType::Discrete, {}) > device_score(DeviceType::Integrated, all));
    CHECK(device_score(DeviceType::Integrated, all) > device_score(DeviceType::Integrated, {}));
    CHECK(device_score(DeviceType::Integrated, {}) > device_score(DeviceType::Cpu, all));
}
