module;

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

module ngine.rhi.vulkan;

import ngine.core;
import ngine.rhi;

namespace ngine::rhi::vulkan {

namespace {

bool has_extension(const DeviceSupport& support, std::string_view name) {
    return std::ranges::find(support.extensions, name) != support.extensions.end();
}

std::string version_string(u32 version) {
    return std::format("{}.{}", version >> 22U, (version >> 12U) & 0x3FFU);
}

} // namespace

DeviceEvaluation evaluate_device(const DeviceSupport& support, bool needs_swapchain,
                                 const Features& disabled) {
    DeviceEvaluation result;
    if (support.api_version < minimum_api_version) {
        result.missing.push_back(std::format("Vulkan {} (hat {})",
                                             version_string(minimum_api_version),
                                             version_string(support.api_version)));
        return result;
    }
    result.api_version = std::min(support.api_version, newest_api_version);

    for (const RequiredFeature& feature : support.required_features) {
        if (!feature.supported) {
            result.missing.emplace_back(feature.name);
        }
    }
    if (needs_swapchain && !has_extension(support, swapchain_extension)) {
        result.missing.emplace_back(swapchain_extension);
    }
    if (!result.missing.empty()) {
        return result;
    }

    result.features.mesh_shader = !disabled.mesh_shader &&
                                  has_extension(support, mesh_shader_extension) &&
                                  support.task_shader && support.mesh_shader;
    result.features.ray_tracing = !disabled.ray_tracing &&
                                  has_extension(support, acceleration_structure_extension) &&
                                  has_extension(support, deferred_host_operations_extension) &&
                                  has_extension(support, ray_query_extension) &&
                                  support.acceleration_structure && support.ray_query;
    return result;
}

int device_score(DeviceType type, const Features& features) noexcept {
    int type_score = 1;
    switch (type) {
    case DeviceType::Discrete: type_score = 4; break;
    case DeviceType::Integrated: type_score = 3; break;
    case DeviceType::Virtual: type_score = 2; break;
    case DeviceType::Cpu:
    case DeviceType::Other: break;
    }
    const int feature_score = (features.mesh_shader ? 1 : 0) + (features.ray_tracing ? 1 : 0);
    // Der Typ zählt immer mehr als die Stufen: eine dedizierte GPU ohne Raytracing ist in der
    // Regel schneller als eine integrierte mit.
    return type_score * 10 + feature_score;
}

std::string describe(const Features& features) {
    std::string list;
    const auto add = [&](bool enabled, std::string_view name) {
        if (enabled) {
            list += list.empty() ? std::string{name} : std::format(", {}", name);
        }
    };
    add(features.mesh_shader, "Mesh Shader");
    add(features.ray_tracing, "Raytracing");
    return list.empty() ? std::string{"keine"} : list;
}

} // namespace ngine::rhi::vulkan
