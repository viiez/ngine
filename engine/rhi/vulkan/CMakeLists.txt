add_library(ngine_rhi_vulkan)
add_library(ngine::rhi_vulkan ALIAS ngine_rhi_vulkan)

target_sources(ngine_rhi_vulkan
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
            vulkan.cppm
    # Interne Partitionen: Teil des Moduls, aber außerhalb nicht sichtbar.
    PRIVATE
        FILE_SET internal_partitions TYPE CXX_MODULES
        FILES
            common.cppm
            instance.cppm
            physical_device.cppm
            resources.cppm
            command_list.cppm
            swapchain.cppm
    PRIVATE
        device.cpp
        instance.cpp
        physical_device.cpp
        resources.cpp
        swapchain.cpp
        vma.cpp)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# volk lädt die Vulkan-Funktionen direkt vom Treiber (vcpkg). Fehlt volk, etwa bei
# Paketen einer Linux-Distribution, linken wir direkt gegen den Vulkan-Loader.
find_package(volk CONFIG QUIET)
if(volk_FOUND)
    target_link_libraries(ngine_rhi_vulkan PRIVATE volk::volk volk::volk_headers)
    target_compile_definitions(ngine_rhi_vulkan PRIVATE NGINE_VULKAN_USE_VOLK=1)
else()
    message(STATUS "NGine: volk nicht gefunden, nutze den Vulkan-Loader direkt")
    find_package(Vulkan REQUIRED)
    target_link_libraries(ngine_rhi_vulkan PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ngine_rhi_vulkan PRIVATE NGINE_VULKAN_USE_VOLK=0)
endif()

target_compile_definitions(ngine_rhi_vulkan PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1)

target_link_libraries(ngine_rhi_vulkan
    PUBLIC ngine::rhi
    PRIVATE ngine::core GPUOpen::VulkanMemoryAllocator)
ngine_set_compiler_options(ngine_rhi_vulkan)
