/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(MW_NATIVE_LINUX_GFX) && defined(MW_NATIVE_LINUX_VULKAN)
#include "capture/linux/KmsCapture.h"
#include "convert/linux/GlConvert.h"
#include "convert/linux/VulkanConvert.h"

#include <fcntl.h>
#include <glob.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <chrono>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// The split route's conversion against the one it stands in for (plan Phase
// 13, C13.4): the same scanout frame through GlConvert and through
// VulkanConvert, each into a surface of its own made the way VaapiEncoder
// makes its input, both read back by VA-API and compared sample by sample —
// 1:1, then scaled down through the Lanczos-2 pass, the pointer drawn on both.
// Two routes that looked alike and differed by a colour matrix, a half-texel
// or a flipped row would pass every other test.
//
// Real display, real GPU, and CAP_SYS_ADMIN for the scanout, as the Linux
// pipeline test: skipped, and said, where any of them is missing.

#if defined(MW_NATIVE_LINUX_GFX) && defined(MW_NATIVE_LINUX_VULKAN)
namespace {

using namespace mw::native;

struct Surface
{
    VASurfaceID id = VA_INVALID_SURFACE;
    VADRMPRIMESurfaceDescriptor desc = {};
    convert::Nv12Target target;
    bool exported = false;
};

bool makeSurface(VADisplay display, int width, int height, Surface& out)
{
    if (vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, static_cast<unsigned>(width),
                         static_cast<unsigned>(height), &out.id, 1, nullptr,
                         0) != VA_STATUS_SUCCESS)
        return false;
    if (vaExportSurfaceHandle(display, out.id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                              &out.desc) != VA_STATUS_SUCCESS ||
        out.desc.num_layers != 2)
        return false;
    out.exported = true;
    out.target.width = width;
    out.target.height = height;
    out.target.modifier = out.desc.objects[0].drm_format_modifier;
    out.target.fdY = out.desc.objects[out.desc.layers[0].object_index[0]].fd;
    out.target.offsetY = out.desc.layers[0].offset[0];
    out.target.pitchY = out.desc.layers[0].pitch[0];
    out.target.fdUV = out.desc.objects[out.desc.layers[1].object_index[0]].fd;
    out.target.offsetUV = out.desc.layers[1].offset[0];
    out.target.pitchUV = out.desc.layers[1].pitch[0];
    return true;
}

void freeSurface(VADisplay display, Surface& s)
{
    if (s.exported)
        for (uint32_t i = 0; i < s.desc.num_objects; ++i)
            ::close(s.desc.objects[i].fd);
    if (s.id != VA_INVALID_SURFACE) vaDestroySurfaces(display, &s.id, 1);
    s = Surface{};
}

/// The surface's NV12 as VA-API reads it back: luma, then interleaved chroma.
bool readBack(VADisplay display, const Surface& s, std::vector<uint8_t>& y,
              std::vector<uint8_t>& uv)
{
    const int w = s.target.width, h = s.target.height;
    VAImageFormat format = {};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage image = {};
    image.image_id = VA_INVALID_ID;
    bool ok = vaCreateImage(display, &format, w, h, &image) == VA_STATUS_SUCCESS &&
              vaGetImage(display, s.id, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h),
                         image.image_id) == VA_STATUS_SUCCESS;
    uint8_t* pixels = nullptr;
    ok = ok &&
         vaMapBuffer(display, image.buf, reinterpret_cast<void**>(&pixels)) == VA_STATUS_SUCCESS;
    if (ok) {
        y.resize(static_cast<size_t>(w) * h);
        uv.resize(static_cast<size_t>(w) * (h / 2));
        for (int row = 0; row < h; ++row)
            std::copy_n(pixels + image.offsets[0] + static_cast<size_t>(row) * image.pitches[0], w,
                        y.data() + static_cast<size_t>(row) * w);
        for (int row = 0; row < h / 2; ++row)
            std::copy_n(pixels + image.offsets[1] + static_cast<size_t>(row) * image.pitches[1], w,
                        uv.data() + static_cast<size_t>(row) * w);
        vaUnmapBuffer(display, image.buf);
    }
    if (image.image_id != VA_INVALID_ID) vaDestroyImage(display, image.image_id);
    return ok;
}

struct Difference
{
    int max = 0;
    size_t overOne = 0; ///< samples more than 1 apart
    double mean = 0;
};

Difference compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    Difference d;
    if (a.size() != b.size() || a.empty()) {
        d.max = 255;
        return d;
    }
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int diff = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        d.max = std::max(d.max, diff);
        d.overOne += diff > 1 ? 1 : 0;
        sum += diff;
    }
    d.mean = sum / static_cast<double>(a.size());
    return d;
}

double msSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace
#endif

void run_vulkan_convert_tests()
{
    SECTION("Linux — the Vulkan conversion writes what the GL one writes");

#if !defined(MW_NATIVE_LINUX_GFX) || !defined(MW_NATIVE_LINUX_VULKAN)
    std::fprintf(stderr, "  skipped: the Vulkan conversion is not built\n");
#else
    glob_t cards = {};
    glob("/dev/dri/card*", 0, nullptr, &cards);
    capture::KmsOutput output;
    for (size_t i = 0; i < cards.gl_pathc && !output.active; ++i) {
        std::string error;
        for (const capture::KmsOutput& out :
             capture::KmsCapture::listOutputs(cards.gl_pathv[i], error))
            if (out.active && !output.active) output = out;
    }
    globfree(&cards);
    if (!output.active) {
        std::fprintf(stderr, "  skipped: no display is being scanned out\n");
        return;
    }
    std::string why;
    if (!capture::KmsCapture::canReadFramebuffers(output.cardPath, why)) {
        std::fprintf(stderr, "  skipped: %s\n", why.c_str());
        return;
    }
    capture::KmsCapture kms(output.cardPath, output.connectorId);
    std::string error;
    capture::KmsFrame frame;
    if (!kms.start(error) || kms.acquire(100, frame) != capture::AcquireStatus::Ok) {
        std::fprintf(stderr, "  skipped: no frame (%s)\n", error.c_str());
        return;
    }
    std::fprintf(stderr, "  frame %dx%d fourcc %.4s modifier 0x%llx, %d plane(s)\n", frame.width,
                 frame.height, reinterpret_cast<const char*>(&frame.fourcc),
                 static_cast<unsigned long long>(frame.modifier), frame.planeCount);

    const int render = ::open(kms.renderNodePath().c_str(), O_RDWR | O_CLOEXEC);
    VADisplay display = render >= 0 ? vaGetDisplayDRM(render) : nullptr;
    int vaMajor = 0, vaMinor = 0;
    if (!display || vaInitialize(display, &vaMajor, &vaMinor) != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "  skipped: no VA-API on %s\n", kms.renderNodePath().c_str());
        if (render >= 0) ::close(render);
        return;
    }

    // The pointer drawn on both, where there is one: the same state, fed to
    // both converters.
    const capture::CursorState& cursor = kms.cursor();
    struct Geometry
    {
        const char* what;
        int width;
        int height;
        convert::ScaleFilter filter;
    };
    const Geometry geometries[] = {
        {"1:1", frame.width, frame.height, convert::ScaleFilter::Bilinear},
        {"Lanczos-2 to 1280x720", 1280, 720, convert::ScaleFilter::Lanczos2},
        {"bilinear to 1280x720", 1280, 720, convert::ScaleFilter::Bilinear},
    };
    for (const Geometry& g : geometries) {
        Surface glSurface, vkSurface;
        if (!makeSurface(display, g.width, g.height, glSurface) ||
            !makeSurface(display, g.width, g.height, vkSurface)) {
            std::fprintf(stderr, "  %s: no VA-API surface\n", g.what);
            CHECK(false);
            freeSurface(display, glSurface);
            freeSurface(display, vkSurface);
            continue;
        }

        convert::GlConvert gl;
        CHECK(gl.init(kms.renderNodePath(), frame.fourcc, frame.width, frame.height, g.width,
                      g.height, g.filter, error));
        CHECK(gl.bindTarget(glSurface.target, error));
        // Twice as well, so the time is the steady one: the first pays for
        // the context's first draw.
        CHECK(gl.convert(frame, cursor, convert::CursorDraw{}, error));
        auto t0 = std::chrono::steady_clock::now();
        CHECK(gl.convert(frame, cursor, convert::CursorDraw{}, error));
        const double glMs = msSince(t0);
        gl.detachThread();
        if (!error.empty()) std::fprintf(stderr, "  GL: %s\n", error.c_str());

        convert::VulkanConvert vk;
        error.clear();
        if (!vk.init(kms.renderNodePath(), frame.fourcc, frame.width, frame.height, g.width,
                     g.height, g.filter, error)) {
            std::fprintf(stderr, "  skipped: no Vulkan conversion here (%s)\n", error.c_str());
            gl.stop();
            freeSurface(display, glSurface);
            freeSurface(display, vkSurface);
            break;
        }
        CHECK(vk.bindTarget(vkSurface.target, error));
        // Twice: the second finds the scanout in its cache and the planes back
        // from VA-API, the path every frame after the first takes.
        CHECK(vk.convert(frame, cursor, convert::CursorDraw{}, error));
        t0 = std::chrono::steady_clock::now();
        CHECK(vk.convert(frame, cursor, convert::CursorDraw{}, error));
        const double vkMs = msSince(t0);
        if (!error.empty()) std::fprintf(stderr, "  Vulkan: %s\n", error.c_str());
        CHECK(!vk.lost());

        std::vector<uint8_t> glY, glUv, vkY, vkUv;
        CHECK(readBack(display, glSurface, glY, glUv));
        CHECK(readBack(display, vkSurface, vkY, vkUv));
        const Difference luma = compare(glY, vkY);
        const Difference chroma = compare(glUv, vkUv);
        unsigned minLuma = 255, maxLuma = 0;
        for (uint8_t v : vkY) {
            minLuma = std::min<unsigned>(minLuma, v);
            maxLuma = std::max<unsigned>(maxLuma, v);
        }
        std::fprintf(stderr,
                     "  %s: luma max %d, %zu over 1, mean %.4f; chroma max %d, %zu over 1, mean "
                     "%.4f; luma %u..%u; GL %.2f ms, Vulkan %.2f ms (%s)\n",
                     g.what, luma.max, luma.overOne, luma.mean, chroma.max, chroma.overOne,
                     chroma.mean, minLuma, maxLuma, glMs, vkMs, vk.priority().c_str());
        // The same shaders on the same texture units: never more than one
        // value apart. A matrix, a half-texel or a flipped row would put whole
        // regions tens of values apart. Where they differ by one is where the
        // two writes round a tie their own way — GL's render target and
        // imageStore — which the chroma meets more often: 1.5 % of luma and
        // 8 % of chroma samples on the 780M (28/09/2026), never two.
        CHECK(luma.max <= 1);
        CHECK(chroma.max <= 1);
        CHECK(luma.mean < 0.05);
        CHECK(chroma.mean < 0.15);
        // Real pixels in the legal range, not two identical black surfaces.
        CHECK(maxLuma > minLuma);
        CHECK(minLuma >= 16);
        CHECK(maxLuma <= 235);

        vk.stop();
        gl.stop();
        freeSurface(display, glSurface);
        freeSurface(display, vkSurface);
    }

    SECTION("Linux — a Vulkan conversion that cannot read a frame gives up, and says so");
    {
        Surface surface;
        convert::VulkanConvert vk;
        if (makeSurface(display, frame.width, frame.height, surface) &&
            vk.init(kms.renderNodePath(), frame.fourcc, frame.width, frame.height, frame.width,
                    frame.height, convert::ScaleFilter::Bilinear, error) &&
            vk.bindTarget(surface.target, error)) {
            // Shared memory, as a portal may hand over: no DMA-BUF to import.
            capture::KmsFrame shm = frame;
            for (int& fd : shm.fds)
                fd = -1;
            shm.planeCount = 0;
            error.clear();
            CHECK(!vk.convert(shm, cursor, convert::CursorDraw{}, error));
            CHECK(!error.empty());
            // lost() is what the session reads to convert through GL instead.
            CHECK(vk.lost());
            std::fprintf(stderr, "  refused as it should: %s\n", error.c_str());
        } else {
            std::fprintf(stderr, "  skipped: %s\n", error.c_str());
        }
        vk.stop();
        freeSurface(display, surface);
    }

    kms.release();
    kms.stop();
    vaTerminate(display);
    ::close(render);
#endif
}
