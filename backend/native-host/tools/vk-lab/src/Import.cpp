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

// `import` — the captured picture into Vulkan (plan pipeline-video-d3d12-v2,
// Phase 13, C13.1): the first link of any Vulkan chain, the split route
// (Vulkan conversion into VA-API) included.
//
// The buffer the KMS primary plane scans out is taken the way KmsCapture
// takes it — GETFB2, one DMA-BUF fd per GEM object (AMD's displayable DCC is
// three planes of one object) — and imported as a VkImage with its modifier
// and its plane layouts. Its implicit fence comes in as a sync_file the copy
// waits on: the compositor may still be drawing into it. The picture is then
// copied out linear on the GPU.
//
// The same buffer is imported by EGL as GlConvert does, and read back: the
// two readings must be the same pixels. Anything else is a wrong import — the
// compression metadata ignored, a plane's offset off, a picture read before
// it was drawn.
//
// Timing: what an import costs the first time a buffer is met (image, memory,
// binding) and what the copy costs on the GPU.
//
// Needs root: GETFB2 withholds the handles without CAP_SYS_ADMIN.

#include "Import.h"

#include "Json.h"
#include "Lab.h"
#include "Vk.h"

#include <fcntl.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifdef MW_VK_LAB_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <gbm.h>
// After gl3.h, whose types it uses.
#include <GLES2/gl2ext.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// Exporting a DMA-BUF's implicit fences (Linux 6.0); the headers of older
// distributions lack it, the running kernel is what counts.
#ifndef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
struct dma_buf_export_sync_file
{
    __u32 flags;
    __s32 fd;
};
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR('b', 2, struct dma_buf_export_sync_file)
#endif

namespace lab {
namespace {

struct Options
{
    std::string device;
    std::string card;
    std::string queue = "compute";
    int repeat = 30;
    std::string out;
    std::string json;
    bool egl = true;
};

/// The KMS primary plane's buffer, as GETFB2 gave it.
struct Framebuffer
{
    uint32_t plane = 0;
    uint32_t width = 0, height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    int planes = 0;
    int fds[4] = {-1, -1, -1, -1}; ///< one per plane, repeated when they share an object
    uint32_t offsets[4] = {};
    uint32_t pitches[4] = {};
    std::vector<int> owned; ///< the distinct fds, to close

    ~Framebuffer()
    {
        for (int fd : owned)
            ::close(fd);
    }
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
        } else if (a == "--card") {
            if (!next(o.card)) return false;
        } else if (a == "--queue") {
            if (!next(o.queue)) return false;
        } else if (a == "--repeat") {
            if (!next(v)) return false;
            o.repeat = std::stoi(v);
        } else if (a == "--out") {
            if (!next(o.out)) return false;
        } else if (a == "--json") {
            if (!next(o.json)) return false;
        } else if (a == "--no-egl") {
            o.egl = false;
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    return (o.queue == "compute" || o.queue == "graphics") && o.repeat > 0;
}

std::string fourccText(uint32_t code)
{
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i)
        s[i] = static_cast<char>((code >> (8 * i)) & 0xff);
    return s;
}

VkFormat vulkanFormat(uint32_t fourcc)
{
    switch (fourcc) {
    case 0x34325258: // XR24
    case 0x34325241: // AR24
        return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x34324258: // XB24
    case 0x34324241: // AB24
        return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x30335258: // XR30
    case 0x30335241: // AR30
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    case 0x30334258: // XB30
    case 0x30334241: // AB30
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/// The primary plane showing something on @p card, exported as KmsCapture
/// does. "" on success.
std::string exportPrimary(int card, Framebuffer& fb)
{
    drmSetClientCap(card, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes* planes = drmModeGetPlaneResources(card);
    std::string error = "no primary plane shows anything";
    for (uint32_t i = 0; planes && i < planes->count_planes; ++i) {
        drmModePlane* p = drmModeGetPlane(card, planes->planes[i]);
        if (!p) continue;
        bool primary = false;
        drmModeObjectProperties* props =
            drmModeObjectGetProperties(card, p->plane_id, DRM_MODE_OBJECT_PLANE);
        for (uint32_t k = 0; props && k < props->count_props; ++k) {
            drmModePropertyRes* prop = drmModeGetProperty(card, props->props[k]);
            if (prop && std::strcmp(prop->name, "type") == 0)
                primary = props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY;
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
        if (!primary || !p->fb_id || !p->crtc_id) {
            drmModeFreePlane(p);
            continue;
        }
        drmModeFB2* f = drmModeGetFB2(card, p->fb_id);
        fb.plane = p->plane_id;
        drmModeFreePlane(p);
        if (!f) {
            error = "GETFB2 failed";
            continue;
        }
        if (!f->handles[0]) {
            drmModeFreeFB2(f);
            error = "GETFB2 withheld the handles: run as root (CAP_SYS_ADMIN)";
            break;
        }
        fb.width = f->width;
        fb.height = f->height;
        fb.fourcc = f->pixel_format;
        fb.modifier = (f->flags & DRM_MODE_FB_MODIFIERS) ? f->modifier : 0;
        uint32_t handles[4] = {};
        for (int k = 0; k < 4 && f->handles[k]; ++k) {
            int fd = -1;
            for (int j = 0; j < k; ++j)
                if (handles[j] == f->handles[k]) fd = fb.fds[j];
            if (fd < 0) {
                if (drmPrimeHandleToFD(card, f->handles[k], DRM_CLOEXEC | DRM_RDWR, &fd) != 0) {
                    error = "PrimeHandleToFD failed";
                    break;
                }
                fb.owned.push_back(fd);
            }
            handles[k] = f->handles[k];
            fb.fds[k] = fd;
            fb.offsets[k] = f->offsets[k];
            fb.pitches[k] = f->pitches[k];
            fb.planes = k + 1;
        }
        drmModeFreeFB2(f);
        for (int k = 0; k < 4 && handles[k]; ++k) {
            bool dup = false;
            for (int j = 0; j < k; ++j)
                dup = dup || handles[j] == handles[k];
            if (dup) continue;
            struct drm_gem_close close = {};
            close.handle = handles[k];
            drmIoctl(card, DRM_IOCTL_GEM_CLOSE, &close);
        }
        if (fb.planes) error.clear();
        break;
    }
    if (planes) drmModeFreePlaneResources(planes);
    return error;
}

/// The first /dev/dri/card* with a primary plane showing something.
std::string findCard()
{
    for (int i = 0; i < 16; ++i) {
        const std::string path = "/dev/dri/card" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        bool active = false;
        for (int c = 0; res && c < res->count_crtcs && !active; ++c) {
            drmModeCrtc* crtc = drmModeGetCrtc(fd, res->crtcs[c]);
            active = crtc && crtc->mode_valid;
            drmModeFreeCrtc(crtc);
        }
        if (res) drmModeFreeResources(res);
        ::close(fd);
        if (active) return path;
    }
    return "";
}

/// An imported picture: its image and memory.
struct Imported
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

void release(DeviceObjects& own, Imported& im)
{
    if (im.image) own.fn.vkDestroyImage(own.device, im.image, nullptr);
    if (im.memory) own.fn.vkFreeMemory(own.device, im.memory, nullptr);
    im = {};
}

/// @p fb as a VkImage: its modifier, its plane layouts, one memory import
/// of its object (the planes of an AMD DCC buffer share it).
VkResult importImage(DeviceObjects& own, const Framebuffer& fb, VkFormat format, Imported& out)
{
    DeviceFunctions& fn = own.fn;
    VkSubresourceLayout layouts[4] = {};
    for (int i = 0; i < fb.planes; ++i) {
        layouts[i].offset = fb.offsets[i];
        layouts[i].rowPitch = fb.pitches[i];
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT explicitInfo = {};
    explicitInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    explicitInfo.drmFormatModifier = fb.modifier;
    explicitInfo.drmFormatModifierPlaneCount = static_cast<uint32_t>(fb.planes);
    explicitInfo.pPlaneLayouts = layouts;
    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.pNext = &explicitInfo;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {fb.width, fb.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = fn.vkCreateImage(own.device, &ici, nullptr, &out.image);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req = {};
    fn.vkGetImageMemoryRequirements(own.device, out.image, &req);
    VkMemoryFdPropertiesKHR fdProps = {};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    r = fn.vkGetMemoryFdPropertiesKHR(own.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                      fb.fds[0], &fdProps);
    if (r != VK_SUCCESS) return r;
    const uint32_t bits = req.memoryTypeBits & fdProps.memoryTypeBits;
    uint32_t type = 0;
    while (type < 32 && !(bits & (1u << type)))
        ++type;
    if (type == 32) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // Vulkan takes the fd it imports: give it a copy, the frame keeps its own.
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = ::dup(fb.fds[0]);
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
    return fn.vkBindImageMemory(own.device, out.image, out.memory, 0);
}

#ifdef MW_VK_LAB_EGL
/// The same buffer read through EGL, as GlConvert imports it, into @p rgba
/// (RGBA, top row first). "" on success.
std::string readWithEgl(const Framebuffer& fb, const std::string& renderNode,
                        std::vector<uint8_t>& rgba)
{
    const int fd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return "cannot open " + renderNode;
    gbm_device* gbm = gbm_create_device(fd);
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto createImage =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto targetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    std::string error;
    EGLDisplay display = gbm && getPlatformDisplay
                             ? getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                             : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    EGLContext context = EGL_NO_CONTEXT;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !createImage ||
        !destroyImage || !targetTexture) {
        error = "EGL does not initialize";
    } else {
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
                                        0, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        eglChooseConfig(display, configAttribs, &config, 1, &count);
        const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
        if (context == EGL_NO_CONTEXT ||
            !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
            error = "no ES3 context";
    }
    if (error.empty()) {
        static const EGLint kFd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                      EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
        static const EGLint kOffset[4] = {
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
        static const EGLint kPitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                         EGL_DMA_BUF_PLANE2_PITCH_EXT,
                                         EGL_DMA_BUF_PLANE3_PITCH_EXT};
        static const EGLint kModLo[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
        static const EGLint kModHi[4] = {
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
        EGLint attribs[64];
        int n = 0;
        attribs[n++] = EGL_WIDTH;
        attribs[n++] = static_cast<EGLint>(fb.width);
        attribs[n++] = EGL_HEIGHT;
        attribs[n++] = static_cast<EGLint>(fb.height);
        attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[n++] = static_cast<EGLint>(fb.fourcc);
        for (int i = 0; i < fb.planes; ++i) {
            attribs[n++] = kFd[i];
            attribs[n++] = fb.fds[i];
            attribs[n++] = kOffset[i];
            attribs[n++] = static_cast<EGLint>(fb.offsets[i]);
            attribs[n++] = kPitch[i];
            attribs[n++] = static_cast<EGLint>(fb.pitches[i]);
            attribs[n++] = kModLo[i];
            attribs[n++] = static_cast<EGLint>(fb.modifier & 0xffffffffu);
            attribs[n++] = kModHi[i];
            attribs[n++] = static_cast<EGLint>(fb.modifier >> 32);
        }
        attribs[n++] = EGL_NONE;
        EGLImageKHR image =
            createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
        if (image == EGL_NO_IMAGE_KHR) {
            error = "eglCreateImageKHR refused the buffer";
        } else {
            GLuint texture = 0, framebuffer = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            targetTexture(GL_TEXTURE_2D, image);
            glGenFramebuffers(1, &framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                error = "the imported texture is not readable as a framebuffer";
            } else {
                rgba.assign(static_cast<size_t>(fb.width) * fb.height * 4, 0);
                glReadPixels(0, 0, static_cast<GLsizei>(fb.width), static_cast<GLsizei>(fb.height),
                             GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                if (glGetError() != GL_NO_ERROR) error = "glReadPixels failed";
            }
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &texture);
            destroyImage(display, image);
        }
    }
    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
    }
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    if (gbm) gbm_device_destroy(gbm);
    ::close(fd);
    return error;
}
#endif

/// The render node of the GPU whose primary node is @p card.
std::string renderNodeOf(const std::string& card)
{
    char* name = nullptr;
    const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        name = drmGetRenderDeviceNameFromFd(fd);
        ::close(fd);
    }
    std::string out = name ? name : "/dev/dri/renderD128";
    std::free(name);
    return out;
}

} // namespace

void importUsage()
{
    say("mw-vk-lab import [--card /dev/dri/cardN] [--device <index|name>]\n"
        "                 [--queue compute|graphics] [--repeat 30] [--out picture.ppm]\n"
        "                 [--json x.json] [--no-egl]\n"
        "  The buffer the KMS primary plane scans out, exported as KmsCapture does (GETFB2,\n"
        "  one fd per GEM object), imported as a VkImage with its modifier and plane layouts,\n"
        "  its implicit fence waited on as a sync_file, copied out linear; then read through\n"
        "  EGL as GlConvert imports it, and the two readings compared pixel for pixel.\n"
        "  --repeat times the import (image, memory, binding) and the copy. Run as root.\n");
}

int runImport(int argc, char** argv)
{
    Options o;
    try {
        if (!parse(argc, argv, o)) {
            importUsage();
            return 2;
        }
    } catch (...) {
        importUsage();
        return 2;
    }
    if (o.card.empty()) o.card = findCard();
    const int card = o.card.empty() ? -1 : ::open(o.card.c_str(), O_RDWR | O_CLOEXEC);
    if (card < 0) {
        say("mw-vk-lab import: no DRM card with an active display\n");
        return 1;
    }
    Framebuffer fb;
    const std::string exported = exportPrimary(card, fb);
    struct stat cardStat = {};
    ::fstat(card, &cardStat);
    ::close(card);
    if (!exported.empty()) {
        say("mw-vk-lab import: %s\n", exported.c_str());
        return 1;
    }
    const VkFormat format = vulkanFormat(fb.fourcc);
    const std::string renderNode = renderNodeOf(o.card);

    Vulkan vk;
    std::string error;
    if (!vk.open(VK_API_VERSION_1_3, error)) {
        say("mw-vk-lab import: %s\n", error.c_str());
        return 1;
    }
    // The Vulkan device of this card: its DRM primary node, or --device.
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(vk.instance(), &count, devices.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    std::vector<std::string> extensions;
    for (uint32_t i = 0; i < count && !pd; ++i) {
        std::vector<std::string> ext = deviceExtensions(vk, devices[i]);
        VkPhysicalDeviceDrmPropertiesEXT drm = {};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2 = {};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        if (hasExtension(ext, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) p2.pNext = &drm;
        vk.vkGetPhysicalDeviceProperties2(devices[i], &p2);
        if (p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        const bool sameCard = drm.hasPrimary &&
                              drm.primaryMajor == static_cast<int64_t>(major(cardStat.st_rdev)) &&
                              drm.primaryMinor == static_cast<int64_t>(minor(cardStat.st_rdev));
        if (o.device.empty() ? !sameCard : !matchesDevice(o.device, i, p2.properties.deviceName))
            continue;
        pd = devices[i];
        props = p2.properties;
        extensions = std::move(ext);
    }
    if (!pd) {
        say("mw-vk-lab import: no Vulkan device for %s\n", o.card.c_str());
        return 1;
    }
    const char* const needed[] = {VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                  "VK_KHR_external_memory_fd",
                                  VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    std::vector<const char*> enable;
    for (const char* name : needed) {
        if (!hasExtension(extensions, name)) {
            say("mw-vk-lab import: %s lacks %s\n", props.deviceName, name);
            return 1;
        }
        enable.push_back(name);
    }
    const bool syncFd = hasExtension(extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    if (syncFd) enable.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    // How many planes Vulkan wants for this modifier: it must be the buffer's.
    uint32_t wantedPlanes = 0;
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
        for (const auto& m : mods)
            if (m.drmFormatModifier == fb.modifier) wantedPlanes = m.drmFormatModifierPlaneCount;
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
        say("mw-vk-lab import: no %s queue family\n", o.queue.c_str());
        return 1;
    }

    say("mw-vk-lab import — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  %s plane %u: %ux%u %s modifier %s, %d plane(s) (Vulkan wants %u), render node %s\n",
        o.card.c_str(), fb.plane, fb.width, fb.height, fourccText(fb.fourcc).c_str(),
        hex(fb.modifier).c_str(), fb.planes, wantedPlanes, renderNode.c_str());
    for (int i = 0; i < fb.planes; ++i)
        say("    plane %d: fd %d, offset %u, pitch %u\n", i, fb.fds[i], fb.offsets[i],
            fb.pitches[i]);
    if (format == VK_FORMAT_UNDEFINED) {
        say("mw-vk-lab import: %s is not a capture format\n", fourccText(fb.fourcc).c_str());
        return 1;
    }
    if (fb.owned.size() != 1) {
        // Planes in separate objects need a disjoint image, a memory each:
        // not met on the benches so far, so not written.
        say("mw-vk-lab import: the planes live in %zu objects; only one is handled\n",
            fb.owned.size());
        return 1;
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
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queue;
    dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    VkResult r = vk.vkCreateDevice(pd, &dci, nullptr, &own.device);
    if (r != VK_SUCCESS) {
        say("mw-vk-lab import: vkCreateDevice: %s\n", vkResultName(r).c_str());
        own.device = VK_NULL_HANDLE;
        return 1;
    }
    std::string missing;
    if (!own.fn.load(vk, own.device, false, missing) || !own.fn.vkGetMemoryFdPropertiesKHR) {
        say("mw-vk-lab import: the device has no %s\n",
            missing.empty() ? "vkGetMemoryFdPropertiesKHR" : missing.c_str());
        return 1;
    }
    DeviceFunctions& fn = own.fn;
    VkDevice device = own.device;
    VkQueue q = VK_NULL_HANDLE;
    fn.vkGetDeviceQueue(device, family, 0, &q);
    VkPhysicalDeviceMemoryProperties mem = {};
    vk.vkGetPhysicalDeviceMemoryProperties(pd, &mem);

    // The linear copy the pictures land in.
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(fb.width) * fb.height * 4;
    VkBuffer readback = VK_NULL_HANDLE;
    void* mapped = nullptr;
    {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        r = fn.vkCreateBuffer(device, &bci, nullptr, &readback);
        if (r == VK_SUCCESS) own.buffers.push_back(readback);
        VkMemoryRequirements req = {};
        if (r == VK_SUCCESS) fn.vkGetBufferMemoryRequirements(device, readback, &req);
        const VkMemoryPropertyFlags visible =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (r == VK_SUCCESS)
            r = own.allocate(mem, req, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, visible,
                             memory);
        if (r == VK_SUCCESS) r = fn.vkBindBufferMemory(device, readback, memory, 0);
        if (r == VK_SUCCESS) r = fn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (r != VK_SUCCESS) {
            say("mw-vk-lab import: readback buffer: %s\n", vkResultName(r).c_str());
            return 1;
        }
    }
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
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
        VkQueryPoolCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 2;
        fn.vkCreateQueryPool(device, &qci, nullptr, &timestamps);
        own.queryPools.push_back(timestamps);
        VkSemaphoreTypeCreateInfo kind = {};
        kind.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        kind.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &kind;
        fn.vkCreateSemaphore(device, &sci, nullptr, &done);
        own.semaphores.push_back(done);
    }

    // One copy of @p image into the readback buffer, after the buffer's
    // implicit fence (a sync_file, when the kernel and the driver offer it).
    // Returns the GPU time in ms, or -1.
    const double period = static_cast<double>(props.limits.timestampPeriod);
    uint64_t submitted = 0;
    bool fenceWaited = false;
    auto copyOut = [&](VkImage image, double& gpuMs) -> VkResult {
        VkSemaphore fence = VK_NULL_HANDLE;
        if (syncFd && fn.vkImportSemaphoreFdKHR) {
            struct dma_buf_export_sync_file exportFence = {};
            exportFence.flags = DMA_BUF_SYNC_READ;
            exportFence.fd = -1;
            if (::ioctl(fb.fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exportFence) == 0 &&
                exportFence.fd >= 0) {
                VkSemaphoreCreateInfo sci = {};
                sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                fn.vkCreateSemaphore(device, &sci, nullptr, &fence);
                VkImportSemaphoreFdInfoKHR isi = {};
                isi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
                isi.semaphore = fence;
                isi.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
                isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
                isi.fd = exportFence.fd;
                if (fn.vkImportSemaphoreFdKHR(device, &isi) != VK_SUCCESS) {
                    ::close(exportFence.fd);
                    fn.vkDestroySemaphore(device, fence, nullptr);
                    fence = VK_NULL_HANDLE;
                }
            }
        }
        fenceWaited = fenceWaited || fence != VK_NULL_HANDLE;
        fn.vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbi = {};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        fn.vkBeginCommandBuffer(cmd, &cbi);
        fn.vkCmdResetQueryPool(cmd, timestamps, 0, 2);
        // Acquired from the "foreign" owner, the display, in the layout it
        // left: GENERAL, never UNDEFINED (which may throw the contents away).
        VkImageMemoryBarrier2 acquire = {};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = family;
        acquire.image = image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &acquire;
        fn.vkCmdPipelineBarrier2(cmd, &dep);
        fn.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, 0);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {fb.width, fb.height, 1};
        fn.vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                                  &region);
        fn.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, timestamps, 1);
        VkImageMemoryBarrier2 back = acquire;
        back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        back.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        back.dstAccessMask = VK_ACCESS_2_NONE;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        back.srcQueueFamilyIndex = family;
        back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        dep.pImageMemoryBarriers = &back;
        fn.vkCmdPipelineBarrier2(cmd, &dep);
        fn.vkEndCommandBuffer(cmd);
        const uint64_t value = ++submitted;
        VkCommandBufferSubmitInfo cmdInfo = {};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;
        VkSemaphoreSubmitInfo wait = {};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = fence;
        wait.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        VkSemaphoreSubmitInfo signal = {};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = done;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = fence ? 1 : 0;
        submit.pWaitSemaphoreInfos = fence ? &wait : nullptr;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        VkResult sr = fn.vkQueueSubmit2(q, 1, &submit, VK_NULL_HANDLE);
        if (sr == VK_SUCCESS) {
            VkSemaphoreWaitInfo wi = {};
            wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wi.semaphoreCount = 1;
            wi.pSemaphores = &done;
            wi.pValues = &value;
            sr = fn.vkWaitSemaphores(device, &wi, 2000000000ull);
        }
        if (fence) fn.vkDestroySemaphore(device, fence, nullptr);
        gpuMs = -1;
        uint64_t ts[2] = {};
        if (sr == VK_SUCCESS &&
            fn.vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS)
            gpuMs = static_cast<double>(ts[1] - ts[0]) * period / 1e6;
        return sr;
    };

    // ── The import, once, then read ──
    Imported first;
    const int64_t t0 = nowUs();
    r = importImage(own, fb, format, first);
    const double firstImportMs = static_cast<double>(nowUs() - t0) / 1000.0;
    if (r != VK_SUCCESS) {
        say("  import: %s\n", vkResultName(r).c_str());
        release(own, first);
        return 1;
    }
    double gpuMs = -1;
    r = copyOut(first.image, gpuMs);
    if (r != VK_SUCCESS) {
        say("  copy: %s\n", vkResultName(r).c_str());
        release(own, first);
        return 1;
    }
    std::vector<uint8_t> vkPixels(static_cast<const uint8_t*>(mapped),
                                  static_cast<const uint8_t*>(mapped) + bytes);
    say("  imported in %.2f ms, copied out in %.3f ms on the GPU%s\n", firstImportMs, gpuMs,
        fenceWaited ? ", after its implicit fence (sync_file)" : ", no sync_file");

    // ── The same buffer through EGL ──
    std::string eglVerdict = "not compared";
    double psnr = -1;
    size_t different = 0;
    int maxDiff = 0;
#ifdef MW_VK_LAB_EGL
    if (o.egl) {
        std::vector<uint8_t> glPixels;
        const std::string bad = readWithEgl(fb, renderNode, glPixels);
        if (!bad.empty()) {
            eglVerdict = "EGL: " + bad;
        } else if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM) {
            eglVerdict = "compared for 8-bit formats only";
        } else {
            // Vulkan's copy is in the image's own byte order (BGRA for XR24),
            // GL's is RGBA. Both start with the buffer's first row: the
            // texture's origin is that row, and glReadPixels starts there.
            const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
            double squares = 0;
            const size_t pixels = static_cast<size_t>(fb.width) * fb.height;
            for (size_t i = 0; i < pixels; ++i) {
                const uint8_t* v = vkPixels.data() + i * 4;
                const uint8_t* g = glPixels.data() + i * 4;
                const int rgb[3] = {bgra ? v[2] : v[0], v[1], bgra ? v[0] : v[2]};
                bool differs = false;
                for (int c = 0; c < 3; ++c) {
                    const int d = std::abs(rgb[c] - static_cast<int>(g[c]));
                    maxDiff = std::max(maxDiff, d);
                    squares += static_cast<double>(d) * d;
                    differs = differs || d != 0;
                }
                different += differs ? 1 : 0;
            }
            const double mse = squares / (static_cast<double>(pixels) * 3);
            psnr = mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
            eglVerdict = different ? "differs" : "identical";
        }
    }
#endif
    say("  against EGL: %s", eglVerdict.c_str());
    if (psnr >= 0)
        say(" (%zu pixels differ, at most by %d; PSNR %.1f dB)", different, maxDiff, psnr);
    say("\n");

    if (!o.out.empty()) {
        std::ofstream out(o.out, std::ios::binary);
        out << "P6\n" << fb.width << ' ' << fb.height << "\n255\n";
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i < static_cast<size_t>(fb.width) * fb.height; ++i) {
            const uint8_t* v = vkPixels.data() + i * 4;
            const char rgb[3] = {static_cast<char>(bgra ? v[2] : v[0]), static_cast<char>(v[1]),
                                 static_cast<char>(bgra ? v[0] : v[2])};
            out.write(rgb, 3);
        }
        say("  picture: %s\n", o.out.c_str());
    }

    // ── Costs: importing again, copying again ──
    std::vector<double> imports, copies;
    for (int i = 0; i < o.repeat; ++i) {
        Imported again;
        const int64_t t = nowUs();
        if (importImage(own, fb, format, again) == VK_SUCCESS)
            imports.push_back(static_cast<double>(nowUs() - t) / 1000.0);
        release(own, again);
        double ms = -1;
        if (copyOut(first.image, ms) == VK_SUCCESS && ms >= 0) copies.push_back(ms);
    }
    release(own, first);
    const Stats is = stats(imports), cs = stats(copies);
    say("  %d imports: %.2f / p99 %.2f ms; %d copies: %.3f / p99 %.3f ms on the GPU\n",
        static_cast<int>(imports.size()), is.mean, is.p99, static_cast<int>(copies.size()), cs.mean,
        cs.p99);

    if (!o.json.empty()) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-vk-lab import");
        j.field("date", nowText());
        j.field("host", hostName());
        j.field("device", props.deviceName);
        j.field("card", o.card);
        j.field("width", fb.width);
        j.field("height", fb.height);
        j.field("fourcc", fourccText(fb.fourcc));
        j.field("modifier", hex(fb.modifier));
        j.field("planes", fb.planes);
        j.field("vulkanPlanes", wantedPlanes);
        j.field("syncFile", fenceWaited);
        j.field("firstImportMs", firstImportMs);
        j.field("importMeanMs", is.mean);
        j.field("importP99Ms", is.p99);
        j.field("copyMeanMs", cs.mean);
        j.field("copyP99Ms", cs.p99);
        j.field("egl", eglVerdict);
        j.field("differentPixels", static_cast<unsigned long long>(different));
        j.field("maxDifference", maxDiff);
        j.field("psnr", psnr);
        j.endObject();
        std::ofstream out(o.json);
        out << j.str() << '\n';
    }
    return 0;
}

} // namespace lab
