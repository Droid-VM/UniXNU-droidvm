// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modern virtio-mmio GPU transport. No renderer or display policy belongs here.
#include "DroidVMVirtioGpu.h"
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" {
volatile uint32_t droidvm_virtio_gpu_state, droidvm_virtio_gpu_last_command, droidvm_virtio_gpu_last_response;
volatile uint64_t droidvm_virtio_gpu_commands, droidvm_virtio_gpu_errors, droidvm_virtio_gpu_timeouts;
}
namespace DroidVMVirtioGpu {
namespace {
struct Desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct Used { uint32_t id, length; };
alignas(16384) uint8_t queues[2][16384];
alignas(16384) uint8_t request[MaxCommand], response[MaxCommand];
uintptr_t registers;
uint64_t requestPA, responsePA, negotiated, nextFence;
uint32_t nextResource;
uint16_t available, consumed;
IOLock *lock;
bool attempted;
uint32_t rd(unsigned offset) {
    uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(registers + offset) : "memory"); return v;
}
void wr(unsigned offset, uint32_t v) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(registers + offset) : "memory");
}
void wr64(unsigned offset, uint64_t v) { wr(offset, uint32_t(v)); wr(offset + 4, uint32_t(v >> 32)); }
void barrier() { __asm__ volatile("dmb osh" ::: "memory"); }
uint64_t nowUs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
bool physical(void *p, size_t size, uint64_t &pa) {
    pa = ml_static_vtop(uintptr_t(p));
    for (size_t i = 0; i < size; i += 4096) if (ml_static_vtop(uintptr_t(p) + i) != pa + i) return false;
    return pa != 0;
}
bool fail(const char *why) { droidvm_virtio_gpu_state = 3; IOLog("virtio-gpu: %s\n", why); return false; }
}
bool start() {
    if (attempted) return ready();
    attempted = true;
    DTEntry armio, node; const void *value; unsigned size; uint64_t ranges[3], reg[2];
    if (SecureDTLookupEntry(nullptr, "/arm-io/virtio-gpu", &node) != kSuccess) return false;
    droidvm_virtio_gpu_state = 1;
    if (SecureDTLookupEntry(nullptr, "/arm-io", &armio) != kSuccess ||
        SecureDTGetProperty(armio, "ranges", &value, &size) != kSuccess || size != sizeof ranges) return fail("no arm-io ranges");
    memcpy(ranges, value, sizeof ranges);
    if (SecureDTGetProperty(node, "reg", &value, &size) != kSuccess || size != sizeof reg) return fail("no reg");
    memcpy(reg, value, sizeof reg);
    if (reg[0] < ranges[0] || reg[1] < 0x100 || reg[1] > ranges[2] || reg[0] - ranges[0] > ranges[2] - reg[1]) return fail("invalid reg");
    registers = ml_io_map(reg[0] - ranges[0] + ranges[1], 0x1000);
    if (!registers || rd(0) != 0x74726976 || rd(4) != 2 || rd(8) != 16) return fail("not a modern virtio GPU");
    if (!physical(request, sizeof request, requestPA) || !physical(response, sizeof response, responsePA)) return fail("noncontiguous commands");
    lock = IOLockAlloc(); if (!lock) return fail("no lock");
    // Reset the firmware's queues before replacing any descriptor addresses.
    wr(0x70, 0);
    for (unsigned i = 0; rd(0x70); ++i) { if (i == 1000) return fail("reset timeout"); IODelay(1000); }
    wr(0x70, 3);
    wr(0x14, 1); uint32_t high = rd(0x10);
    if (!(high & 1)) return fail("no VERSION_1");
    wr(0x14, 0); uint32_t low = rd(0x10) & ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 6));
    // ACCESS_PLATFORM is offered by pseudo-unprotected crosvm; DMA addresses remain GPAs.
    high &= 3;
    negotiated = (uint64_t(high) << 32) | low;
    wr(0x24, 0); wr(0x20, low); wr(0x24, 1); wr(0x20, high); wr(0x70, 11);
    if (!(rd(0x70) & 8)) return fail("features refused");
    for (unsigned q = 0; q < 2; ++q) {
        uint64_t pa; if (!physical(queues[q], sizeof queues[q], pa)) return fail("noncontiguous queue");
        wr(0x30, q);
        if (rd(0x44) || rd(0x34) < 4) return fail("queue unavailable");
        memset(queues[q], 0, sizeof queues[q]);
        *reinterpret_cast<uint16_t *>(queues[q] + 128) = 1; // no interrupts; worker polls
        wr(0x38, 4); wr64(0x80, pa); wr64(0x90, pa + 128); wr64(0xa0, pa + 256); wr(0x44, 1);
    }
    barrier(); wr(0x70, 15);
    if ((rd(0x70) & 0x4f) != 15) return fail("device not ready");
    droidvm_virtio_gpu_state = 2;
    IOLog("virtio-gpu: control/cursor queues ready, features %llx\n", negotiated);
    return true;
}
bool ready() { return droidvm_virtio_gpu_state == 2; }
uint64_t features() { return negotiated; }
IOReturn command(const void *data, uint32_t size, const void *extra, uint32_t extraSize,
                 void *reply, uint32_t capacity, uint32_t *actual) {
    if (actual) *actual = 0;
    if (!ready()) return kIOReturnNotReady;
    if (!data || size < sizeof(Header) || size > MaxCommand || extraSize > MaxCommand - size ||
        (extraSize && !extra) || capacity < sizeof(Header) || capacity > MaxCommand) return kIOReturnBadArgument;
    IOLockLock(lock);
    if (!ready()) { IOLockUnlock(lock); return kIOReturnNotResponding; }
    memcpy(request, data, size); if (extraSize) memcpy(request + size, extra, extraSize);
    Header *hdr = reinterpret_cast<Header *>(request);
    hdr->flags |= 1; hdr->fence = ++nextFence;
    memset(response, 0, capacity);
    droidvm_virtio_gpu_last_command = hdr->type;
    auto desc = reinterpret_cast<Desc *>(queues[0]);
    desc[0] = {requestPA, size + extraSize, 1, 1}; desc[1] = {responsePA, capacity, 2, 0};
    auto avail = reinterpret_cast<volatile uint16_t *>(queues[0] + 128);
    auto used = reinterpret_cast<volatile uint16_t *>(queues[0] + 256);
    avail[2 + available % 4] = 0; barrier(); avail[1] = ++available; barrier(); wr(0x50, 0);
    uint64_t begin = nowUs();
    while (used[1] == consumed && nowUs() - begin < 5000000) { IOSleep(1); barrier(); }
    barrier();
    auto element = reinterpret_cast<volatile Used *>(queues[0] + 260) + consumed % 4;
    auto answer = reinterpret_cast<Header *>(response);
    const bool errorResponse = answer->type >= 0x1200 && answer->type < 0x1300;
    IOReturn result = kIOReturnSuccess;
    if (used[1] == consumed) {
        droidvm_virtio_gpu_timeouts = droidvm_virtio_gpu_timeouts + 1; fail("control queue timed out; DMA buffers retained"); result = kIOReturnTimeout;
    } else if (used[1] != uint16_t(consumed + 1) || element->id != 0 || element->length < sizeof(Header) ||
               element->length > capacity || (!errorResponse && (!(answer->flags & 1) || answer->fence != hdr->fence))) {
        fail("malformed fenced response; DMA buffers retained"); result = kIOReturnIOError;
    } else {
        ++consumed; droidvm_virtio_gpu_last_response = answer->type;
        if (reply) memcpy(reply, response, element->length);
        if (actual) *actual = element->length;
        if (answer->type < 0x1100 || answer->type >= 0x1200) result = kIOReturnIOError;
    }
    uint32_t interrupts = rd(0x60) & 3; if (interrupts) wr(0x64, interrupts);
    droidvm_virtio_gpu_commands = droidvm_virtio_gpu_commands + 1;
    if (result) { droidvm_virtio_gpu_errors = droidvm_virtio_gpu_errors + 1; IOLog("virtio-gpu: command %x response %x status %x\n", hdr->type, answer->type, result); }
    IOLockUnlock(lock); return result;
}
uint32_t resourceId() { IOLockLock(lock); uint32_t id = ++nextResource; IOLockUnlock(lock); return id; }
IOReturn unref(uint32_t id) {
    struct { Header h; uint32_t id, padding; } cmd = {{0x102, 0, 0, 0, 0, {}}, id, 0}; Header reply;
    return command(&cmd, sizeof cmd, nullptr, 0, &reply, sizeof reply);
}
}
