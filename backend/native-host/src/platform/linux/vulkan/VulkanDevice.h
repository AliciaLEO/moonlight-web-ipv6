/*
 * MoonlightWeb — native capture & encoding engine.
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

// The Vulkan side of the Linux chain (plan pipeline-video-d3d12-v2, Phase 13):
// the loader, the device of the GPU the display scans out of, and the one
// compute queue the conversion runs on.
//
// ── Why a compute queue ─────────────────────────────────────────────────────
//
// Under a game that saturates the GPU, work on the graphics ring waits for
// the game's frame in flight and, at the game's priority, for the next one:
// the GL conversion of today waits 46 ms on a Radeon 780M. A compute queue
// runs beside the game on the units it leaves: 10.6 ms without any privilege,
// 8.0 ms at HIGH (docs/bench-native-host.md §8o.1).
//
// ── Why the priority is proven by a submission ──────────────────────────────
//
// A device created with a queue at HIGH says nothing yet: on the 780M under
// Linux 6.8 an encode queue at HIGH is created, and the kernel refuses its
// first submission (§8o.3). So HIGH is kept only once an empty submission has
// gone through and come back; anything else steps down to the default, and
// the description says why. HIGH is asked for only when the process holds
// CAP_SYS_NICE — amdgpu, i915 and xe grant it to nobody else — raised on this
// thread for the creation (platform/linux/ScopedCapability.h).
//
// ── Why dlopen ──────────────────────────────────────────────────────────────
//
// A host without Vulkan still runs, converting through GL: nothing links
// libvulkan. The loader (Apache-2.0) is opened at run time and every function
// looked up by name; the headers are the module's own (third_party/
// vulkan-headers, Apache-2.0 OR MIT, see LICENSE.md).

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>
#include <string>

namespace mw::native::vulkan {

/// "VK_ERROR_DEVICE_LOST", or the number: how a VkResult is logged here.
std::string resultText(VkResult result);

/// How long the conversion's CPU waits on its own GPU work before it calls the
/// device gone — the D3D12 chain's rule (d3d12::kGpuGoneMs): past what a
/// busy GPU takes under a game, short of a stream that freezes silently.
constexpr uint64_t kGpuGoneMs = 3000;

// The device's functions the conversion calls, core 1.3.
#define MW_VULKAN_DEVICE_FUNCTIONS(X)                                                              \
    X(vkDestroyDevice)                                                                             \
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
    X(vkCmdDispatch)                                                                               \
    X(vkGetMemoryFdPropertiesKHR)

// An extension's, null where the device lacks it.
#define MW_VULKAN_OPTIONAL_DEVICE_FUNCTIONS(X) X(vkImportSemaphoreFdKHR)

#define MW_VULKAN_DECLARE(name) PFN_##name name = nullptr;

struct DeviceFunctions
{
    MW_VULKAN_DEVICE_FUNCTIONS(MW_VULKAN_DECLARE)
    MW_VULKAN_OPTIONAL_DEVICE_FUNCTIONS(MW_VULKAN_DECLARE)
};

class Loader;

/// The Vulkan device of the GPU behind one DRM render node, with the one
/// compute queue the conversion runs on.
class VulkanDevice
{
public:
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    /// The device of the GPU behind @p renderNode ("/dev/dri/renderD128"),
    /// matched by its DRM node numbers (VK_EXT_physical_device_drm), with a
    /// compute queue at HIGH when @p wantHigh, the process holds CAP_SYS_NICE
    /// and a submission proves it — the default priority otherwise. Null, with
    /// the reason, when there is no loader, no Vulkan 1.3 device on that GPU,
    /// or it lacks what importing a DMA-BUF and writing into one need.
    static std::unique_ptr<VulkanDevice> open(const std::string& renderNode, bool wantHigh,
                                              std::string& error);

    const DeviceFunctions& fn() const { return m_Fn; }
    VkDevice device() const { return m_Device; }
    VkPhysicalDevice physical() const { return m_Physical; }
    VkQueue queue() const { return m_Queue; }
    uint32_t family() const { return m_Family; }

    /// "AMD Radeon 780M Graphics (RADV PHOENIX), Mesa 23.2.1": the log's name.
    const std::string& name() const { return m_Name; }
    /// The queue as obtained: "a compute queue, priority high (CAP_SYS_NICE)",
    /// or normal and why.
    const std::string& queueDescription() const { return m_QueueDescription; }
    bool highPriority() const { return m_High; }

    /// Whether a submission's wait may use a sync_file (the DMA-BUF's
    /// implicit fence, exported by the kernel): VK_KHR_external_semaphore_fd
    /// and a driver that imports SYNC_FD into a binary semaphore.
    bool importsSyncFile() const { return m_SyncFile; }

    /// Nanoseconds per timestamp tick, and whether the queue has timestamps.
    float timestampPeriod() const { return m_TimestampPeriod; }
    bool timestamps() const { return m_Timestamps; }

    /// A memory type among @p bits with every property of @p want, or
    /// UINT32_MAX.
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) const;

    /// What @p format at the DRM @p modifier can do on this device, and how
    /// many memory planes it takes; false when the driver does not list the
    /// modifier for the format at all.
    bool modifierFeatures(VkFormat format, uint64_t modifier, VkFormatFeatureFlags2& features,
                          uint32_t& planes) const;

    /// Submit @p cmd on the queue, waiting on @p waits first, and wait for it
    /// on the CPU — the hand-off the next API needs (GlConvert's glFinish).
    /// Gives up after kGpuGoneMs. Any failure leaves the device lost for good:
    /// lost() then says so and nothing more is submitted.
    bool run(VkCommandBuffer cmd, const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
             std::string& error);
    bool lost() const { return m_Lost; }

private:
    VulkanDevice() = default;
    bool create(bool high, std::string& error);
    void destroy();

    std::shared_ptr<Loader> m_Loader;
    DeviceFunctions m_Fn;
    VkPhysicalDevice m_Physical = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_Queue = VK_NULL_HANDLE;
    uint32_t m_Family = UINT32_MAX;
    bool m_AsyncCompute = false;
    bool m_PriorityExtension = false;
    bool m_High = false;
    bool m_SyncFile = false;
    bool m_Timestamps = false;
    float m_TimestampPeriod = 1.0f;
    VkSemaphore m_Timeline = VK_NULL_HANDLE;
    uint64_t m_Submitted = 0;
    bool m_Lost = false;
    std::string m_Name;
    std::string m_QueueDescription;
    VkPhysicalDeviceMemoryProperties m_Memory = {};
};

} // namespace mw::native::vulkan
