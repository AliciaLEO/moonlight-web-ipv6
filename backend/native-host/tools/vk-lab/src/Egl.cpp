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

// `egl` — the witness of `queues` (plan pipeline-video-d3d12-v2, Phase 13,
// C13.1): the chain of today, GLES on the graphics ring, as GlConvert sets it
// up (GBM on the render node, a surfaceless ES3 context), with the same
// Lanczos-2 conversion drawn in two passes and waited for with glFinish.
//
// The one thing that changes from run to run is EGL_IMG_context_priority —
// LOW, MEDIUM (what GlConvert gets today, asking nothing), HIGH, and REALTIME
// where EGL_NV_context_priority_realtime exists. The priority actually granted
// is read back from the context: Mesa gives MEDIUM without a word when the
// kernel refuses more (amdgpu wants CAP_SYS_NICE above it). If a HIGH context
// alone gets most of what a Vulkan queue would, the chain of today can have
// it with one attribute.

#include "Egl.h"

#include "Json.h"
#include "Lab.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef EGL_CONTEXT_PRIORITY_LEVEL_IMG
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG 0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG 0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG 0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG 0x3103
#endif
#ifndef EGL_CONTEXT_PRIORITY_REALTIME_NV
#define EGL_CONTEXT_PRIORITY_REALTIME_NV 0x3357
#endif

namespace lab {
namespace {

struct Options
{
    std::string renderNode = "/dev/dri/renderD128";
    int sourceW = 2560, sourceH = 1440;
    int targetW = 1920, targetH = 1080;
    int fps = 60;
    int seconds = 6;
    std::vector<std::string> priorities = {"realtime", "high", "medium", "low"};
    std::string json;
};

const char* const kVertex = R"(#version 300 es
void main()
{
    // One triangle over the whole target.
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)";

// The conversion of the lab's `queues` (shaders/convert.comp), as a fragment
// shader: Lanczos-2 into luma, then chroma at half size.
const char* const kFragment = R"(#version 300 es
precision highp float;
uniform sampler2D source;
uniform vec2 sourceSize;
uniform vec2 targetSize;
uniform int pass;
out vec4 color;

float lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

vec3 resample(vec2 p, vec2 size)
{
    vec2 scale = sourceSize / size;
    vec2 at = (p + 0.5) * scale - 0.5;
    vec2 base = floor(at);
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int j = -1; j <= 2; ++j) {
        for (int i = -1; i <= 2; ++i) {
            vec2 tap = base + vec2(float(i), float(j));
            float w = lanczos2(at.x - tap.x) * lanczos2(at.y - tap.y);
            sum += w * textureLod(source, (tap + 0.5) / sourceSize, 0.0).rgb;
            weights += w;
        }
    }
    return sum / weights;
}

void main()
{
    vec2 p = floor(gl_FragCoord.xy);
    if (pass == 0) {
        vec3 rgb = resample(p, targetSize);
        float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        color = vec4((16.0 + 219.0 * y) / 255.0, 0.0, 0.0, 1.0);
    } else {
        vec3 rgb = resample(p, targetSize * 0.5);
        float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        float u = (rgb.b - y) / 1.8556;
        float v = (rgb.r - y) / 1.5748;
        color = vec4((128.0 + 224.0 * u) / 255.0, (128.0 + 224.0 * v) / 255.0, 0.0, 1.0);
    }
}
)";

struct Result
{
    std::string asked;
    std::string granted;
    std::string error;
    Stats wall;
    size_t frames = 0;
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
        if (a == "--render-node") {
            if (!next(o.renderNode)) return false;
        } else if (a == "--source") {
            if (!next(v) || std::sscanf(v.c_str(), "%dx%d", &o.sourceW, &o.sourceH) != 2)
                return false;
        } else if (a == "--target") {
            if (!next(v) || std::sscanf(v.c_str(), "%dx%d", &o.targetW, &o.targetH) != 2)
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
        } else if (a == "--json") {
            if (!next(o.json)) return false;
        } else {
            say("unknown option %s\n", a.c_str());
            return false;
        }
    }
    for (const std::string& p : o.priorities)
        if (p != "low" && p != "medium" && p != "high" && p != "realtime") return false;
    return o.fps > 0 && o.seconds > 1 && !o.priorities.empty() && o.targetW % 2 == 0 &&
           o.targetH % 2 == 0;
}

std::string priorityName(EGLint value)
{
    switch (value) {
    case EGL_CONTEXT_PRIORITY_LOW_IMG: return "low";
    case EGL_CONTEXT_PRIORITY_MEDIUM_IMG: return "medium";
    case EGL_CONTEXT_PRIORITY_HIGH_IMG: return "high";
    case EGL_CONTEXT_PRIORITY_REALTIME_NV: return "realtime";
    default: return hex(static_cast<unsigned long long>(value));
    }
}

EGLint priorityValue(const std::string& name)
{
    if (name == "low") return EGL_CONTEXT_PRIORITY_LOW_IMG;
    if (name == "high") return EGL_CONTEXT_PRIORITY_HIGH_IMG;
    if (name == "realtime") return EGL_CONTEXT_PRIORITY_REALTIME_NV;
    return EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
}

GLuint compile(GLenum type, const char* text, std::string& error)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        error = std::string("shader: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/// A GL render target: a texture of @p format and its framebuffer.
struct Target
{
    GLuint texture = 0;
    GLuint framebuffer = 0;
    int w = 0, h = 0;
};

bool makeTarget(Target& t, GLenum internal, int w, int h)
{
    t.w = w;
    t.h = h;
    glGenTextures(1, &t.texture);
    glBindTexture(GL_TEXTURE_2D, t.texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, w, h);
    glGenFramebuffers(1, &t.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

/// One priority: a context asking for it, the conversion drawn at the rate
/// asked, glFinish after each, the times kept after the first second.
Result runOne(EGLDisplay display, EGLConfig config, bool realtimeExt, const std::string& priority,
              const Options& o, const std::vector<uint8_t>& source)
{
    Result r;
    r.asked = priority;
    if (priority == "realtime" && !realtimeExt) {
        r.error = "no EGL_NV_context_priority_realtime";
        return r;
    }
    const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_PRIORITY_LEVEL_IMG,
                              priorityValue(priority), EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, attribs);
    if (context == EGL_NO_CONTEXT) {
        r.error = "eglCreateContext failed (0x" + std::to_string(eglGetError()) + ")";
        return r;
    }
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        r.error = "eglMakeCurrent failed";
        eglDestroyContext(display, context);
        return r;
    }
    EGLint granted = 0;
    eglQueryContext(display, context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &granted);
    r.granted = priorityName(granted);

    std::string error;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex, error);
    const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kFragment, error) : 0;
    GLuint program = 0;
    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) error = "link failed";
    }
    GLuint sourceTexture = 0;
    Target luma, chroma;
    if (error.empty()) {
        glGenTextures(1, &sourceTexture);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, o.sourceW, o.sourceH);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, o.sourceW, o.sourceH, GL_RGBA, GL_UNSIGNED_BYTE,
                        source.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!makeTarget(luma, GL_R8, o.targetW, o.targetH) ||
            !makeTarget(chroma, GL_RG8, o.targetW / 2, o.targetH / 2))
            error = "incomplete framebuffer";
    }
    if (!error.empty()) {
        r.error = error;
    } else {
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "source"), 0);
        glUniform2f(glGetUniformLocation(program, "sourceSize"), static_cast<float>(o.sourceW),
                    static_cast<float>(o.sourceH));
        glUniform2f(glGetUniformLocation(program, "targetSize"), static_cast<float>(o.targetW),
                    static_cast<float>(o.targetH));
        const GLint pass = glGetUniformLocation(program, "pass");
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sourceTexture);
        glFinish();
        std::vector<double> wall;
        const int frames = o.fps * o.seconds;
        const int64_t start = nowUs() + 20000;
        for (int n = 0; n < frames; ++n) {
            sleepUntilUs(start + static_cast<int64_t>(n) * 1000000 / o.fps);
            const int64_t t0 = nowUs();
            glBindFramebuffer(GL_FRAMEBUFFER, luma.framebuffer);
            glViewport(0, 0, luma.w, luma.h);
            glUniform1i(pass, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindFramebuffer(GL_FRAMEBUFFER, chroma.framebuffer);
            glViewport(0, 0, chroma.w, chroma.h);
            glUniform1i(pass, 1);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFinish(); // what GlConvert does before handing the surface to VA-API
            const double ms = static_cast<double>(nowUs() - t0) / 1000.0;
            if (n >= o.fps) wall.push_back(ms);
        }
        r.wall = stats(wall);
        r.frames = wall.size();
        glDeleteVertexArrays(1, &vao);
    }
    for (Target* t : {&luma, &chroma}) {
        if (t->framebuffer) glDeleteFramebuffers(1, &t->framebuffer);
        if (t->texture) glDeleteTextures(1, &t->texture);
    }
    if (sourceTexture) glDeleteTextures(1, &sourceTexture);
    if (program) glDeleteProgram(program);
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    return r;
}

} // namespace

void eglUsage()
{
    say("mw-vk-lab egl [--render-node /dev/dri/renderD128] [--priorities "
        "realtime,high,medium,low]\n"
        "              [--fps 60] [--seconds 6] [--source 2560x1440] [--target 1920x1080]\n"
        "              [--json x.json]\n"
        "  The witness of `queues`: the same conversion drawn in GLES on the graphics ring,\n"
        "  as GlConvert does, glFinish after each frame, in a context asking each priority\n"
        "  of EGL_IMG_context_priority in turn; the priority granted is read back. Run it\n"
        "  beside the same load as `queues`.\n");
}

int runEgl(int argc, char** argv)
{
    Options o;
    try {
        if (!parse(argc, argv, o)) {
            eglUsage();
            return 2;
        }
    } catch (...) {
        eglUsage();
        return 2;
    }
    const int fd = ::open(o.renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        say("mw-vk-lab egl: cannot open %s\n", o.renderNode.c_str());
        return 1;
    }
    gbm_device* gbm = gbm_create_device(fd);
    auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = gbm && getPlatformDisplay
                             ? getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                             : EGL_NO_DISPLAY;
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
        say("mw-vk-lab egl: EGL does not initialize on %s\n", o.renderNode.c_str());
        if (gbm) gbm_device_destroy(gbm);
        ::close(fd);
        return 1;
    }
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    const bool priorityExt = extensions && std::strstr(extensions, "EGL_IMG_context_priority");
    const bool realtimeExt =
        extensions && std::strstr(extensions, "EGL_NV_context_priority_realtime");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0,
                                    EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    eglChooseConfig(display, configAttribs, &config, 1, &count);

    std::vector<uint8_t> source(static_cast<size_t>(o.sourceW) * o.sourceH * 4);
    for (int y = 0; y < o.sourceH; ++y) {
        for (int x = 0; x < o.sourceW; ++x) {
            uint8_t* p = source.data() + (static_cast<size_t>(y) * o.sourceW + x) * 4;
            const bool stripe = y % 90 < 30 && (x / 2) % 2;
            const bool block = (x / 16 + y / 16) % 7 == 0;
            p[0] = static_cast<uint8_t>(stripe ? 230 : (x * 255) / o.sourceW);
            p[1] = static_cast<uint8_t>(stripe ? 230 : (y * 255) / o.sourceH);
            p[2] = static_cast<uint8_t>(block ? 40 : 160);
            p[3] = 255;
        }
    }

    say("mw-vk-lab egl — %s, %s\n", hostName().c_str(), nowText().c_str());
    say("  EGL %d.%d on %s, EGL_IMG_context_priority %s, realtime %s, CAP_SYS_NICE %s\n", major,
        minor, o.renderNode.c_str(), priorityExt ? "yes" : "no", realtimeExt ? "yes" : "no",
        capEffective(kCapSysNice) ? "effective" : "not effective");
    say("  %dx%d -> %dx%d Lanczos-2 in two passes, glFinish, %d i/s, %d s a priority\n", o.sourceW,
        o.sourceH, o.targetW, o.targetH, o.fps, o.seconds);
    std::vector<Result> results;
    for (const std::string& priority : o.priorities) {
        const std::string started = nowText();
        Result r = runOne(display, config, realtimeExt, priority, o, source);
        if (!r.error.empty())
            say("  %-8s %s: %s\n", priority.c_str(), started.c_str() + 11, r.error.c_str());
        else
            say("  %-8s %s: granted %s, wall %.2f / p50 %.2f / p99 %.2f / max %.2f ms (%zu "
                "frames)\n",
                priority.c_str(), started.c_str() + 11, r.granted.c_str(), r.wall.mean, r.wall.p50,
                r.wall.p99, r.wall.max, r.frames);
        results.push_back(r);
    }
    eglTerminate(display);
    gbm_device_destroy(gbm);
    ::close(fd);

    if (!o.json.empty()) {
        Json j;
        j.beginObject();
        j.field("tool", "mw-vk-lab egl");
        j.field("date", nowText());
        j.field("host", hostName());
        j.field("capSysNiceEffective", capEffective(kCapSysNice));
        j.key("runs").beginArray();
        for (const Result& r : results) {
            j.beginObject();
            j.field("asked", r.asked);
            j.field("granted", r.granted);
            j.field("error", r.error);
            j.field("frames", static_cast<unsigned long long>(r.frames));
            j.field("wallMean", r.wall.mean);
            j.field("wallP50", r.wall.p50);
            j.field("wallP99", r.wall.p99);
            j.field("wallMax", r.wall.max);
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
