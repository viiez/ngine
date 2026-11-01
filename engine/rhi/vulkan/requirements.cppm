module;

#include <string>
#include <string_view>
#include <vector>

// Mindestanforderung und optionale Feature-Stufen des Vulkan-Backends (ADR 0017).
// Die Auswertung arbeitet nur mit einfachen Daten statt mit Vulkan-Typen, damit sie sich
// ohne GPU testen lässt. physical_device.cpp füllt DeviceSupport aus den Vulkan-Abfragen.
export module ngine.rhi.vulkan:requirements;

import ngine.core;
import ngine.rhi;

export namespace ngine::rhi::vulkan {

// Kodiert eine Vulkan-Version wie VK_MAKE_API_VERSION (Variante 0, Patch 0).
[[nodiscard]] constexpr u32 make_api_version(u32 major, u32 minor) noexcept {
    return (major << 22U) | (minor << 12U);
}

// Ohne diese Version startet die Engine nicht.
inline constexpr u32 minimum_api_version = make_api_version(1, 3);
// Neueste Version, die NGine nutzt, wenn Treiber sie anbieten. Gebaut wird gegen die aktuellen
// Header, verlangt wird zur Laufzeit nur minimum_api_version.
inline constexpr u32 newest_api_version = make_api_version(1, 4);

// Extensions der optionalen Feature-Stufen.
inline constexpr std::string_view mesh_shader_extension = "VK_EXT_mesh_shader";
inline constexpr std::string_view acceleration_structure_extension =
    "VK_KHR_acceleration_structure";
inline constexpr std::string_view deferred_host_operations_extension =
    "VK_KHR_deferred_host_operations";
inline constexpr std::string_view ray_query_extension = "VK_KHR_ray_query";
inline constexpr std::string_view swapchain_extension = "VK_KHR_swapchain";

// Ein Pflicht-Feature und ob die GPU es kann.
struct RequiredFeature {
    std::string_view name;
    bool supported = false;
};

// Was eine GPU meldet. Die Felder der optionalen Stufen sind nur gültig, wenn die
// zugehörigen Extensions vorhanden sind.
struct DeviceSupport {
    u32 api_version = 0;
    std::vector<std::string> extensions;
    std::vector<RequiredFeature> required_features;
    bool task_shader = false;
    bool mesh_shader = false;
    bool acceleration_structure = false;
    bool ray_query = false;
};

struct DeviceEvaluation {
    // Was zur Mindestanforderung fehlt; leer, wenn die GPU genügt.
    std::vector<std::string> missing;
    // Vulkan-Version, mit der das Device erzeugt wird: höchstens newest_api_version.
    u32 api_version = 0;
    // Optionale Stufen, die die GPU kann.
    Features features;

    [[nodiscard]] bool suitable() const noexcept {
        return missing.empty();
    }
};

// Prüft eine GPU gegen die Mindestanforderung und erkennt die optionalen Stufen.
// disabled schaltet Stufen ab, auch wenn die GPU sie kann (Rückfallpfade testen).
[[nodiscard]] DeviceEvaluation evaluate_device(const DeviceSupport& support, bool needs_swapchain,
                                               const Features& disabled = {});

// Bewertung für die Auswahl: dedizierte vor integrierten GPUs, bei gleichem Typ die mit mehr
// optionalen Stufen.
[[nodiscard]] int device_score(DeviceType type, const Features& features) noexcept;

// Lesbare Liste der Stufen, z. B. "Mesh Shader, Raytracing" oder "keine".
[[nodiscard]] std::string describe(const Features& features);

} // namespace ngine::rhi::vulkan
