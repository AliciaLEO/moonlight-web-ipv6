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

#pragma once

// The Vulkan loader, opened at run time, and the functions the lab calls.
//
// Nothing links libvulkan: the engine will open libvulkan.so.1 with dlopen
// (a host without Vulkan still runs, on VA-API), and the lab does the same so
// that what it measures is what the engine will meet. Every function is
// looked up by name; the ones of an extension the driver lacks stay null, and
// the probes test them before calling.

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <string>
#include <vector>

namespace lab {

/// "VK_ERROR_NOT_PERMITTED" (or the number) — a VkResult as the spec names it.
std::string vkResultName(VkResult result);
/// "1.4.318" from a packed API version.
std::string vkVersionText(uint32_t version);
/// "G8_B8R8_2PLANE_420_UNORM" for the formats the lab meets, else the number.
std::string vkFormatName(VkFormat format);

// Before an instance exists.
#define MW_VK_GLOBAL_FUNCTIONS(X)                                                                  \
    X(vkEnumerateInstanceVersion)                                                                  \
    X(vkEnumerateInstanceExtensionProperties)                                                      \
    X(vkCreateInstance)

// Core 1.1 at least, which every driver the lab looks at has.
#define MW_VK_INSTANCE_FUNCTIONS(X)                                                                \
    X(vkDestroyInstance)                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                  \
    X(vkGetPhysicalDeviceProperties2)                                                              \
    X(vkGetPhysicalDeviceMemoryProperties)                                                         \
    X(vkEnumerateDeviceExtensionProperties)                                                        \
    X(vkGetPhysicalDeviceQueueFamilyProperties2)                                                   \
    X(vkGetPhysicalDeviceFormatProperties2)                                                        \
    X(vkGetPhysicalDeviceImageFormatProperties2)                                                   \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)                                              \
    X(vkCreateDevice)                                                                              \
    X(vkDestroyDevice)                                                                             \
    X(vkGetDeviceProcAddr)

// An extension's: null when the loader does not know them.
#define MW_VK_INSTANCE_OPTIONAL_FUNCTIONS(X)                                                       \
    X(vkGetPhysicalDeviceVideoCapabilitiesKHR)                                                     \
    X(vkGetPhysicalDeviceVideoFormatPropertiesKHR)                                                 \
    X(vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR)                                     \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)                                              \
    X(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)

#define MW_VK_DECLARE(name) PFN_##name name = nullptr;

/// libvulkan.so.1 and one instance on it.
class Vulkan
{
public:
    Vulkan() = default;
    ~Vulkan();
    Vulkan(const Vulkan&) = delete;
    Vulkan& operator=(const Vulkan&) = delete;

    /// Opens the loader and creates an instance at @p apiVersion (capped at
    /// what the loader offers). False with @p error when there is no loader,
    /// or it creates no instance.
    bool open(uint32_t apiVersion, std::string& error);

    VkInstance instance() const { return m_Instance; }
    /// What the loader offers (vkEnumerateInstanceVersion).
    uint32_t loaderVersion() const { return m_LoaderVersion; }
    /// What the instance was created at.
    uint32_t instanceVersion() const { return m_InstanceVersion; }
    /// The file dlopen resolved, from the dynamic linker's own map.
    std::string libraryPath() const;

    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    MW_VK_GLOBAL_FUNCTIONS(MW_VK_DECLARE)
    MW_VK_INSTANCE_FUNCTIONS(MW_VK_DECLARE)
    MW_VK_INSTANCE_OPTIONAL_FUNCTIONS(MW_VK_DECLARE)

private:
    void* m_Library = nullptr;
    VkInstance m_Instance = VK_NULL_HANDLE;
    uint32_t m_LoaderVersion = VK_API_VERSION_1_0;
    uint32_t m_InstanceVersion = VK_API_VERSION_1_0;
};

/// The device extensions @p device offers.
std::vector<std::string> deviceExtensions(const Vulkan& vk, VkPhysicalDevice device);
bool hasExtension(const std::vector<std::string>& extensions, const char* name);

// A device's, core 1.3: what a probe that records and submits work calls.
#define MW_VK_DEVICE_FUNCTIONS(X)                                                                  \
    X(vkGetDeviceQueue)                                                                            \
    X(vkDeviceWaitIdle)                                                                            \
    X(vkCreateCommandPool)                                                                         \
    X(vkDestroyCommandPool)                                                                        \
    X(vkAllocateCommandBuffers)                                                                    \
    X(vkResetCommandBuffer)                                                                        \
    X(vkBeginCommandBuffer)                                                                        \
    X(vkEndCommandBuffer)                                                                          \
    X(vkQueueSubmit2)                                                                              \
    X(vkCreateSemaphore)                                                                           \
    X(vkDestroySemaphore)                                                                          \
    X(vkWaitSemaphores)                                                                            \
    X(vkCreateBuffer)                                                                              \
    X(vkDestroyBuffer)                                                                             \
    X(vkGetBufferMemoryRequirements)                                                               \
    X(vkBindBufferMemory)                                                                          \
    X(vkCreateImage)                                                                               \
    X(vkDestroyImage)                                                                              \
    X(vkGetImageMemoryRequirements)                                                                \
    X(vkBindImageMemory)                                                                           \
    X(vkCreateImageView)                                                                           \
    X(vkDestroyImageView)                                                                          \
    X(vkAllocateMemory)                                                                            \
    X(vkFreeMemory)                                                                                \
    X(vkMapMemory)                                                                                 \
    X(vkUnmapMemory)                                                                               \
    X(vkCmdPipelineBarrier2)                                                                       \
    X(vkCmdCopyBufferToImage)                                                                      \
    X(vkCreateQueryPool)                                                                           \
    X(vkDestroyQueryPool)                                                                          \
    X(vkGetQueryPoolResults)                                                                       \
    X(vkCmdResetQueryPool)                                                                         \
    X(vkCmdBeginQuery)                                                                             \
    X(vkCmdEndQuery)                                                                               \
    X(vkCmdWriteTimestamp2)                                                                        \
    X(vkCreateShaderModule)                                                                        \
    X(vkDestroyShaderModule)                                                                       \
    X(vkCreateDescriptorSetLayout)                                                                 \
    X(vkDestroyDescriptorSetLayout)                                                                \
    X(vkCreatePipelineLayout)                                                                      \
    X(vkDestroyPipelineLayout)                                                                     \
    X(vkCreateComputePipelines)                                                                    \
    X(vkDestroyPipeline)                                                                           \
    X(vkCreateDescriptorPool)                                                                      \
    X(vkDestroyDescriptorPool)                                                                     \
    X(vkAllocateDescriptorSets)                                                                    \
    X(vkUpdateDescriptorSets)                                                                      \
    X(vkCreateSampler)                                                                             \
    X(vkDestroySampler)                                                                            \
    X(vkCmdBindPipeline)                                                                           \
    X(vkCmdBindDescriptorSets)                                                                     \
    X(vkCmdPushConstants)                                                                          \
    X(vkCmdDispatch)

// VK_KHR_video_queue and VK_KHR_video_encode_queue.
#define MW_VK_VIDEO_ENCODE_FUNCTIONS(X)                                                            \
    X(vkCreateVideoSessionKHR)                                                                     \
    X(vkDestroyVideoSessionKHR)                                                                    \
    X(vkGetVideoSessionMemoryRequirementsKHR)                                                      \
    X(vkBindVideoSessionMemoryKHR)                                                                 \
    X(vkCreateVideoSessionParametersKHR)                                                           \
    X(vkDestroyVideoSessionParametersKHR)                                                          \
    X(vkGetEncodedVideoSessionParametersKHR)                                                       \
    X(vkCmdBeginVideoCodingKHR)                                                                    \
    X(vkCmdEndVideoCodingKHR)                                                                      \
    X(vkCmdControlVideoCodingKHR)                                                                  \
    X(vkCmdEncodeVideoKHR)

/// A device's functions, through vkGetDeviceProcAddr.
struct DeviceFunctions
{
    MW_VK_DEVICE_FUNCTIONS(MW_VK_DECLARE)
    MW_VK_VIDEO_ENCODE_FUNCTIONS(MW_VK_DECLARE)

    /// Loads them for @p device, the video encode ones when @p video. False
    /// with the first one missing in @p missing.
    bool load(const Vulkan& vk, VkDevice device, bool video, std::string& missing);
};

/// The index of a memory type of @p mem among @p bits with every property of
/// @p want, or UINT32_MAX.
uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& mem, uint32_t bits,
                    VkMemoryPropertyFlags want);

/// A device and everything a probe makes on it, destroyed in reverse before
/// the device itself — whichever way the probe leaves.
struct DeviceObjects
{
    const Vulkan* vk = nullptr;
    DeviceFunctions fn;
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> memories;
    std::vector<VkBuffer> buffers;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSampler> samplers;
    std::vector<VkSemaphore> semaphores;
    std::vector<VkCommandPool> pools;
    std::vector<VkQueryPool> queryPools;
    std::vector<VkShaderModule> shaders;
    std::vector<VkDescriptorSetLayout> setLayouts;
    std::vector<VkPipelineLayout> pipelineLayouts;
    std::vector<VkPipeline> pipelines;
    std::vector<VkDescriptorPool> descriptorPools;
    VkVideoSessionKHR session = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR parameters = VK_NULL_HANDLE;

    DeviceObjects() = default;
    DeviceObjects(const DeviceObjects&) = delete;
    DeviceObjects& operator=(const DeviceObjects&) = delete;
    ~DeviceObjects();

    /// Memory for @p req, of a type with the @p want properties, else with
    /// the @p fallback ones; kept for the destructor.
    VkResult allocate(const VkPhysicalDeviceMemoryProperties& mem, const VkMemoryRequirements& req,
                      VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback,
                      VkDeviceMemory& out);
};

/// @p s put at the head of the pNext chain @p head points to.
template <typename T> void pushNext(void*& head, T& s)
{
    s.pNext = head;
    head = &s;
}

/// A video encode profile as the queries and the session take it: the generic
/// part, the codec's profile, and the usage — streaming a desktop or a game at
/// ultra-low latency, which is what the engine will declare. Built in place:
/// the structures point at each other.
struct EncodeProfileChain
{
    VkVideoEncodeUsageInfoKHR usage = {};
    VkVideoEncodeH264ProfileInfoKHR h264 = {};
    VkVideoEncodeH265ProfileInfoKHR h265 = {};
    VkVideoEncodeAV1ProfileInfoKHR av1 = {};
    VkVideoProfileInfoKHR info = {};

    EncodeProfileChain(VkVideoCodecOperationFlagBitsKHR op, int stdProfile,
                       VkVideoComponentBitDepthFlagsKHR depth);
    EncodeProfileChain(const EncodeProfileChain&) = delete;
    EncodeProfileChain& operator=(const EncodeProfileChain&) = delete;
};

} // namespace lab
