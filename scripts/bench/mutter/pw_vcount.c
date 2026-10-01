/*
 * pw_vcount — count what a PipeWire video node delivers, once a second.
 *
 *   pw_vcount <node id> [seconds] [width height] [fps] [options]
 *     options, comma-separated: dmabuf, size=pin|range, rate=max|range|product
 *     (seconds 0: until SIGTERM)
 *
 * A bench probe for plan « Idées Punktfunk » C1 (Mutter's own virtual
 * monitors): it connects to the node as a plain consumer, asks for the given
 * size (Mutter sizes a virtual monitor made without `modes` after what its
 * consumer negotiates) and rate (see addSizeAndRate), for the header, cursor
 * and damage metadata, and prints one JSON line per second: frames with
 * pixels, buffers without (a cursor-only update), cursor metadata seen, the
 * longest gap between two frames. Shared memory, or DMA-BUF with a linear or
 * implicit modifier; the frames are never read, only counted.
 *
 * Builds against PipeWire 0.3.48 (Ubuntu 22.04) and later:
 *   gcc -O2 -o pw_vcount pw_vcount.c $(pkg-config --cflags --libs libpipewire-0.3)
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define CURSOR_META_SIZE(w, h) \
    (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)

enum { RATE_MAX, RATE_RANGE, RATE_PRODUCT };

struct probe {
    int dmabuf, pinSize, rateMode;
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    struct spa_hook listener;
    struct spa_source* timer;
    int seconds;
    int elapsed;
    /* this second */
    unsigned frames, empty, corrupted, cursorMeta, cursorBitmap, damageMeta;
    double maxGapMs;
    /* whole run */
    unsigned long long totalFrames, totalEmpty, totalCursor;
    uint64_t lastFrameNs;
    int cursorX, cursorY;
    uint32_t centre, corner;
    int sampled;
    const char* dump;
    int dumped;
    int haveFormat;
    struct spa_video_info_raw raw;
};

static uint64_t nowNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One frame as a binary PPM (BGRx/RGBx read as 4 bytes a pixel), at the
 * path the "dump=" option names: what the monitor shows, to look at. */
static void dumpFrame(struct probe* p, const uint8_t* px, int stride)
{
    FILE* f = fopen(p->dump, "wb");
    p->dumped = 1;
    if (!f) return;
    const int w = (int)p->raw.size.width, h = (int)p->raw.size.height;
    const int bgr = p->raw.format == SPA_VIDEO_FORMAT_BGRx || p->raw.format == SPA_VIDEO_FORMAT_BGRA;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* s = px + (size_t)y * stride + (size_t)x * 4;
            const uint8_t rgb[3] = {bgr ? s[2] : s[0], s[1], bgr ? s[0] : s[2]};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void onProcess(void* userdata)
{
    struct probe* p = userdata;
    struct pw_buffer* b = pw_stream_dequeue_buffer(p->stream);
    if (!b) return;
    struct spa_buffer* buf = b->buffer;

    struct spa_meta_cursor* mc =
        spa_buffer_find_meta_data(buf, SPA_META_Cursor, sizeof(*mc));
    if (mc && spa_meta_cursor_is_valid(mc)) {
        p->cursorMeta++;
        p->cursorX = mc->position.x;
        p->cursorY = mc->position.y;
        if (mc->bitmap_offset) p->cursorBitmap++;
    }
    struct spa_meta_region* dmg =
        spa_buffer_find_meta_data(buf, SPA_META_VideoDamage, sizeof(*dmg));
    if (dmg && dmg->region.size.width) p->damageMeta++;

    const struct spa_chunk* chunk = buf->datas[0].chunk;
    if (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) {
        p->corrupted++;
    } else if (chunk->size == 0) {
        p->empty++;
    } else {
        const uint64_t t = nowNs();
        if (p->lastFrameNs) {
            const double gap = (double)(t - p->lastFrameNs) / 1e6;
            if (gap > p->maxGapMs) p->maxGapMs = gap;
        }
        p->lastFrameNs = t;
        p->frames++;
        /* Two pixels of the last frame, when it is mapped (shared memory):
         * the centre and a corner, to tell what the monitor shows. */
        const uint8_t* px = buf->datas[0].data;
        if (px && p->raw.size.width) {
            const int stride = chunk->stride ? chunk->stride : (int)p->raw.size.width * 4;
            const uint8_t* c = px + chunk->offset + (size_t)(p->raw.size.height / 2) * stride +
                               (size_t)(p->raw.size.width / 2) * 4;
            const uint8_t* k = px + chunk->offset + (size_t)8 * stride + 8 * 4;
            p->centre = (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
            p->corner = (uint32_t)k[0] << 16 | (uint32_t)k[1] << 8 | k[2];
            p->sampled = 1;
            if (p->dump && p->elapsed == 3 && !p->dumped) dumpFrame(p, px + chunk->offset, stride);
        }
    }
    pw_stream_queue_buffer(p->stream, b);
}

static void onParamChanged(void* userdata, uint32_t id, const struct spa_pod* param)
{
    struct probe* p = userdata;
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t mediaType, mediaSubtype;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_video || mediaSubtype != SPA_MEDIA_SUBTYPE_raw) return;
    memset(&p->raw, 0, sizeof(p->raw));
    if (spa_format_video_raw_parse(param, &p->raw) < 0) return;
    p->haveFormat = 1;
    const int modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
    printf("{\"format\":%u,\"w\":%u,\"h\":%u,\"rate\":\"%u/%u\",\"max_rate\":\"%u/%u\","
           "\"memory\":\"%s\",\"modifier\":\"0x%llx\"}\n",
           p->raw.format, p->raw.size.width, p->raw.size.height, p->raw.framerate.num,
           p->raw.framerate.denom, p->raw.max_framerate.num, p->raw.max_framerate.denom,
           modifier ? "dmabuf" : "shm", modifier ? (unsigned long long)p->raw.modifier : 0ull);
    fflush(stdout);

    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[4];
    const int stride = SPA_ROUND_UP_N(p->raw.size.width * 4, 4);
    if (modifier) /* the compositor sizes its DMA-BUFs itself */
        params[0] = spa_pod_builder_add_object(
            &bld, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_DmaBuf));
    else
        params[0] = spa_pod_builder_add_object(
            &bld, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * (int)p->raw.size.height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    params[1] = spa_pod_builder_add_object(
        &bld, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object(
        &bld, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(384, 384), CURSOR_META_SIZE(1, 1),
                                 CURSOR_META_SIZE(512, 512)));
    params[3] = spa_pod_builder_add_object(
        &bld, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * 16,
                                 sizeof(struct spa_meta_region) * 1,
                                 sizeof(struct spa_meta_region) * 16));
    pw_stream_update_params(p->stream, params, 4);
}

static void onStateChanged(void* userdata, enum pw_stream_state old,
                           enum pw_stream_state state, const char* error)
{
    struct probe* p = userdata;
    (void)old;
    printf("{\"state\":\"%s\"%s%s%s}\n", pw_stream_state_as_string(state),
           error ? ",\"error\":\"" : "", error ? error : "", error ? "\"" : "");
    fflush(stdout);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
        pw_main_loop_quit(p->loop);
}

static const struct pw_stream_events kEvents = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = onStateChanged,
    .param_changed = onParamChanged,
    .process = onProcess,
};

static void onTimer(void* userdata, uint64_t expirations)
{
    struct probe* p = userdata;
    (void)expirations;
    p->elapsed++;
    p->totalFrames += p->frames;
    p->totalEmpty += p->empty;
    p->totalCursor += p->cursorMeta;
    printf("{\"t\":%d,\"fps\":%u,\"empty\":%u,\"corrupted\":%u,\"cursor\":%u,"
           "\"cursor_bitmap\":%u,\"cursor_xy\":[%d,%d],\"damage\":%u,\"max_gap_ms\":%.1f,"
           "\"centre\":\"%06x\",\"corner\":\"%06x\"}\n",
           p->elapsed, p->frames, p->empty, p->corrupted, p->cursorMeta, p->cursorBitmap,
           p->cursorX, p->cursorY, p->damageMeta, p->maxGapMs, p->sampled ? p->centre : 0,
           p->sampled ? p->corner : 0);
    fflush(stdout);
    p->frames = p->empty = p->corrupted = p->cursorMeta = p->cursorBitmap = p->damageMeta = 0;
    p->maxGapMs = 0;
    if (p->seconds > 0 && p->elapsed >= p->seconds) pw_main_loop_quit(p->loop);
}

/* How the size and the rate are asked for:
 *   size  pin (default) = the one size; range = preferred size, any accepted;
 *   rate  max (default) = maxFramerate exactly fps (Mutter makes a virtual
 *         monitor at the negotiated maxFramerate); range = maxFramerate up to
 *         fps; product = framerate up to fps, no maxFramerate (PortalCapture). */
static void addSizeAndRate(struct spa_pod_builder* b, const struct probe* p, int w, int h, int fps)
{
    if (p->pinSize)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(w, h)), 0);
    else
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                            SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(w, h),
                                                           &SPA_RECTANGLE(1, 1),
                                                           &SPA_RECTANGLE(8192, 8192)),
                            0);
    if (p->rateMode == RATE_MAX)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
                            SPA_FORMAT_VIDEO_maxFramerate,
                            SPA_POD_Fraction(&SPA_FRACTION(fps, 1)), 0);
    else if (p->rateMode == RATE_RANGE)
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
                            SPA_FORMAT_VIDEO_maxFramerate,
                            SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(fps, 1),
                                                          &SPA_FRACTION(0, 1),
                                                          &SPA_FRACTION(fps, 1)),
                            0);
    else
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
                            SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(fps, 1),
                                                          &SPA_FRACTION(0, 1),
                                                          &SPA_FRACTION(fps, 1)),
                            0);
}

static void onQuit(void* userdata, int signal)
{
    struct probe* p = userdata;
    (void)signal;
    pw_main_loop_quit(p->loop);
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <node id> [seconds] [width height] [fps] [options]\n",
                argv[0]);
        return 2;
    }
    const uint32_t node = (uint32_t)strtoul(argv[1], NULL, 10);
    const int seconds = argc > 2 ? atoi(argv[2]) : 10;
    const int width = argc > 4 ? atoi(argv[3]) : 1920;
    const int height = argc > 4 ? atoi(argv[4]) : 1080;
    const int maxFps = argc > 5 ? atoi(argv[5]) : 240;

    pw_init(&argc, &argv);
    struct probe p;
    memset(&p, 0, sizeof(p));
    p.seconds = seconds;
    p.pinSize = 1;
    p.rateMode = RATE_MAX;
    const char* opts = argc > 6 ? argv[6] : "";
    p.dmabuf = strstr(opts, "dmabuf") != NULL;
    if (strstr(opts, "size=range")) p.pinSize = 0;
    if (strstr(opts, "rate=range")) p.rateMode = RATE_RANGE;
    if (strstr(opts, "rate=product")) p.rateMode = RATE_PRODUCT;
    const char* dump = strstr(opts, "dump=");
    if (dump) {
        static char path[256];
        snprintf(path, sizeof(path), "%s", dump + 5);
        path[strcspn(path, ",")] = 0;
        p.dump = path;
    }
    p.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(p.loop), SIGTERM, onQuit, &p);
    pw_loop_add_signal(pw_main_loop_get_loop(p.loop), SIGINT, onQuit, &p);

    /* The node is named by its id, as Mutter's PipeWireStreamAdded gives it:
     * PW_KEY_TARGET_OBJECT would read a number as an object serial. */
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
        "Screen", NULL);
    p.stream = pw_stream_new_simple(pw_main_loop_get_loop(p.loop), "mw-pw-vcount", props,
                                    &kEvents, &p);

    uint8_t buffer[4096];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod* params[8];
    uint32_t n = 0;
    static const uint32_t kFormats[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                                        SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA};
    /* DMA-BUF first (as the product offers it): linear, or whatever the
     * compositor's allocator picks, "do not fixate"; then shared memory. */
    for (unsigned i = 0; p.dmabuf && i < 4; i++) {
        struct spa_pod_frame obj, choice;
        spa_pod_builder_push_object(&bld, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&bld, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format, SPA_POD_Id(kFormats[i]), 0);
        spa_pod_builder_prop(&bld, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY | (1u << 4) /* DONT_FIXATE */);
        spa_pod_builder_push_choice(&bld, &choice, SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(&bld, 0);                     /* default: linear */
        spa_pod_builder_long(&bld, 0);                     /* DRM_FORMAT_MOD_LINEAR */
        spa_pod_builder_long(&bld, 0x00ffffffffffffffLL);  /* DRM_FORMAT_MOD_INVALID */
        spa_pod_builder_pop(&bld, &choice);
        addSizeAndRate(&bld, &p, width, height, maxFps);
        params[n++] = spa_pod_builder_pop(&bld, &obj);
    }
    {
        struct spa_pod_frame obj;
        spa_pod_builder_push_object(&bld, &obj, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&bld, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                            SPA_FORMAT_VIDEO_format,
                            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx,
                                                   SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                                                   SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA),
                            0);
        addSizeAndRate(&bld, &p, width, height, maxFps);
        params[n++] = spa_pod_builder_pop(&bld, &obj);
    }

    const int rc = pw_stream_connect(
        p.stream, PW_DIRECTION_INPUT, node,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, n);
    if (rc < 0) {
        fprintf(stderr, "pw_stream_connect: %s\n", spa_strerror(rc));
        return 1;
    }

    p.timer = pw_loop_add_timer(pw_main_loop_get_loop(p.loop), onTimer, &p);
    struct timespec first = {1, 0}, every = {1, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(p.loop), p.timer, &first, &every, false);

    pw_main_loop_run(p.loop);
    printf("{\"done\":1,\"seconds\":%d,\"frames\":%llu,\"empty\":%llu,\"cursor\":%llu}\n",
           p.elapsed, p.totalFrames, p.totalEmpty, p.totalCursor);
    pw_stream_destroy(p.stream);
    pw_main_loop_destroy(p.loop);
    pw_deinit();
    return 0;
}
