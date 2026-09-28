/*
 * MoonlightWeb — native capture & encoding engine: Vulkan lab.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "Vk.h"

#include <dlfcn.h>
#include <link.h>

#include <algorithm>
#include <cstdio>

namespace lab {

std::string vkResultName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_NOT_PERMITTED: return "VK_ERROR_NOT_PERMITTED";
    case VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR: return "VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED";
    case VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR:
        return "VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED";
    case VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR: return "VK_ERROR_INVALID_VIDEO_STD_PARAMETERS";
    default: break;
    }
    return "VkResult " + std::to_string(static_cast<int>(result));
}

std::string vkVersionText(uint32_t version)
{
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

std::string vkFormatName(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "G8_B8R8_2PLANE_420_UNORM (NV12)";
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
        return "G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 (P010)";
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return "G8_B8_R8_3PLANE_420_UNORM (I420)";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_UNORM_PACK32";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM_PACK32";
    case VK_FORMAT_R8_UNORM: return "R8_UNORM";
    case VK_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case VK_FORMAT_R16_UNORM: return "R16_UNORM";
    case VK_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    default: break;
    }
    return "VkFormat " + std::to_string(static_cast<int>(format));
}

Vulkan::~Vulkan()
{
    if (m_Instance && vkDestroyInstance) vkDestroyInstance(m_Instance, nullptr);
    if (m_Library) ::dlclose(m_Library);
}

bool Vulkan::open(uint32_t apiVersion, std::string& error)
{
    m_Library = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!m_Library) {
        const char* why = ::dlerror();
        error = std::string("no Vulkan loader (") + (why ? why : "dlopen failed") + ")";
        return false;
    }
    vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(m_Library, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        error = "the loader has no vkGetInstanceProcAddr";
        return false;
    }
#define MW_VK_LOAD_GLOBAL(name)                                                                    \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE, #name));
    MW_VK_GLOBAL_FUNCTIONS(MW_VK_LOAD_GLOBAL)
#undef MW_VK_LOAD_GLOBAL
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) {
        error = "the loader does not hand out vkCreateInstance";
        return false;
    }
    // A 1.0 loader has no vkEnumerateInstanceVersion; nothing here runs on one.
    m_LoaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&m_LoaderVersion);
    if (m_LoaderVersion < VK_API_VERSION_1_1) {
        error = "loader " + vkVersionText(m_LoaderVersion) + " — the lab needs Vulkan 1.1";
        return false;
    }
    m_InstanceVersion = std::min(apiVersion, m_LoaderVersion);

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "mw-vk-lab";
    app.applicationVersion = 1;
    app.pEngineName = "MoonlightWeb";
    app.engineVersion = 1;
    app.apiVersion = m_InstanceVersion;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    const VkResult created = vkCreateInstance(&info, nullptr, &m_Instance);
    if (created != VK_SUCCESS) {
        m_Instance = VK_NULL_HANDLE;
        error = "vkCreateInstance: " + vkResultName(created);
        return false;
    }
#define MW_VK_LOAD_INSTANCE(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(m_Instance, #name));
    MW_VK_INSTANCE_FUNCTIONS(MW_VK_LOAD_INSTANCE)
    MW_VK_INSTANCE_OPTIONAL_FUNCTIONS(MW_VK_LOAD_INSTANCE)
#undef MW_VK_LOAD_INSTANCE
#define MW_VK_CHECK(name)                                                                          \
    if (!name) {                                                                                   \
        error = "the loader does not hand out " #name;                                             \
        return false;                                                                              \
    }
    MW_VK_INSTANCE_FUNCTIONS(MW_VK_CHECK)
#undef MW_VK_CHECK
    return true;
}

std::string Vulkan::libraryPath() const
{
    if (!m_Library) return "";
    struct link_map* map = nullptr;
    if (::dlinfo(m_Library, RTLD_DI_LINKMAP, &map) != 0 || !map || !map->l_name) return "";
    return map->l_name;
}

std::vector<std::string> deviceExtensions(const Vulkan& vk, VkPhysicalDevice device)
{
    uint32_t count = 0;
    vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    std::vector<std::string> names;
    names.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        names.emplace_back(props[i].extensionName);
    std::sort(names.begin(), names.end());
    return names;
}

bool hasExtension(const std::vector<std::string>& extensions, const char* name)
{
    return std::binary_search(extensions.begin(), extensions.end(), std::string(name));
}

} // namespace lab
