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

// `queues` — where the conversion waits when a game holds the GPU (plan
// pipeline-video-d3d12-v2, Phase 13, C13.1; the D3D12 lab's queues, C1.1).
//
// The Linux chain converts in GL today, with no say in the GPU's scheduling:
// behind a game, its passes wait in the same line as everyone's. A Vulkan
// queue can ask for a global priority (VK_KHR_global_priority): up to MEDIUM
// for anyone, HIGH and REALTIME for a process holding CAP_SYS_NICE on amdgpu —
// what the launcher would hand the host (§9-9 of the plan). On Windows, the
// same question decided G1: the priority class was what counted.
//
// The probe submits the cost of a real conversion — a Lanczos-2 resample of a
// 1440p picture into a 1080p luma plane and its chroma plane — at a steady
// rate, on a compute or a graphics queue, at each priority in turn, and reads
// the wall time (submit to done) against the GPU time (timestamps): what is
// left is waiting. The load is someone else's: mw-gpu-load, or a game, on the
// same GPU at the same time. At rest the priorities should not differ; under
// load they are the question.

#include "Queues.h"

#include "Json.h"
#include "Lab.h"
#include "Vk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// glslangValidator's output: the SPIR-V of shaders/convert.comp, as kConvertSpv.
#include "ConvertSpv.h"

namespace lab {
namespace {

struct Options
{
    std::string device;
    uint32_t sourceW = 2560, sourceH = 1440;
    uint32_t targetW = 1920, targetH = 1080;
    int fps = 60;
    int seconds = 8;
    std::vector<std::string> priorities = {"low", "medium", "high", "realtime"};
    std::string queue = "compute";
    std::string json, csv;
};

struct Result
{
    std::string priority;
    VkResult created = VK_SUCCESS;
    Stats wall, gpu, wait;
    size_t frames = 0;
};

#define VKTRY(call, what)                                                                          \
    do {                                                                                           \
        const VkResult result_ = (call);                                                           \
        if (result_ != VK_SUCCESS) {                                                               \
            say("mw-vk-lab queues: %s: %s\n", what, vkResultName(result_).c_str());                \
            return result_;                                                                        \
        }                                                                                          \
    } while (0)

VkQueueGlobalPriority priorityOf(const std::string& name)
{
    if (name == "low") return VK_QUEUE_GLOBAL_PRIORITY_LOW;
    if (name == "high") return VK_QUEUE_GLOBAL_PRIORITY_HIGH;
    if (name == "realtime") return VK_QUEUE_GLOBAL_PRIORITY_REALTIME;
    return VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--device") {
            if (!next(o.device)) return false;
        } else if (a == "--source") {
            if (!next(v) || std::sscanf(v.c_str(), "%ux%u", &o.sourceW, &o.sourceH) != 2)
                return false;
        } else if (a == "--target") {
            if (!next(v) || std::sscanf(v.c_str(), "%ux%u", &o.targetW, &o.targetH) != 2)
                return false;
        } else if (a == "--fps") {
            if (!next(v)) return false;
            o.fps = std::stoi(v);
        } else if (a == "--seconds") {
            if (!next(v)) return false;
            o.seconds = std::stoi(v);
        } else if (a == "--priorities") {
            if (!next(v)) return false;
            o.priorities.clear();
            std::stringstream list(v);
            std::string item;
            while (std::getline(list, item, ','))
                if (!item.empty()) o.priorities.push_back(item);
        } else if (a == "--queue") {
            if (!next(o.queue)) return false;
        } else if (a == "--json") {
            if (!next(o.json)) return false;
        } else if (a == "--csv") {
            if (!next(o.csv)) return false;
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    for (const std::string& p : o.priorities)
        if (p != "low" && p != "medium" && p != "high" && p != "realtime") return false;
    return (o.queue == "compute" || o.queue == "graphics") && o.fps > 0 && o.seconds > 1 &&
           !o.priorities.empty() && o.targetW % 2 == 0 && o.targetH % 2 == 0;
}

/// The picture converted: a ramp, fine stripes and blocks, RGBA.
std::vector<uint8_t> makeSource(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            const bool stripe = y % 90 < 30 && (x / 2) % 2;
            const bool block = (x / 16 + y / 16) % 7 == 0;
            p[0] = static_cast<uint8_t>(stripe ? 230 : (x * 255) / w);
            p[1] = static_cast<uint8_t>(stripe ? 230 : (y * 255) / h);
            p[2] = static_cast<uint8_t>(block ? 40 : 160);
            p[3] = 255;
        }
    }
    return rgba;
}

/// One priority: a device with one queue at it, the conversion submitted at
/// the rate asked, the times kept after the first second.
VkResult runOne(const Vulkan& vk, VkPhysicalDevice pd, const std::vector<std::string>& extensions,
                uint32_t family, const std::string& priority, const Options& o, double period,
                Result& out, std::ofstream& csv)
{
    out.priority = priority;
    DeviceObjects own;
    own.vk = &vk;
    const char* priorityExt = hasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)
                                  ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME
                                  : VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME;
    VkDeviceQueueGlobalPriorityCreateInfo prio = {};
    prio.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO;
    prio.globalPriority = priorityOf(priority);
    const float one = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.pNext = &prio;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &one;
    VkPhysicalDeviceVulkan13Features f13 = {};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queue;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &priorityExt;
    out.created = vk.vkCreateDevice(pd, &dci, nullptr, &own.device);
    if (out.created != VK_SUCCESS) {
        own.device = VK_NULL_HANDLE;
        return out.created;
    }
    std::string missing;
    if (!own.fn.load(vk, own.device, false, missing)) {
        say("mw-vk-lab queues: the device has no %s\n", missing.c_str());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    DeviceFunctions& fn = own.fn;
    VkDevice device = own.device;
    VkQueue q = VK_NULL_HANDLE;
    fn.vkGetDeviceQueue(device, family, 0, &q);
    VkPhysicalDeviceMemoryProperties mem = {};
    vk.vkGetPhysicalDeviceMemoryProperties(pd, &mem);

    // ── The pictures: the source, the luma and chroma planes ──
    struct Picture
    {
        VkFormat format;
        uint32_t w, h;
        VkImageUsageFlags usage;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    Picture pictures[3] = {
        {VK_FORMAT_R8G8B8A8_UNORM, o.sourceW, o.sourceH,
         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
        {VK_FORMAT_R8_UNORM, o.targetW, o.targetH, VK_IMAGE_USAGE_STORAGE_BIT},
        {VK_FORMAT_R8G8_UNORM, o.targetW / 2, o.targetH / 2, VK_IMAGE_USAGE_STORAGE_BIT},
    };
    for (Picture& p : pictures) {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = p.format;
        ici.extent = {p.w, p.h, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = p.usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKTRY(fn.vkCreateImage(device, &ici, nullptr, &p.image), "image");
        own.images.push_back(p.image);
        VkMemoryRequirements req = {};
        fn.vkGetImageMemoryRequirements(device, p.image, &req);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VKTRY(own.allocate(mem, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, memory),
              "image memory");
        VKTRY(fn.vkBindImageMemory(device, p.image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = p.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = p.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKTRY(fn.vkCreateImageView(device, &vci, nullptr, &p.view), "image view");
        own.views.push_back(p.view);
    }

    // The source goes up once.
    const std::vector<uint8_t> source = makeSource(o.sourceW, o.sourceH);
    VkBuffer staging = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = source.size();
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKTRY(fn.vkCreateBuffer(device, &bci, nullptr, &staging), "staging buffer");
        own.buffers.push_back(staging);
        VkMemoryRequirements req = {};
        fn.vkGetBufferMemoryRequirements(device, staging, &req);
        const VkMemoryPropertyFlags visible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VKTRY(own.allocate(mem, req, visible, visible, memory), "staging memory");
        VKTRY(fn.vkBindBufferMemory(device, staging, memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        VKTRY(fn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        std::memcpy(mapped, source.data(), source.size());
    }

    // ── The pipeline ──
    VkSampler sampler = VK_NULL_HANDLE;
    {
        VkSamplerCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKTRY(fn.vkCreateSampler(device, &sci, nullptr, &sampler), "sampler");
        own.samplers.push_back(sampler);
    }
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (uint32_t b = 0; b < 3; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci = {};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 3;
        lci.pBindings = bindings;
        VKTRY(fn.vkCreateDescriptorSetLayout(device, &lci, nullptr, &setLayout), "set layout");
        own.setLayouts.push_back(setLayout);
    }
    struct Push
    {
        float sourceW, sourceH, targetW, targetH;
        uint32_t pass;
    };
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    {
        VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &setLayout;
        pci.pushConstantRangeCount = 1;
        pci.pPushConstantRanges = &range;
        VKTRY(fn.vkCreatePipelineLayout(device, &pci, nullptr, &pipelineLayout), "pipeline layout");
        own.pipelineLayouts.push_back(pipelineLayout);
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo mci = {};
        mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mci.codeSize = sizeof(kConvertSpv);
        mci.pCode = kConvertSpv;
        VkShaderModule module = VK_NULL_HANDLE;
        VKTRY(fn.vkCreateShaderModule(device, &mci, nullptr, &module), "shader module");
        own.shaders.push_back(module);
        VkComputePipelineCreateInfo cpi = {};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = pipelineLayout;
        VKTRY(fn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline),
              "compute pipeline");
        own.pipelines.push_back(pipeline);
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo dpi = {};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VKTRY(fn.vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "descriptor pool");
        own.descriptorPools.push_back(pool);
        VkDescriptorSetAllocateInfo dai = {};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &setLayout;
        VKTRY(fn.vkAllocateDescriptorSets(device, &dai, &set), "descriptor set");
        VkDescriptorImageInfo images[3] = {
            {sampler, pictures[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, pictures[1].view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, pictures[2].view, VK_IMAGE_LAYOUT_GENERAL},
        };
        VkWriteDescriptorSet writes[3] = {};
        for (uint32_t b = 0; b < 3; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[b].pImageInfo = &images[b];
        }
        fn.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    // ── Commands: the upload once, then the conversion recorded once ──
    VkCommandPool pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = family;
        VKTRY(fn.vkCreateCommandPool(device, &pci, nullptr, &pool), "command pool");
        own.pools.push_back(pool);
    }
    VkCommandBuffer cmds[2] = {};
    {
        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 2;
        VKTRY(fn.vkAllocateCommandBuffers(device, &cai, cmds), "command buffers");
    }
    VkQueryPool timestamps = VK_NULL_HANDLE;
    {
        VkQueryPoolCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        VKTRY(fn.vkCreateQueryPool(device, &qci, nullptr, &timestamps), "timestamp pool");
        own.queryPools.push_back(timestamps);
    }
    VkSemaphore done = VK_NULL_HANDLE;
    {
        VkSemaphoreTypeCreateInfo kind = {};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &kind;
        VKTRY(fn.vkCreateSemaphore(device, &sci, nullptr, &done), "timeline semaphore");
        own.semaphores.push_back(done);
    }

    VkCommandBufferBeginInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    auto barrier = [&](VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        fn.vkCmdPipelineBarrier2(cmd, &dep);
    };

    fn.vkBeginCommandBuffer(cmds[0], &cbi);
    barrier(cmds[0], pictures[0].image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {o.sourceW, o.sourceH, 1};
    fn.vkCmdCopyBufferToImage(cmds[0], staging, pictures[0].image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmds[0], pictures[0].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (int p = 1; p < 3; ++p)
        barrier(cmds[0], pictures[p].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    fn.vkEndCommandBuffer(cmds[0]);

    // The conversion, the same every frame: recorded once, submitted again.
    fn.vkBeginCommandBuffer(cmds[1], &cbi);
    fn.vkCmdResetQueryPool(cmds[1], timestamps, 0, 2);
    for (int p = 1; p < 3; ++p)
        barrier(cmds[1], pictures[p].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    fn.vkCmdWriteTimestamp2(cmds[1], VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, 0);
    fn.vkCmdBindPipeline(cmds[1], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    fn.vkCmdBindDescriptorSets(cmds[1], VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set,
                               0, nullptr);
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const Push push = {static_cast<float>(o.sourceW), static_cast<float>(o.sourceH),
                           static_cast<float>(o.targetW), static_cast<float>(o.targetH), pass};
        fn.vkCmdPushConstants(cmds[1], pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                              &push);
        const uint32_t w = pass == 0 ? o.targetW : o.targetW / 2;
        const uint32_t h = pass == 0 ? o.targetH : o.targetH / 2;
        fn.vkCmdDispatch(cmds[1], (w + 15) / 16, (h + 15) / 16, 1);
    }
    fn.vkCmdWriteTimestamp2(cmds[1], VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, timestamps, 1);
    fn.vkEndCommandBuffer(cmds[1]);

    // ── The run ──
    std::vector<double> wall, gpu, wait;
    const int frames = o.fps * o.seconds;
    const int64_t start = nowUs() + 20000;
    for (int n = 0; n <= frames; ++n) {
        // Submission 0 is the upload; the conversions follow at the rate asked.
        if (n > 0) sleepUntilUs(start + static_cast<int64_t>(n - 1) * 1000000 / o.fps);
        const uint64_t value = static_cast<uint64_t>(n) + 1;
        VkCommandBufferSubmitInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmds[n == 0 ? 0 : 1];
        VkSemaphoreSubmitInfo signal = {};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = done;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        const int64_t t0 = nowUs();
        VKTRY(fn.vkQueueSubmit2(q, 1, &submit, VK_NULL_HANDLE), "submit");
        VkSemaphoreWaitInfo wi = {};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &done;
        wi.pValues = &value;
        VKTRY(fn.vkWaitSemaphores(device, &wi, 2000000000ull), "wait");
        const double wallMs = static_cast<double>(nowUs() - t0) / 1000.0;
        if (n == 0) continue;
        uint64_t ts[2] = {};
        VKTRY(fn.vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "timestamps");
        const double gpuMs = static_cast<double>(ts[1] - ts[0]) * period / 1e6;
        if (csv.is_open())
            csv << priority << ',' << n << ',' << wallMs << ',' << gpuMs << ','
                << std::max(0.0, wallMs - gpuMs) << '\n';
        if (n <= o.fps) continue; // the first second settles the clocks
        wall.push_back(wallMs);
        gpu.push_back(gpuMs);
        wait.push_back(std::max(0.0, wallMs - gpuMs));
    }
    out.wall = stats(wall);
    out.gpu = stats(gpu);
    out.wait = stats(wait);
    out.frames = wall.size();
    return VK_SUCCESS;
}

} // namespace

void queuesUsage()
{
    say("mw-vk-lab queues [--device <index|name>] [--queue compute|graphics]\n"
        "                 [--priorities low,medium,high,realtime] [--fps 60] [--seconds 8]\n"
        "                 [--source 2560x1440] [--target 1920x1080] [--csv x.csv] [--json x.json]\n"
        "  The cost of a conversion (Lanczos-2, source into a luma and a chroma plane at the\n"
        "  target size) submitted at a steady rate on one queue, at each priority in turn:\n"
        "  wall time (submit to done), GPU time (timestamps), and the difference, waiting.\n"
        "  Run it beside a load on the same GPU (mw-gpu-load, a game): at rest the\n"
        "  priorities should not differ. HIGH and REALTIME need CAP_SYS_NICE (amdgpu).\n");
}

int runQueues(int argc, char** argv)
{
    Options o;
    try {
        if (!parse(argc, argv, o)) {
            queuesUsage();
            return 2;
        }
    } catch (...) {
        queuesUsage();
        return 2;
    }
    Vulkan vk;
    std::string error;
    if (!vk.open(VK_API_VERSION_1_3, error)) {
        say("mw-vk-lab queues: %s\n", error.c_str());
        return 1;
    }
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, devices.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < count && !pd; ++i) {
        VkPhysicalDeviceProperties2 p2 = {};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        vk.vkGetPhysicalDeviceProperties2(devices[i], &p2);
        if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (!matchesDevice(o.device, i, p2.properties.deviceName)) continue;
        pd = devices[i];
        props = p2.properties;
        extensions = deviceExtensions(vk, pd);
    }
    if (!pd) {
        say("mw-vk-lab queues: no GPU matches \"%s\"\n", o.device.c_str());
        return 1;
    }
    if (!hasExtension(extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) &&
        !hasExtension(extensions, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME)) {
        say("mw-vk-lab queues: %s has no global priority extension\n", props.deviceName);
        return 1;
    }
    uint32_t familyCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties2> families(familyCount);
    for (auto& f : families) {
        f = {};
        f.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    }
    vk.vkGetPhysicalDeviceQueueFamilyProperties2(pd, &familyCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; ++i) {
        const VkQueueFlags f = families[i].queueFamilyProperties.queueFlags;
        const bool compute = (f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT);
        if ((o.queue == "compute" && compute) ||
            (o.queue == "graphics" && (f & VK_QUEUE_GRAPHICS_BIT)))
            family = i;
    }
    if (family == UINT32_MAX) {
        say("mw-vk-lab queues: no %s queue family\n", o.queue.c_str());
        return 1;
    }
    if (!families[family].queueFamilyProperties.timestampValidBits) {
        say("mw-vk-lab queues: the %s family has no timestamps\n", o.queue.c_str());
        return 1;
    }

    say("mw-vk-lab queues — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  %s, %s queue (family %u), %ux%u -> %ux%u Lanczos-2, %d i/s, %d s a priority; "
        "CAP_SYS_NICE %s\n",
        props.deviceName, o.queue.c_str(), family, o.sourceW, o.sourceH, o.targetW, o.targetH,
        o.fps, o.seconds, capEffective(kCapSysNice) ? "effective" : "not effective");

    std::ofstream csv;
    if (!o.csv.empty()) {
        csv.open(o.csv);
        csv << "priority,frame,wall_ms,gpu_ms,wait_ms\n";
    }
    std::vector<Result> results;
    const double period = static_cast<double>(props.limits.timestampPeriod);
    for (const std::string& priority : o.priorities) {
        Result r;
        // The clock time of each run, to set against the load's own log: a
        // queue starved at LOW holds the device's teardown until its work
        // gets through, which can outlast the load.
        const std::string started = nowText();
        const VkResult ran = runOne(vk, pd, extensions, family, priority, o, period, r, csv);
        if (ran != VK_SUCCESS) {
            say("  %-8s %s: %s\n", priority.c_str(), started.c_str() + 11,
                vkResultName(ran).c_str());
            r.created = ran;
        } else {
            say("  %-8s %s: wall %.2f / p50 %.2f / p99 %.2f / max %.2f ms, GPU %.2f / p99 %.2f "
                "ms, waiting %.2f / p99 %.2f ms (%zu frames)\n",
                priority.c_str(), started.c_str() + 11, r.wall.mean, r.wall.p50, r.wall.p99,
                r.wall.max, r.gpu.mean, r.gpu.p99, r.wait.mean, r.wait.p99, r.frames);
        }
        results.push_back(r);
    }

    if (!o.json.empty()) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-vk-lab queues");
        j.field("date", nowText());
        j.field("host", hostName());
        j.field("device", props.deviceName);
        j.field("queue", o.queue);
        j.field("fps", o.fps);
        j.field("capSysNiceEffective", capEffective(kCapSysNice));
        j.key("runs").beginArray();
        for (const Result& r : results) {
            j.beginObject();
            j.field("priority", r.priority);
            j.field("result", vkResultName(r.created));
            j.field("frames", static_cast<unsigned long long>(r.frames));
            j.field("wallMean", r.wall.mean);
            j.field("wallP50", r.wall.p50);
            j.field("wallP99", r.wall.p99);
            j.field("wallMax", r.wall.max);
            j.field("gpuMean", r.gpu.mean);
            j.field("gpuP99", r.gpu.p99);
            j.field("waitMean", r.wait.mean);
            j.field("waitP99", r.wait.p99);
            j.endObject();
        }
        j.endArray();
        j.endObject();
        std::ofstream out(o.json);
        out << j.str() << '\n';
    }
    return 0;
}

} // namespace lab
