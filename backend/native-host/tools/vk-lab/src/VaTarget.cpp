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

// `vatarget` — the split route's last link (plan pipeline-video-d3d12-v2,
// Phase 13, C13.1 and C13.3): a Vulkan compute queue writing straight into the
// surface VA-API encodes from.
//
// The surface is made the way VaapiEncoder makes its input — vaCreateSurfaces
// with nothing but the format, exported as two layers, one per plane — so the
// modifier, offsets and pitches are the ones the engine will meet. Its two
// planes are imported as an R8 and an RG8 image with that modifier, written by
// a compute shader with imageStore (shaders/fill.comp: a pattern every value
// of which is exact in 8 bits), released to the foreign queue family, and the
// CPU waits for the queue — the hand-off GlConvert makes with glFinish. VA-API
// then reads the surface back (vaGetImage), and every pixel is compared.
//
// Answers: which modifier VA-API hands out, whether the driver writes that
// modifier as a storage image, whether the writes are what VA-API sees, and
// what one write costs.

#include "VaTarget.h"

#include "Lab.h"
#include "Vk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// glslangValidator's output: the SPIR-V of shaders/fill.comp, as kFillSpv. It
// names uint32_t without including anything, hence last.
#include "FillSpv.h"

namespace lab {
namespace {

struct Options
{
    std::string device;
    std::string render = "/dev/dri/renderD128";
    int width = 1920;
    int height = 1080;
    int repeat = 60;
};

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
        } else if (a == "--render") {
            if (!next(o.render)) return false;
        } else if (a == "--size") {
            if (!next(v) || std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (a == "--repeat") {
            if (!next(v)) return false;
            o.repeat = std::stoi(v);
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    return o.width > 0 && o.height > 0 && o.repeat > 0;
}

std::string features(VkFormatFeatureFlags2 f)
{
    return decode(f,
                  {{VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT, "storage"},
                   {VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT, "sampled"},
                   {VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT, "linear"},
                   {VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT, "color"},
                   {VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT, "xfer-src"},
                   {VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT, "xfer-dst"},
                   {VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT, "write-no-fmt"}},
                  false);
}

/// The modifier's features for @p format, and its plane count; false when the
/// driver does not list it at all.
bool modifierFeatures(const Vulkan& vk, VkPhysicalDevice pd, VkFormat format, uint64_t modifier,
                      VkFormatFeatureFlags2& out, uint32_t& planes)
{
    VkDrmFormatModifierPropertiesList2EXT list = {};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT;
    VkFormatProperties2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    f2.pNext = &list;
    vk.vkGetPhysicalDeviceFormatProperties2(pd, format, &f2);
    std::vector<VkDrmFormatModifierProperties2EXT> mods(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = mods.data();
    vk.vkGetPhysicalDeviceFormatProperties2(pd, format, &f2);
    for (const auto& m : mods) {
        if (m.drmFormatModifier != modifier) continue;
        out = m.drmFormatModifierTilingFeatures;
        planes = m.drmFormatModifierPlaneCount;
        return true;
    }
    return false;
}

struct Plane
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

/// One layer of the exported surface as a single-plane image of @p format.
VkResult importPlane(DeviceObjects& own, VkFormat format, uint32_t w, uint32_t h, uint64_t modifier,
                     int fd, uint32_t offset, uint32_t pitch, Plane& out)
{
    DeviceFunctions& fn = own.fn;
    VkSubresourceLayout layout = {};
    layout.offset = offset;
    layout.rowPitch = pitch;
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitInfo = {};
    explicitInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    explicitInfo.drmFormatModifier = modifier;
    explicitInfo.drmFormatModifierPlaneCount = 1;
    explicitInfo.pPlaneLayouts = &layout;
    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &explicitInfo;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = fn.vkCreateImage(own.device, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return r;
    own.images.push_back(out.image);
    VkMemoryRequirements req = {};
    fn.vkGetImageMemoryRequirements(own.device, out.image, &req);
    VkMemoryFdPropertiesKHR fdProps = {};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    r = fn.vkGetMemoryFdPropertiesKHR(own.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                      fd, &fdProps);
    if (r != VK_SUCCESS) return r;
    const uint32_t bits = req.memoryTypeBits & fdProps.memoryTypeBits;
    uint32_t type = 0;
    while (type < 32 && !(bits & (1u << type)))
        ++type;
    if (type == 32) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = ::dup(fd); // Vulkan takes the fd it imports
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import;
    dedicated.image = out.image;
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = fn.vkAllocateMemory(own.device, &mai, nullptr, &out.memory);
    if (r != VK_SUCCESS) {
        ::close(import.fd);
        return r;
    }
    own.memories.push_back(out.memory);
    r = fn.vkBindImageMemory(own.device, out.image, out.memory, 0);
    if (r != VK_SUCCESS) return r;
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = fn.vkCreateImageView(own.device, &vci, nullptr, &out.view);
    if (r == VK_SUCCESS) own.views.push_back(out.view);
    return r;
}

} // namespace

void vaTargetUsage()
{
    say("mw-vk-lab vatarget [--render /dev/dri/renderD128] [--device <index|name>]\n"
        "                   [--size 1920x1080] [--repeat 60]\n"
        "  The surface VA-API encodes from, made as VaapiEncoder makes it and exported as two\n"
        "  layers; its planes imported into Vulkan as R8 and RG8 storage images, written by a\n"
        "  compute shader, handed back to VA-API (foreign release, CPU wait) and read back\n"
        "  there pixel for pixel. --repeat times one write and its wait.\n");
}

int runVaTarget(int argc, char** argv)
{
    Options o;
    try {
        if (!parse(argc, argv, o)) {
            vaTargetUsage();
            return 2;
        }
    } catch (...) {
        vaTargetUsage();
        return 2;
    }

    // ── VA-API: the encoder's input surface, as VaapiEncoder makes it ──
    const int vaFd = ::open(o.render.c_str(), O_RDWR | O_CLOEXEC);
    if (vaFd < 0) {
        say("mw-vk-lab vatarget: cannot open %s\n", o.render.c_str());
        return 1;
    }
    VADisplay va = vaGetDisplayDRM(vaFd);
    int vaMajor = 0, vaMinor = 0;
    if (!va || vaInitialize(va, &vaMajor, &vaMinor) != VA_STATUS_SUCCESS) {
        say("mw-vk-lab vatarget: VA-API does not initialize on %s\n", o.render.c_str());
        ::close(vaFd);
        return 1;
    }
    const char* vendor = vaQueryVendorString(va);
    VASurfaceID surface = VA_INVALID_SURFACE;
    VADRMPRIMESurfaceDescriptor desc = {};
    bool exported = false;
    int rc = 1;
    if (vaCreateSurfaces(va, VA_RT_FORMAT_YUV420, static_cast<unsigned>(o.width),
                         static_cast<unsigned>(o.height), &surface, 1, nullptr,
                         0) != VA_STATUS_SUCCESS) {
        say("mw-vk-lab vatarget: vaCreateSurfaces failed\n");
    } else if (vaExportSurfaceHandle(va, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                     VA_EXPORT_SURFACE_WRITE_ONLY |
                                         VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                     &desc) != VA_STATUS_SUCCESS) {
        say("mw-vk-lab vatarget: vaExportSurfaceHandle failed\n");
    } else {
        exported = true;
    }
    auto finish = [&](int code) {
        if (exported)
            for (uint32_t i = 0; i < desc.num_objects; ++i)
                ::close(desc.objects[i].fd);
        if (surface != VA_INVALID_SURFACE) vaDestroySurfaces(va, &surface, 1);
        vaTerminate(va);
        ::close(vaFd);
        return code;
    };
    if (!exported) return finish(1);

    say("mw-vk-lab vatarget — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  VA-API %d.%d, %s\n", vaMajor, vaMinor, vendor ? vendor : "?");
    say("  surface %dx%d: %u object(s), %u layer(s)\n", o.width, o.height, desc.num_objects,
        desc.num_layers);
    for (uint32_t i = 0; i < desc.num_objects; ++i)
        say("    object %u: fd %d, %u bytes, modifier %s\n", i, desc.objects[i].fd,
            desc.objects[i].size, hex(desc.objects[i].drm_format_modifier).c_str());
    for (uint32_t i = 0; i < desc.num_layers; ++i)
        say("    layer %u: fourcc %.4s, object %u, offset %u, pitch %u\n", i,
            reinterpret_cast<const char*>(&desc.layers[i].drm_format),
            desc.layers[i].object_index[0], desc.layers[i].offset[0], desc.layers[i].pitch[0]);
    if (desc.num_layers != 2) {
        say("mw-vk-lab vatarget: not the two NV12 layers\n");
        return finish(1);
    }
    const uint64_t modifier = desc.objects[0].drm_format_modifier;

    // ── Vulkan: the device of this render node ──
    Vulkan vk;
    std::string error;
    if (!vk.open(VK_API_VERSION_1_3, error)) {
        say("mw-vk-lab vatarget: %s\n", error.c_str());
        return finish(1);
    }
    struct stat renderStat = {};
    ::fstat(vaFd, &renderStat);
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, devices.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    std::string driverInfo;
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < count && !pd; ++i) {
        std::vector<std::string> ext = deviceExtensions(vk, devices[i]);
        VkPhysicalDeviceDrmPropertiesEXT drm = {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceDriverProperties driver = {};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2 = {};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &driver;
        if (hasExtension(ext, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) driver.pNext = &drm;
        vk.vkGetPhysicalDeviceProperties2(devices[i], &p2);
        if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const bool sameNode = drm.hasRender &&
                              drm.renderMajor == static_cast<int64_t>(major(renderStat.st_rdev)) &&
                              drm.renderMinor == static_cast<int64_t>(minor(renderStat.st_rdev));
        if (o.device.empty() ? !sameNode : !matchesDevice(o.device, i, p2.properties.deviceName))
            continue;
        pd = devices[i];
        props = p2.properties;
        driverInfo = driver.driverInfo;
        extensions = std::move(ext);
    }
    if (!pd) {
        say("mw-vk-lab vatarget: no Vulkan device for %s\n", o.render.c_str());
        return finish(1);
    }
    say("  %s (%s)\n", props.deviceName, driverInfo.c_str());

    VkFormatFeatureFlags2 lumaFeatures = 0, chromaFeatures = 0;
    uint32_t lumaPlanes = 0, chromaPlanes = 0;
    const bool lumaListed =
        modifierFeatures(vk, pd, VK_FORMAT_R8_UNORM, modifier, lumaFeatures, lumaPlanes);
    const bool chromaListed =
        modifierFeatures(vk, pd, VK_FORMAT_R8G8_UNORM, modifier, chromaFeatures, chromaPlanes);
    say("  modifier %s as R8: %s; as RG8: %s\n", hex(modifier).c_str(),
        lumaListed ? features(lumaFeatures).c_str() : "not listed",
        chromaListed ? features(chromaFeatures).c_str() : "not listed");
    if (!(lumaFeatures & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) ||
        !(chromaFeatures & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)) {
        say("  → no storage image at this modifier: the conversion would have to write an image "
            "of its own and copy it in\n");
        return finish(1);
    }

    const char* const needed[] = {VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                  "VK_KHR_external_memory_fd",
                                  VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    std::vector<const char*> enable;
    for (const char* name : needed) {
        if (!hasExtension(extensions, name)) {
            say("mw-vk-lab vatarget: %s lacks %s\n", props.deviceName, name);
            return finish(1);
        }
        enable.push_back(name);
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
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) family = i;
    }
    for (uint32_t i = 0; i < familyCount && family == UINT32_MAX; ++i)
        if (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
    if (family == UINT32_MAX) {
        say("mw-vk-lab vatarget: no compute queue\n");
        return finish(1);
    }

    DeviceObjects own;
    own.vk = &vk;
    const float one = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
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
    VkPhysicalDeviceFeatures2 f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    f2.features.shaderStorageImageExtendedFormats = VK_TRUE; // r8 and rg8
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queue;
    dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    VkResult r = vk.vkCreateDevice(pd, &dci, nullptr, &own.device);
    if (r != VK_SUCCESS) {
        say("mw-vk-lab vatarget: vkCreateDevice: %s\n", vkResultName(r).c_str());
        own.device = VK_NULL_HANDLE;
        return finish(1);
    }
    std::string missing;
    if (!own.fn.load(vk, own.device, false, missing) || !own.fn.vkGetMemoryFdPropertiesKHR) {
        say("mw-vk-lab vatarget: the device has no %s\n",
            missing.empty() ? "vkGetMemoryFdPropertiesKHR" : missing.c_str());
        return finish(1);
    }
    DeviceFunctions& fn = own.fn;
    VkDevice device = own.device;
    VkQueue q = VK_NULL_HANDLE;
    fn.vkGetDeviceQueue(device, family, 0, &q);

    // ── The two planes, imported ──
    const uint32_t w = static_cast<uint32_t>(o.width), h = static_cast<uint32_t>(o.height);
    Plane luma, chroma;
    const int64_t importStart = nowUs();
    r = importPlane(own, VK_FORMAT_R8_UNORM, w, h, modifier,
                    desc.objects[desc.layers[0].object_index[0]].fd, desc.layers[0].offset[0],
                    desc.layers[0].pitch[0], luma);
    if (r == VK_SUCCESS)
        r = importPlane(own, VK_FORMAT_R8G8_UNORM, w / 2, h / 2, modifier,
                        desc.objects[desc.layers[1].object_index[0]].fd, desc.layers[1].offset[0],
                        desc.layers[1].pitch[0], chroma);
    const double importMs = static_cast<double>(nowUs() - importStart) / 1000.0;
    if (r != VK_SUCCESS) {
        say("mw-vk-lab vatarget: import of the planes: %s\n", vkResultName(r).c_str());
        return finish(1);
    }
    say("  both planes imported as storage images in %.2f ms\n", importMs);

    // ── The fill pipeline ──
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smi = {};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = sizeof(kFillSpv);
        smi.pCode = kFillSpv;
        fn.vkCreateShaderModule(device, &smi, nullptr, &shader);
        own.shaders.push_back(shader);
        VkDescriptorSetLayoutBinding bindings[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dsl = {};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings = bindings;
        fn.vkCreateDescriptorSetLayout(device, &dsl, nullptr, &setLayout);
        own.setLayouts.push_back(setLayout);
        VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo pli = {};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        fn.vkCreatePipelineLayout(device, &pli, nullptr, &pipelineLayout);
        own.pipelineLayouts.push_back(pipelineLayout);
        VkComputePipelineCreateInfo cpi = {};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = shader;
        cpi.stage.pName = "main";
        cpi.layout = pipelineLayout;
        r = fn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
        if (r != VK_SUCCESS) {
            say("mw-vk-lab vatarget: compute pipeline: %s\n", vkResultName(r).c_str());
            return finish(1);
        }
        own.pipelines.push_back(pipeline);
        VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo dpi = {};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &size;
        fn.vkCreateDescriptorPool(device, &dpi, nullptr, &descriptorPool);
        own.descriptorPools.push_back(descriptorPool);
        VkDescriptorSetAllocateInfo dsa = {};
        dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsa.descriptorPool = descriptorPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &setLayout;
        fn.vkAllocateDescriptorSets(device, &dsa, &set);
        VkDescriptorImageInfo infos[2] = {{VK_NULL_HANDLE, luma.view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, chroma.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[2] = {};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &infos[i];
        }
        fn.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore done = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        fn.vkCreateCommandPool(device, &pci, nullptr, &pool);
        own.pools.push_back(pool);
        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        fn.vkAllocateCommandBuffers(device, &cai, &cmd);
        VkSemaphoreTypeCreateInfo kind = {};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &kind;
        fn.vkCreateSemaphore(device, &sci, nullptr, &done);
        own.semaphores.push_back(done);
    }

    // One write of the pattern for @p frame, handed back to VA-API; the CPU
    // waits for it, as GlConvert's glFinish does.
    uint64_t submitted = 0;
    auto write = [&](uint32_t frame) -> VkResult {
        fn.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbi = {};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fn.vkBeginCommandBuffer(cmd, &cbi);
        VkImageMemoryBarrier2 barriers[2] = {};
        for (int i = 0; i < 2; ++i) {
            // Taken over from VA-API, which may have read the previous picture.
            VkImageMemoryBarrier2& b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.oldLayout = frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex =
                frame == 0 ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT;
            b.dstQueueFamilyIndex = frame == 0 ? VK_QUEUE_FAMILY_IGNORED : family;
            b.image = i == 0 ? luma.image : chroma.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = barriers;
        fn.vkCmdPipelineBarrier2(cmd, &dep);
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set,
                                   0, nullptr);
        fn.vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(frame),
                              &frame);
        fn.vkCmdDispatch(cmd, (w + 15) / 16, (h + 15) / 16, 1);
        for (int i = 0; i < 2; ++i) {
            // Released to VA-API: the writes flushed, the image handed over.
            VkImageMemoryBarrier2& b = barriers[i];
            b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.dstAccessMask = VK_ACCESS_2_NONE;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = family;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        }
        fn.vkCmdPipelineBarrier2(cmd, &dep);
        fn.vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;
        VkSemaphoreSubmitInfo signal = {};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = done;
        signal.value = ++submitted;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        VkResult sr = fn.vkQueueSubmit2(q, 1, &submit, VK_NULL_HANDLE);
        if (sr != VK_SUCCESS) return sr;
        VkSemaphoreWaitInfo wait = {};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &done;
        wait.pValues = &submitted;
        return fn.vkWaitSemaphores(device, &wait, 2000000000ull);
    };

    // ── Written, then read back by VA-API ──
    std::vector<double> waits;
    for (int n = 0; n < o.repeat; ++n) {
        const int64_t t0 = nowUs();
        r = write(static_cast<uint32_t>(n));
        if (r != VK_SUCCESS) {
            say("mw-vk-lab vatarget: write %d: %s\n", n, vkResultName(r).c_str());
            return finish(1);
        }
        if (n > 0) waits.push_back(static_cast<double>(nowUs() - t0) / 1000.0);
    }
    const uint32_t lastFrame = static_cast<uint32_t>(o.repeat - 1);
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    image.image_id = VA_INVALID_ID;
    size_t wrongLuma = 0, wrongChroma = 0;
    bool read = false;
    if (vaCreateImage(va, &format, o.width, o.height, &image) == VA_STATUS_SUCCESS &&
        vaGetImage(va, surface, 0, 0, w, h, image.image_id) == VA_STATUS_SUCCESS) {
        uint8_t* pixels = nullptr;
        if (vaMapBuffer(va, image.buf, reinterpret_cast<void**>(&pixels)) == VA_STATUS_SUCCESS) {
            read = true;
            for (uint32_t y = 0; y < h; ++y) {
                const uint8_t* row =
                    pixels + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0];
                for (uint32_t x = 0; x < w; ++x)
                    wrongLuma +=
                        row[x] != static_cast<uint8_t>((x * 3u + y * 5u + lastFrame) & 255u);
            }
            for (uint32_t y = 0; y < h / 2; ++y) {
                const uint8_t* row =
                    pixels + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1];
                for (uint32_t x = 0; x < w / 2; ++x) {
                    wrongChroma +=
                        row[2 * x] != static_cast<uint8_t>((x * 7u + lastFrame) & 255u) ||
                        row[2 * x + 1] != static_cast<uint8_t>((y * 11u + lastFrame) & 255u);
                }
            }
            vaUnmapBuffer(va, image.buf);
        }
    }
    if (image.image_id != VA_INVALID_ID) vaDestroyImage(va, image.image_id);
    if (!read) {
        say("mw-vk-lab vatarget: VA-API could not read the surface back\n");
        return finish(1);
    }
    const Stats s = stats(waits);
    say("  write + wait, after the first: mean %.3f, p99 %.3f, max %.3f ms over %zu writes\n",
        s.mean, s.p99, s.max, waits.size());
    say("  read back by VA-API: %zu of %u luma and %zu of %u chroma samples wrong\n", wrongLuma,
        w * h, wrongChroma, (w / 2) * (h / 2));
    rc = (wrongLuma == 0 && wrongChroma == 0) ? 0 : 1;
    say("  → %s\n", rc == 0 ? "the compute writes are what VA-API encodes from"
                            : "VA-API does not see what Vulkan wrote");
    fn.vkDeviceWaitIdle(device);
    return finish(rc);
}

} // namespace lab
