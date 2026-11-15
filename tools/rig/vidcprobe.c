// vidcprobe: crosvm's msm_vidc direct decoder (devices/src/virtio/media/android_codec_backend/vidc.rs) replayed on the phone
// as root, no VM: the same ioctls in the same order, CAPTURE buffers as udmabufs over a memfd cut in scattered 16 KiB runs
// (what guest buffers are) or dma-heap buffers. Every event, error and frame is printed.
//
// Build: aarch64-linux-gnu-gcc -static -O2 -o vidcprobe vidcprobe.c; run as root on the phone with Annex-B streams that
// carry access unit delimiters (ffmpeg: -x264-params aud=1, -x265-params aud=1). udmabufs need DroidVM's udmabuf module
// with msm_prime=1 on a Qualcomm phone (gunyah_host_mod/udmabuf): without it msm_vidc calls them secure, refuses the first
// CAPTURE buffer (EINVAL; dmesg "secure buffer mapping to non-secure region"), and keeps that buffer for good.
//
//   vidcprobe FILE h264|hevc W H [-d] [-c udmabuf|heap] [-n BUFFERS] [-s STREAMON_AT] [-x]
//     -d  decode order (DISPLAY_DELAY_ENABLE 1, DISPLAY_DELAY 0) as the DroidVM VideoToolbox host asks
//     -x  a second SOURCE_CHANGE is followed (crosvm fails the session on one)
//     -L  lockstep (with -d): the next access unit only once the frames are caught up, as a player feeding in real time does
//     -P MS  poll timeout (default 100); -A N  with -L, access units given ahead of the frames out (default 2; 0: one at a time)
//     -W MS  the drain (DEC_CMD_STOP) only MS after the last access unit: whether the last frames come out without one
//     -N  each access unit without its delimiter (AUD), as an MP4/MOV sample is
//     -k  each frame read back through a mapping, without DMA_BUF_IOCTL_SYNC (as a guest reads it), and hashed: the same
//         stream gives the same hashes into udmabufs as into dma-heap buffers when the device's writes reach the CPU
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
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

#define OUTPUT_BUFFERS 8
#define CAPTURE_SLOTS 32
#define STEP_US 4166
#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

static int dev, heap = -1, udmabuf = -1, memfd = -1;
static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define LOG(...) do { printf("%8.3f ", now() - t0); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int x(unsigned long req, void *arg) { return ioctl(dev, req, arg) < 0 ? errno : 0; }

// Access units: split at access unit delimiters (H.264 NAL 9, HEVC NAL 35).
static uint8_t *data; static size_t size;
static size_t aus[4096]; static int nau;
static void split(int hevc) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
        int type = hevc ? (data[i + 3] >> 1) & 63 : data[i + 3] & 31;
        size_t start = (i && !data[i - 1]) ? i - 1 : i;
        if (type == (hevc ? 35 : 9) && nau < 4095) aus[nau++] = start;
    }
    aus[nau] = size;
}

struct buf { int fd; void *map; size_t len; int queued; void *view; };
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL; return h; }
static struct buf bits[OUTPUT_BUFFERS], caps[CAPTURE_SLOTS];

static int heap_alloc(size_t len) {
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { LOG("dma-heap alloc %zu: errno %d", len, errno); exit(1); }
    return a.fd;
}

static off_t memfd_used;
static int udmabuf_alloc(size_t len) {
    // Runs of 16 KiB, every other one skipped and the order reversed in pairs: pages scattered like a guest's.
    size_t runs = (len + 16383) / 16384;
    struct udmabuf_create_list *list = calloc(1, sizeof *list + runs * sizeof(struct udmabuf_create_item));
    off_t base = memfd_used;
    memfd_used += runs * 2 * 16384;
    if (ftruncate(memfd, memfd_used) < 0) { LOG("ftruncate: errno %d", errno); exit(1); }
    list->flags = UDMABUF_FLAGS_CLOEXEC;
    list->count = runs;
    for (size_t i = 0; i < runs; i++) {
        size_t j = i ^ 1; if (j >= runs) j = i;
        list->list[i].memfd = memfd;
        list->list[i].offset = base + j * 2 * 16384;
        list->list[i].size = 16384;
    }
    int fd = ioctl(udmabuf, UDMABUF_CREATE_LIST, list);
    if (fd < 0) { LOG("UDMABUF_CREATE_LIST %zu runs: errno %d", runs, errno); exit(1); }
    free(list);
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "vidcprobe FILE h264|hevc W H [-d] [-c udmabuf|heap] [-n N] [-s K] [-x]\n"); return 2; }
    t0 = now();
    const int hevc = !strcmp(argv[2], "hevc");
    const unsigned w = atoi(argv[3]), h = atoi(argv[4]);
    int delay = 0, use_heap = 0, nbuf = 0, streamon_at = 0, follow = 0, check = 0, lockstep = 0, poll_ms = 100, ahead = 2, drain_wait_ms = 0, strip_aud = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "-d")) delay = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) use_heap = !strcmp(argv[++i], "heap");
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nbuf = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) streamon_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-x")) follow = 1;
        else if (!strcmp(argv[i], "-k")) check = 1;
        else if (!strcmp(argv[i], "-L")) lockstep = 1;
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) poll_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) ahead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) drain_wait_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-N")) strip_aud = 1;
    }
    int f = open(argv[1], O_RDONLY); struct stat st; fstat(f, &st); size = st.st_size;
    data = malloc(size); if (read(f, data, size) != (ssize_t)size) return 1; close(f);
    split(hevc);
    LOG("%s: %zu bytes, %d access units, %s %ux%u, decode order %d, capture %s", argv[1], size, nau, argv[2], w, h, delay,
        use_heap ? "dma-heap" : "udmabuf over memfd");

    dev = open("/dev/video32", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (dev < 0) { LOG("open /dev/video32: errno %d", errno); return 1; }
    heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    memfd = memfd_create("guest", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd >= 0 && fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) LOG("F_SEAL_SHRINK: errno %d", errno);
    if (heap < 0 || udmabuf < 0 || memfd < 0) { LOG("heap %d udmabuf %d memfd %d: errno %d", heap, udmabuf, memfd, errno); return 1; }
    struct v4l2_capability cap = {0};
    x(VIDIOC_QUERYCAP, &cap);
    LOG("driver %s card %s bus %s version %#x caps %#x", cap.driver, cap.card, cap.bus_info, cap.version, cap.device_caps);

    struct v4l2_format fmt = { .type = OUT };
    fmt.fmt.pix_mp.pixelformat = hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.width = w; fmt.fmt.pix_mp.height = h; fmt.fmt.pix_mp.num_planes = 1;
    int r = x(VIDIOC_S_FMT, &fmt); if (r) { LOG("S_FMT(OUTPUT): errno %d", r); return 1; }
    x(VIDIOC_G_FMT, &fmt);
    const size_t sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    LOG("OUTPUT %ux%u sizeimage %zu", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, sizeimage);
    if (delay) {
        struct v4l2_control c = { V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE, 1 };
        r = x(VIDIOC_S_CTRL, &c); LOG("S_CTRL DISPLAY_DELAY_ENABLE 1: %d", r);
        c.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY; c.value = 0;
        r = x(VIDIOC_S_CTRL, &c); LOG("S_CTRL DISPLAY_DELAY 0: %d", r);
    }
    unsigned events[] = { V4L2_EVENT_SOURCE_CHANGE, V4L2_EVENT_EOS };
    for (int i = 0; i < 2; i++) { struct v4l2_event_subscription s = { .type = events[i] }; r = x(VIDIOC_SUBSCRIBE_EVENT, &s); if (r) LOG("SUBSCRIBE %u: %d", events[i], r); }
    struct v4l2_requestbuffers rq = { .count = OUTPUT_BUFFERS, .type = OUT, .memory = V4L2_MEMORY_DMABUF };
    r = x(VIDIOC_REQBUFS, &rq); if (r) { LOG("REQBUFS(OUTPUT): errno %d", r); return 1; }
    const size_t bitlen = ((sizeimage > (1 << 20) ? sizeimage : (1 << 20)) + 4095) & ~4095UL;
    for (unsigned i = 0; i < rq.count && i < OUTPUT_BUFFERS; i++) {
        bits[i].fd = heap_alloc(bitlen); bits[i].len = bitlen;
        bits[i].map = mmap(NULL, bitlen, PROT_READ | PROT_WRITE, MAP_SHARED, bits[i].fd, 0);
    }
    const unsigned nbits = rq.count < OUTPUT_BUFFERS ? rq.count : OUTPUT_BUFFERS;
    int kind = OUT; r = x(VIDIOC_STREAMON, &kind); if (r) { LOG("STREAMON(OUTPUT): errno %d", r); return 1; }
    LOG("%u bitstream buffers of %zu", nbits, bitlen);

    int next = 0, fed = 0, frames = 0, errors = 0, capture_ready = 0, streaming = 0, changes = 0, drain_sent = 0, last = 0;
    unsigned min_capture = 1, ncap = 0, queued = 0; size_t caplen = 0;
    double progress = now(), started = now();
    while (now() - progress < 4.0 && !last) {
        struct pollfd p = { dev, POLLIN | POLLOUT | POLLPRI, 0 };
        poll(&p, 1, poll_ms);
        struct v4l2_event ev;
        while (!(r = x(VIDIOC_DQEVENT, &ev))) {
            progress = now();
            if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                changes++;
                LOG("event SOURCE_CHANGE #%d changes %#x (sequence %u)", changes, ev.u.src_change.changes, ev.sequence);
                if (capture_ready && !follow) { LOG("crosvm would fail the session here (a mid-stream resolution change)"); goto done; }
                if (capture_ready) {
                    int k = CAP; r = x(VIDIOC_STREAMOFF, &k); LOG("  STREAMOFF(CAPTURE): %d", r);
                    struct v4l2_requestbuffers z = { .count = 0, .type = CAP, .memory = V4L2_MEMORY_DMABUF };
                    r = x(VIDIOC_REQBUFS, &z); LOG("  REQBUFS(CAPTURE, 0): %d", r);
                    for (unsigned i = 0; i < ncap; i++) { close(caps[i].fd); caps[i].queued = 0; }
                    capture_ready = streaming = 0; queued = 0;
                }
                uint32_t offered[16]; int noffered = 0;
                for (unsigned i = 0; i < 16; i++) {
                    struct v4l2_fmtdesc d = { .index = i, .type = CAP };
                    if (x(VIDIOC_ENUM_FMT, &d)) break;
                    offered[noffered++] = d.pixelformat;
                }
                char list[160] = ""; for (int i = 0; i < noffered; i++) { char s[8]; snprintf(s, 8, "%.4s ", (char *)&offered[i]); strcat(list, s); }
                uint32_t linear = 0;
                for (int i = 0; i < noffered; i++) if (offered[i] == V4L2_PIX_FMT_NV12) linear = offered[i];
                if (!linear) for (int i = 0; i < noffered; i++) if (offered[i] == V4L2_PIX_FMT_P010) linear = offered[i];
                LOG("  CAPTURE formats: %s-> %.4s", list, (char *)&linear);
                if (!linear) goto done;
                struct v4l2_format cf = { .type = CAP };
                x(VIDIOC_G_FMT, &cf);
                cf.fmt.pix_mp.pixelformat = linear;
                r = x(VIDIOC_S_FMT, &cf); if (r) LOG("  S_FMT(CAPTURE): %d", r);
                x(VIDIOC_G_FMT, &cf);
                caplen = cf.fmt.pix_mp.plane_fmt[0].sizeimage;
                LOG("  CAPTURE %ux%u planes %u bytesperline %u sizeimage %zu", cf.fmt.pix_mp.width, cf.fmt.pix_mp.height,
                    cf.fmt.pix_mp.num_planes, cf.fmt.pix_mp.plane_fmt[0].bytesperline, caplen);
                struct v4l2_selection sel = { .type = CAP, .target = V4L2_SEL_TGT_CROP };
                r = x(VIDIOC_G_SELECTION, &sel); LOG("  G_SELECTION CROP: %d -> %d,%d %ux%u", r, sel.r.left, sel.r.top, sel.r.width, sel.r.height);
                struct v4l2_control c = { V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, 0 };
                r = x(VIDIOC_G_CTRL, &c); min_capture = r ? 4 : (c.value > 0 ? c.value : 1);
                struct v4l2_requestbuffers q = { .count = CAPTURE_SLOTS, .type = CAP, .memory = V4L2_MEMORY_DMABUF };
                r = x(VIDIOC_REQBUFS, &q); if (r) { LOG("  REQBUFS(CAPTURE): errno %d", r); goto done; }
                ncap = nbuf ? (unsigned)nbuf : min_capture; if (ncap > q.count) ncap = q.count;
                LOG("  min %u buffers, %u slots; allocating %u", min_capture, q.count, ncap);
                for (unsigned i = 0; i < ncap; i++) {
                    caps[i].len = (caplen + 4095) & ~4095UL;
                    caps[i].fd = use_heap ? heap_alloc(caps[i].len) : udmabuf_alloc(caps[i].len);
                    caps[i].queued = 0; caps[i].view = NULL;
                }
                capture_ready = 1;
            } else LOG("event %u", ev.type);
        }
        // Bitstream buffers back.
        for (;;) {
            struct v4l2_plane pl = {0}; struct v4l2_buffer b = { .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_DQBUF, &b);
            if (r == EAGAIN || r == EPIPE) break;
            if (r) { LOG("DQBUF(OUTPUT): errno %d", r); goto done; }
            bits[b.index].fd = bits[b.index].fd; bits[b.index].queued = 0; progress = now();
            if (b.flags & V4L2_BUF_FLAG_ERROR) LOG("OUTPUT %u back with ERROR", b.index);
        }
        // Feed.
        while (next < nau && (!lockstep || !streaming || next <= frames + errors + ahead)) {
            int slot = -1; for (unsigned i = 0; i < nbits; i++) if (!bits[i].queued) { slot = i; break; }
            if (slot < 0) break;
            size_t len = aus[next + 1] - aus[next];
            const uint8_t *au = data + aus[next];
            if (strip_aud) {   // the delimiter that starts the unit left out, as a container's samples have none
                size_t i = 0;
                while (i + 3 < len && !(au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)) i++;
                size_t j = i + 3;
                while (j + 3 < len && !(au[j] == 0 && au[j + 1] == 0 && (au[j + 2] == 1 || (au[j + 2] == 0 && au[j + 3] == 1)))) j++;
                if (j + 3 < len) { au += j; len -= j; }
            }
            memcpy(bits[slot].map, au, len);
            struct v4l2_plane pl = { .bytesused = len, .length = bits[slot].len, .m.fd = bits[slot].fd };
            struct v4l2_buffer b = { .index = slot, .type = OUT, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            long us = (long)next * STEP_US; b.timestamp.tv_sec = us / 1000000; b.timestamp.tv_usec = us % 1000000;
            r = x(VIDIOC_QBUF, &b);
            if (r) { LOG("QBUF(OUTPUT) au %d %zu bytes: errno %d", next, len, r); goto done; }
            bits[slot].queued = 1; next++; fed++; progress = now();
        }
        static double all_in = 0;
        if (next >= nau && !all_in) { all_in = now(); if (drain_wait_ms) LOG("all %d access units in, %d frames out; the drain waits %d ms", fed, frames, drain_wait_ms); }
        if (next >= nau && !drain_sent && (now() - all_in) * 1000 >= drain_wait_ms) {
            if (drain_wait_ms) LOG("%d frames out before the drain", frames);
            struct v4l2_decoder_cmd dc = { .cmd = V4L2_DEC_CMD_STOP };
            r = x(VIDIOC_DECODER_CMD, &dc); LOG("all %d access units in; DEC_CMD_STOP: %d", fed, r); drain_sent = 1;
        }
        // CAPTURE buffers in.
        if (capture_ready) {
            for (unsigned i = 0; i < ncap; i++) {
                if (caps[i].queued) continue;
                struct v4l2_plane pl = { .length = caps[i].len, .m.fd = caps[i].fd };
                struct v4l2_buffer b = { .index = i, .type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
                r = x(VIDIOC_QBUF, &b);
                if (r) { LOG("QBUF(CAPTURE %u, %zu bytes): errno %d", i, caps[i].len, r); goto done; }
                caps[i].queued = 1; queued++;
            }
            unsigned at = streamon_at ? (unsigned)streamon_at : (min_capture < 4 ? min_capture : 4);
            if (!streaming && queued >= at) {
                int k = CAP; r = x(VIDIOC_STREAMON, &k); LOG("STREAMON(CAPTURE) with %u queued: %d", queued, r);
                if (r) goto done;
                streaming = 1;
            }
        }
        // Frames out.
        while (streaming) {
            struct v4l2_plane pl = {0}; struct v4l2_buffer b = { .type = CAP, .memory = V4L2_MEMORY_DMABUF, .m.planes = &pl, .length = 1 };
            r = x(VIDIOC_DQBUF, &b);
            if (r == EAGAIN) break;
            if (r == EPIPE) { LOG("DQBUF(CAPTURE): EPIPE (after LAST)"); last = 1; break; }
            if (r) { LOG("DQBUF(CAPTURE): errno %d", r); goto done; }
            progress = now();
            caps[b.index].queued = 0; queued--;
            long us = b.timestamp.tv_sec * 1000000L + b.timestamp.tv_usec;
            if (b.flags & V4L2_BUF_FLAG_ERROR) errors++; else if (pl.bytesused) frames++;
            if (frames + errors <= 3 || (b.flags & (V4L2_BUF_FLAG_LAST | V4L2_BUF_FLAG_ERROR)))
                LOG("frame: buffer %u bytesused %u flags %#x au %ld%s", b.index, pl.bytesused, b.flags, us / STEP_US,
                    b.flags & V4L2_BUF_FLAG_ERROR ? " ERROR" : "");
            if (check && pl.bytesused && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
                struct buf *c = &caps[b.index];
                if (!c->view) c->view = mmap(NULL, c->len, PROT_READ, MAP_SHARED, c->fd, 0);
                if (c->view == MAP_FAILED) { LOG("mmap CAPTURE %u: errno %d", b.index, errno); c->view = NULL; }
                else {
                    static uint64_t all = 0; uint64_t h = fnv(c->view, pl.bytesused); all ^= h * (us / STEP_US + 1);
                    if (frames <= 5) LOG("  au %ld hash %016llx", us / STEP_US, (unsigned long long)h);
                    if (b.flags & V4L2_BUF_FLAG_LAST || next >= nau) LOG("  running hash %016llx after %d frames", (unsigned long long)all, frames);
                }
            }
            if (b.flags & V4L2_BUF_FLAG_LAST) last = 1;
        }
    }
done:
    LOG("%.1f ms for %d frames (%.1f fps)", (now() - started) * 1000, frames, frames / (now() - started));
    LOG("result: %d access units in, %d frames out, %d error frames, %d source changes%s", fed, frames, errors, changes,
        last ? ", drained" : (now() - progress >= 4.0 ? ", STALLED 4 s" : ""));
    // crosvm's teardown (Stream::drop): both queues off and emptied before the node closes.
    int types[] = { CAP, OUT };
    for (int i = 0; i < 2; i++) {
        int k = types[i]; int so = x(VIDIOC_STREAMOFF, &k);
        struct v4l2_requestbuffers z = { .count = 0, .type = types[i], .memory = V4L2_MEMORY_DMABUF };
        int rb = x(VIDIOC_REQBUFS, &z);
        LOG("teardown %s: STREAMOFF %d, REQBUFS(0) %d", i ? "OUTPUT" : "CAPTURE", so, rb);
    }
    close(dev);
    return 0;
}
