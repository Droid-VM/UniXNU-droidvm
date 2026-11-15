// vencprobe: Qualcomm's msm_vidc encoder (/dev/video33) driven on the phone as root, no VM -- what crosvm's direct
// encoder (devices/src/virtio/media/android_codec_backend/vidc_enc.rs) does: the kernel's stateful encoder sequence
// (CAPTURE format, OUTPUT format, frame rate, controls, buffers, both queues on), raw NV12 frames copied into the
// driver's layout, the bitstream written to a file. Every step's answer, every buffer and the timing are printed.
//
// Build: aarch64-linux-gnu-gcc -static -O2 -o vencprobe vencprobe.c; run as root with a tightly packed NV12 file
// (ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -frames 60 -pix_fmt nv12 -f rawvideo in.nv12), then check the output
// on a PC (ffmpeg -i out.h264 -f rawvideo -pix_fmt nv12 dec.nv12, and PSNR against the input).
//
//   vencprobe IN.nv12 W H h264|hevc OUT [-b BITRATE] [-r FPS] [-g GOP] [-c] [-m mmap|heap] [-q] [-S]
//     -c  constant bitrate (default variable)
//     -m  CAPTURE (bitstream) buffers: mmap (the driver's) or heap (dma-heap buffers of ours, DMABUF); OUTPUT is always
//         dma-heap buffers
//     -q  list every control the driver has, then go on
//     -S  stream headers in a buffer of their own (HEADER_MODE_SEPARATE; default joined with the first frame)
//     -L  lockstep: one frame with the driver at a time, the next only after its coded frame is out (the per-frame latency)
//     -P MS  poll timeout (default 100); -I poll for POLLIN only
//     -2  after the drain, what crosvm's flush does (STREAMOFF + STREAMON(OUTPUT), ENC_CMD_START, the CAPTURE buffers in
//         again), then the frames once more: the second round is written to OUT too
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RAW_BUFFERS 8
#define BIT_BUFFERS 8
#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

static int dev, heap = -1;
static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define LOG(...) do { printf("%8.3f ", now() - t0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int x(unsigned long req, void *arg) { return ioctl(dev, req, arg) < 0 ? errno : 0; }

struct buf { int fd; uint8_t *map; size_t len; int queued; };
static struct buf raws[RAW_BUFFERS], bits[BIT_BUFFERS];

static int heap_alloc(size_t len) {
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { LOG("dma-heap alloc %zu: errno %d", len, errno); exit(1); }
    return a.fd;
}

static void list_controls(void) {
    struct v4l2_query_ext_ctrl q = { .id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND };
    while (!x(VIDIOC_QUERY_EXT_CTRL, &q)) {
        if (q.type != V4L2_CTRL_TYPE_CTRL_CLASS)
            LOG("  ctrl %#010x %-40s type %u %lld..%lld step %llu default %lld flags %#x", q.id, q.name, q.type,
                (long long)q.minimum, (long long)q.maximum, (unsigned long long)q.step, (long long)q.default_value, q.flags);
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    }
}

static void set(unsigned id, int value, const char *name) {
    struct v4l2_ext_control c = { .id = id, .value = value };
    struct v4l2_ext_controls cs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &c };
    int r = x(VIDIOC_S_EXT_CTRLS, &cs);
    struct v4l2_ext_control g = { .id = id };
    struct v4l2_ext_controls gs = { .which = V4L2_CTRL_WHICH_CUR_VAL, .count = 1, .controls = &g };
    int rg = x(VIDIOC_G_EXT_CTRLS, &gs);
    LOG("S_EXT_CTRLS %s = %d: %d (reads back %d, %d)", name, value, r, g.value, rg);
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "vencprobe IN.nv12 W H h264|hevc OUT [-b BPS] [-r FPS] [-g GOP] [-c] [-m mmap|heap] [-q] [-S]\n"); return 2; }
    t0 = now();
    const unsigned w = atoi(argv[2]), h = atoi(argv[3]);
    const int hevc = !strcmp(argv[4], "hevc");
    int bitrate = 4000000, fps = 30, gop = 30, cbr = 0, mmapcap = 1, query = 0, separate = 0, rounds = 1, lockstep = 0, poll_ms = 100, poll_events = POLLIN | POLLOUT | POLLPRI;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) bitrate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")) cbr = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mmapcap = !strcmp(argv[++i], "mmap");
        else if (!strcmp(argv[i], "-q")) query = 1;
        else if (!strcmp(argv[i], "-S")) separate = 1;
        else if (!strcmp(argv[i], "-2")) rounds = 2;
        else if (!strcmp(argv[i], "-L")) lockstep = 1;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) poll_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-I")) poll_events = POLLIN;
    }
    int in = open(argv[1], O_RDONLY); struct stat st;
    if (in < 0 || fstat(in, &st)) { LOG("cannot open %s", argv[1]); return 1; }
    const size_t frame = (size_t)w * h * 3 / 2;
    const int nframes = st.st_size / frame;
    uint8_t *data = malloc(st.st_size);
    if (read(in, data, st.st_size) != st.st_size) return 1;
    close(in);
    FILE *out = fopen(argv[5], "wb");
    if (!out) { LOG("cannot write %s", argv[5]); return 1; }
    LOG("%s: %d NV12 frames %ux%u -> %s %s, %d bps %s, %d fps, gop %d, bitstream %s", argv[1], nframes, w, h, argv[4],
        argv[5], bitrate, cbr ? "CBR" : "VBR", fps, gop, mmapcap ? "MMAP" : "dma-heap");

    dev = open("/dev/video33", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev < 0) { LOG("open /dev/video33: errno %d", errno); return 1; }
    heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heap < 0) { LOG("open dma heap: errno %d", errno); return 1; }
    struct v4l2_capability cap = {0};
    x(VIDIOC_QUERYCAP, &cap);
    LOG("driver %s card %s bus %s version %#x caps %#x", cap.driver, cap.card, cap.bus_info, cap.version, cap.device_caps);
    for (int t = 0; t < 2; t++) {
        char list[256] = "";
        for (unsigned i = 0; i < 32; i++) {
            struct v4l2_fmtdesc d = { .index = i, .type = t ? OUT : CAP };
            if (x(VIDIOC_ENUM_FMT, &d)) break;
            char s[16]; snprintf(s, sizeof s, "%.4s ", (char *)&d.pixelformat); strcat(list, s);
        }
        LOG("%s formats: %s", t ? "OUTPUT (raw)" : "CAPTURE (coded)", list);
    }
    if (query) list_controls();

    // 1. The coded format.
    struct v4l2_format cf = { .type = CAP };
    cf.fmt.pix_mp.pixelformat = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    cf.fmt.pix_mp.width = w; cf.fmt.pix_mp.height = h; cf.fmt.pix_mp.num_planes = 1;
    cf.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
    int r = x(VIDIOC_S_FMT, &cf); if (r) { LOG("S_FMT(CAPTURE): errno %d", r); return 1; }
    x(VIDIOC_G_FMT, &cf);
    LOG("CAPTURE %.4s %ux%u sizeimage %u", (char *)&cf.fmt.pix_mp.pixelformat, cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
        cf.fmt.pix_mp.plane_fmt[0].sizeimage);
    // 2. The raw format.
    struct v4l2_format of = { .type = OUT };
    of.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    of.fmt.pix_mp.width = w; of.fmt.pix_mp.height = h; of.fmt.pix_mp.num_planes = 1;
    r = x(VIDIOC_S_FMT, &of); if (r) { LOG("S_FMT(OUTPUT): errno %d", r); return 1; }
    x(VIDIOC_G_FMT, &of);
    const unsigned stride = of.fmt.pix_mp.plane_fmt[0].bytesperline;
    const size_t rawlen = of.fmt.pix_mp.plane_fmt[0].sizeimage;
    LOG("OUTPUT %.4s %ux%u planes %u bytesperline %u sizeimage %zu colorspace %u", (char *)&of.fmt.pix_mp.pixelformat,
        of.fmt.pix_mp.width, of.fmt.pix_mp.height, of.fmt.pix_mp.num_planes, stride, rawlen, of.fmt.pix_mp.colorspace);
    // The chroma plane: after the luma rows the size implies (sizeimage is luma + chroma + any tail).
    const unsigned scanlines = ALIGN(h, 32);
    LOG("chroma at %u x %u = %u (sizeimage leaves %zd after a %u-row chroma)", stride, scanlines, stride * scanlines,
        (ssize_t)rawlen - (ssize_t)stride * scanlines - (ssize_t)stride * scanlines / 2, scanlines / 2);
    // The coded size follows the raw one, and the bitstream buffer size with it.
    x(VIDIOC_G_FMT, &cf);
    const size_t bitlen = ALIGN(cf.fmt.pix_mp.plane_fmt[0].sizeimage, 4096);
    LOG("CAPTURE now %ux%u sizeimage %u", cf.fmt.pix_mp.width, cf.fmt.pix_mp.height, cf.fmt.pix_mp.plane_fmt[0].sizeimage);
    struct v4l2_selection sel = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .target = V4L2_SEL_TGT_CROP };
    r = x(VIDIOC_G_SELECTION, &sel); LOG("G_SELECTION(OUTPUT, CROP): %d -> %d,%d %ux%u", r, sel.r.left, sel.r.top, sel.r.width, sel.r.height);
    // 3. The frame rate.
    struct v4l2_streamparm parm = { .type = OUT };
    parm.parm.output.timeperframe.numerator = 1; parm.parm.output.timeperframe.denominator = fps;
    r = x(VIDIOC_S_PARM, &parm);
    LOG("S_PARM(OUTPUT) 1/%d: %d -> %u/%u", fps, r, parm.parm.output.timeperframe.numerator, parm.parm.output.timeperframe.denominator);
    // 4. The controls.
    set(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "FRAME_RC_ENABLE");
    set(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, cbr ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, "BITRATE_MODE");
    set(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "BITRATE");
    set(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "GOP_SIZE");
    set(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "B_FRAMES");
    set(V4L2_CID_MPEG_VIDEO_HEADER_MODE, separate ? V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE : V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "HEADER_MODE");
    set(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1, "PREPEND_SPSPPS_TO_IDR");
    if (hevc) set(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "HEVC_PROFILE");
    else set(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H264_PROFILE");
    unsigned events[] = { V4L2_EVENT_EOS };
    for (int i = 0; i < 1; i++) { struct v4l2_event_subscription s = { .type = events[i] }; r = x(VIDIOC_SUBSCRIBE_EVENT, &s); if (r) LOG("SUBSCRIBE %u: %d", events[i], r); }
    // 5. Buffers.
    struct v4l2_requestbuffers rq = { .count = RAW_BUFFERS, .type = OUT, .memory = V4L2_MEMORY_DMABUF };
    r = x(VIDIOC_REQBUFS, &rq); if (r) { LOG("REQBUFS(OUTPUT): errno %d", r); return 1; }
    const unsigned nraw = rq.count < RAW_BUFFERS ? rq.count : RAW_BUFFERS;
    for (unsigned i = 0; i < nraw; i++) {
        raws[i].len = ALIGN(rawlen, 4096); raws[i].fd = heap_alloc(raws[i].len);
        raws[i].map = mmap(NULL, raws[i].len, PROT_READ | PROT_WRITE, MAP_SHARED, raws[i].fd, 0);
    }
    struct v4l2_requestbuffers cq = { .count = BIT_BUFFERS, .type = CAP, .memory = mmapcap ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF };
    r = x(VIDIOC_REQBUFS, &cq); if (r) { LOG("REQBUFS(CAPTURE, %s): errno %d", mmapcap ? "MMAP" : "DMABUF", r); return 1; }
    const unsigned nbit = cq.count < BIT_BUFFERS ? cq.count : BIT_BUFFERS;
    for (unsigned i = 0; i < nbit; i++) {
        if (mmapcap) {
            struct v4l2_plane pl = {0}; struct v4l2_buffer b = { .index = i, .type = CAP, .memory = V4L2_MEMORY_MMAP, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_QUERYBUF, &b); if (r) { LOG("QUERYBUF(CAPTURE %u): errno %d", i, r); return 1; }
            bits[i].len = pl.length;
            bits[i].map = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, dev, pl.m.mem_offset);
            if (bits[i].map == MAP_FAILED) { LOG("mmap CAPTURE %u: errno %d", i, errno); return 1; }
        } else {
            bits[i].len = bitlen; bits[i].fd = heap_alloc(bitlen);
            bits[i].map = mmap(NULL, bitlen, PROT_READ, MAP_SHARED, bits[i].fd, 0);
        }
    }
    LOG("%u raw buffers of %zu, %u bitstream buffers of %zu", nraw, raws[0].len, nbit, bits[0].len);
    // 6. Both queues on (CAPTURE buffers in first, as GStreamer does).
    for (unsigned i = 0; i < nbit; i++) {
        struct v4l2_plane pl = { .length = bits[i].len }; if (!mmapcap) pl.m.fd = bits[i].fd;
        struct v4l2_buffer b = { .index = i, .type = CAP, .memory = cq.memory, .m.planes = &pl, .length = 1 };
        r = x(VIDIOC_QBUF, &b); if (r) { LOG("QBUF(CAPTURE %u): errno %d", i, r); return 1; }
        bits[i].queued = 1;
    }
    int kind = CAP; r = x(VIDIOC_STREAMON, &kind); LOG("STREAMON(CAPTURE): %d", r); if (r) return 1;
    kind = OUT; r = x(VIDIOC_STREAMON, &kind); LOG("STREAMON(OUTPUT): %d", r); if (r) return 1;

    int next = 0, coded = 0, keys = 0, errors = 0, last = 0, stop_sent = 0, round = 1, base = 0;
    size_t bytes = 0; double first = 0, progress = now();
again:
    while (now() - progress < 4.0 && !last) {
        struct pollfd p = { dev, poll_events, 0 };
        int pr = poll(&p, 1, poll_ms);
        static int polls, timeouts; polls++; if (!pr) timeouts++;
        if (lockstep && polls % 20 == 0) LOG("polls %d, %d timed out, last revents %#x", polls, timeouts, p.revents);
        struct v4l2_event ev;
        while (!x(VIDIOC_DQEVENT, &ev)) LOG("event %u", ev.type);
        for (;;) {
            struct v4l2_plane pl = {0}; struct v4l2_buffer b = { .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_DQBUF, &b);
            if (r == EAGAIN || r == EPIPE) break;
            if (r) { LOG("DQBUF(OUTPUT): errno %d", r); goto done; }
            raws[b.index].queued = 0; progress = now();
            if (b.flags & V4L2_BUF_FLAG_ERROR) LOG("OUTPUT %u back with ERROR", b.index);
        }
        while (next < base + nframes && (!lockstep || next - base == coded - (round - 1) * nframes)) {
            int slot = -1; for (unsigned i = 0; i < nraw; i++) if (!raws[i].queued) { slot = i; break; }
            if (slot < 0) break;
            const uint8_t *src = data + (size_t)(next - base) * frame;
            uint8_t *dst = raws[slot].map;
            for (unsigned y = 0; y < h; y++) memcpy(dst + (size_t)y * stride, src + (size_t)y * w, w);
            for (unsigned y = 0; y < h / 2; y++) memcpy(dst + (size_t)(scanlines + y) * stride, src + (size_t)w * h + (size_t)y * w, w);
            struct v4l2_plane pl = { .bytesused = rawlen, .length = raws[slot].len, .m.fd = raws[slot].fd };
            struct v4l2_buffer b = { .index = slot, .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            long us = (long)next * 1000000L / fps; b.timestamp.tv_sec = us / 1000000; b.timestamp.tv_usec = us % 1000000;
            r = x(VIDIOC_QBUF, &b);
            if (r) { LOG("QBUF(OUTPUT) frame %d: errno %d", next, r); goto done; }
            if (!next) first = now();
            raws[slot].queued = 1; next++; progress = now();
        }
        if (next >= base + nframes && !stop_sent) {
            struct v4l2_encoder_cmd ec = { .cmd = V4L2_ENC_CMD_STOP };
            r = x(VIDIOC_ENCODER_CMD, &ec); LOG("all %d frames in; ENC_CMD_STOP: %d", next, r); stop_sent = 1;
        }
        for (;;) {
            struct v4l2_plane pl = {0}; struct v4l2_buffer b = { .type = CAP, .memory = cq.memory, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_DQBUF, &b);
            if (r == EAGAIN) break;
            if (r == EPIPE) { LOG("DQBUF(CAPTURE): EPIPE (after LAST)"); last = 1; break; }
            if (r) { LOG("DQBUF(CAPTURE): errno %d", r); goto done; }
            progress = now();
            bits[b.index].queued = 0;
            if (b.flags & V4L2_BUF_FLAG_ERROR) errors++;
            else if (pl.bytesused) {
                fwrite(bits[b.index].map + pl.data_offset, 1, pl.bytesused - pl.data_offset, out);
                bytes += pl.bytesused - pl.data_offset; coded++;
                if (b.flags & V4L2_BUF_FLAG_KEYFRAME) keys++;
            }
            long us = b.timestamp.tv_sec * 1000000L + b.timestamp.tv_usec;
            if (coded <= 3 || (b.flags & (V4L2_BUF_FLAG_LAST | V4L2_BUF_FLAG_ERROR)))
                LOG("coded: buffer %u bytesused %u offset %u flags %#x frame %ld", b.index, pl.bytesused, pl.data_offset, b.flags,
                    us * fps / 1000000);
            if (b.flags & V4L2_BUF_FLAG_LAST) { last = 1; break; }
            struct v4l2_plane qp = { .length = bits[b.index].len }; if (!mmapcap) qp.m.fd = bits[b.index].fd;
            struct v4l2_buffer q = { .index = b.index, .type = CAP, .memory = cq.memory, .m.planes = &qp, .length = 1 };
            r = x(VIDIOC_QBUF, &q); if (r) { LOG("re-QBUF(CAPTURE %u): errno %d", b.index, r); goto done; }
            bits[b.index].queued = 1;
        }
    }
    if (last && round < rounds) {
        int k = OUT; r = x(VIDIOC_STREAMOFF, &k); LOG("round %d: STREAMOFF(OUTPUT): %d", round, r);
        for (unsigned i = 0; i < nraw; i++) raws[i].queued = 0;
        r = x(VIDIOC_STREAMON, &k); LOG("round %d: STREAMON(OUTPUT): %d", round, r);
        struct v4l2_encoder_cmd ec = { .cmd = V4L2_ENC_CMD_START };
        r = x(VIDIOC_ENCODER_CMD, &ec); LOG("round %d: ENC_CMD_START: %d", round, r);
        for (unsigned i = 0; i < nbit; i++) {
            if (bits[i].queued) continue;
            struct v4l2_plane pl = { .length = bits[i].len }; if (!mmapcap) pl.m.fd = bits[i].fd;
            struct v4l2_buffer b = { .index = i, .type = CAP, .memory = cq.memory, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_QBUF, &b); if (r) { LOG("round %d: QBUF(CAPTURE %u): errno %d", round, i, r); goto done; }
            bits[i].queued = 1;
        }
        LOG("round %d done at %d coded; round %d", round, coded, round + 1);
        round++; base = next; last = 0; stop_sent = 0; progress = now();
        goto again;
    }
done:;
    const double secs = now() - first;
    LOG("result: %d frames in, %d coded (%d key), %d errors, %zu bytes (%.0f kbit/s at %d fps), %.1f ms (%.1f fps)%s", next,
        coded, keys, errors, bytes, coded ? bytes * 8.0 * fps / coded / 1000 : 0, fps, secs * 1000, coded / secs,
        last ? ", drained" : (now() - progress >= 4.0 ? ", STALLED 4 s" : ""));
    fclose(out);
    int types[] = { CAP, OUT };
    for (int i = 0; i < 2; i++) {
        int k = types[i]; int so = x(VIDIOC_STREAMOFF, &k);
        struct v4l2_requestbuffers z = { .count = 0, .type = types[i], .memory = i ? V4L2_MEMORY_DMABUF : cq.memory };
        if (!i && mmapcap) for (unsigned j = 0; j < nbit; j++) munmap(bits[j].map, bits[j].len);
        int rb = x(VIDIOC_REQBUFS, &z);
        LOG("teardown %s: STREAMOFF %d, REQBUFS(0) %d", i ? "OUTPUT" : "CAPTURE", so, rb);
    }
    close(dev);
    return 0;
}
