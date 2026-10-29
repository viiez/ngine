// Gemeinsame Vulkan-Includes des Backends. Nur im globalen Modulfragment einbinden.
#pragma once

#if NGINE_VULKAN_USE_VOLK
#include <volk.h>
#else
#include <vulkan/vulkan.h>
#endif

#include <vk_mem_alloc.h>
