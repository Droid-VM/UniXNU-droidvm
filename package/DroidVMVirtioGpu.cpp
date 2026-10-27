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
volatile uint32_t droidvm_virtio_gpu_in_flight, droidvm_virtio_gpu_in_flight_peak;
// Commands that waited 5 s or more for a free slot, and the longest wait (us): a full queue is backpressure, not a failure.
volatile uint64_t droidvm_virtio_gpu_slot_waits_long, droidvm_virtio_gpu_slot_wait_max_us;
}
namespace DroidVMVirtioGpu {
namespace {
struct Desc { uint64_t address; uint32_t length; uint16_t flags, next; };
struct Used { uint32_t id, length; };
// Each command in flight owns a slot: its request and response buffers and descriptors 2*slot (the request) and 2*slot+1 (the
// response). A fenced command (a 3D submission) holds its slot until its fence signals, so the slots bound how much GPU work is in
// flight. The last LargeSlots slots have the large buffers, for commands longer than a slot's: a resource's backing pages, and the
// submissions of heavy frames (a submit's BO table outgrows 16 KiB), which a single large slot ran one GPU job at a time.
// 256 slots is the most the queue takes (crosvm's control queue is at most 512 descriptors, and a ticket names its slot in 8
// bits); 244 slots of 16 + 4 KiB and 12 of 128 + 128 KiB are 7.8 MiB of buffers, inside the 8 MiB budget.
enum : uint32_t { Slots = 256, LargeSlots = 12, Large = Slots - LargeSlots, QueueSize = 2 * Slots, SlotRequest = 16384,
                  SlotResponse = 4096, AvailOffset = QueueSize * 16, UsedOffset = 10240 };
static_assert(sizeof(Desc) == 16 && AvailOffset + 6 + 2 * QueueSize <= UsedOffset && UsedOffset + 6 + 8 * QueueSize <= 16384,
              "control queue layout");
enum : uint8_t { Free, Posted, Done, Abandoned };   // Abandoned: its waiter gave up; the device still owns its buffers
struct Slot {
    uint8_t *request, *response;
    uint64_t requestPA, responsePA, fence;
    uint32_t requestCapacity, responseCapacity, capacity, length, type;
    uint16_t generation;
    uint8_t state;
    bool fenced;
    IOReturn result;
};
alignas(16384) uint8_t queues[2][16384];
alignas(16384) uint8_t requests[Large][SlotRequest];
alignas(4096) uint8_t responses[Large][SlotResponse];
alignas(16384) uint8_t largeRequest[LargeSlots][MaxCommand], largeResponse[LargeSlots][MaxCommand];
Slot slots[Slots];
uintptr_t registers;
uint64_t negotiated, nextFence, lastReturnUs;
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
// Polling, as every virtio driver of this guest does (the queues are NO_INTERRUPT): the used ring is guest memory, so reading it
// costs no exit. A wait spins for its first 200 us and then sleeps, so the vCPUs of waiting threads go idle (WFI, on which the
// hypervisor blocks them): four vCPUs that spun through commands the host was slow to return came before each host watchdog
// bite on 2026-10-25, when this spun for 2 ms.
void pause(uint64_t begin) { if (nowUs() - begin < 200) IODelay(5); else IOSleep(1); }
// Takes every command the device has returned off the used ring, in whatever order it returned them (a fenced command comes back
// when its fence signals). Called with the lock held, by whichever thread is waiting.
void reap() {
    auto used = reinterpret_cast<volatile uint16_t *>(queues[0] + UsedOffset);
    barrier();
    bool any = false;
    while (used[1] != consumed) {
        auto element = reinterpret_cast<volatile Used *>(queues[0] + UsedOffset + 4) + consumed % QueueSize;
        const uint32_t head = element->id, length = element->length;
        ++consumed; any = true; lastReturnUs = nowUs();
        if (head >= QueueSize || (head & 1) || (slots[head / 2].state != Posted && slots[head / 2].state != Abandoned)) {
            fail("the device returned a descriptor no command is waiting on; DMA buffers retained"); return;
        }
        Slot &slot = slots[head / 2];
        auto answer = reinterpret_cast<Header *>(slot.response);
        const bool errorResponse = answer->type >= 0x1200 && answer->type < 0x1300;
        IOReturn result = kIOReturnSuccess;
        if (length < sizeof(Header) || length > slot.capacity ||
            (!errorResponse && slot.fenced && (!(answer->flags & FlagFence) || answer->fence != slot.fence))) result = kIOReturnIOError;
        else if (answer->type < 0x1100 || answer->type >= 0x1200) result = kIOReturnIOError;
        slot.result = result; slot.length = length;
        droidvm_virtio_gpu_last_response = answer->type;
        droidvm_virtio_gpu_commands = droidvm_virtio_gpu_commands + 1;
        if (result) {
            droidvm_virtio_gpu_errors = droidvm_virtio_gpu_errors + 1;
            IOLog("virtio-gpu: command %x response %x length %u status %x\n", slot.type, answer->type, length, result);
        }
        slot.state = slot.state == Abandoned ? Free : Done;
        droidvm_virtio_gpu_in_flight = droidvm_virtio_gpu_in_flight - 1;
    }
    if (any) { uint32_t interrupts = rd(0x60) & 3; if (interrupts) wr(0x64, interrupts); }
}
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
    for (uint32_t s = 0; s < Slots; ++s) {
        Slot &slot = slots[s];
        slot.request = s >= Large ? largeRequest[s - Large] : requests[s];
        slot.response = s >= Large ? largeResponse[s - Large] : responses[s];
        slot.requestCapacity = s >= Large ? uint32_t(MaxCommand) : uint32_t(SlotRequest);
        slot.responseCapacity = s >= Large ? uint32_t(MaxCommand) : uint32_t(SlotResponse);
        if (!physical(slot.request, slot.requestCapacity, slot.requestPA) || !physical(slot.response, slot.responseCapacity, slot.responsePA))
            return fail("noncontiguous command buffers");
    }
    lock = IOLockAlloc(); if (!lock) return fail("no lock");
    // Reset the firmware's queues before replacing any descriptor addresses.
    wr(0x70, 0);
    for (unsigned i = 0; rd(0x70); ++i) { if (i == 1000) return fail("reset timeout"); IODelay(1000); }
    wr(0x70, 3);
    wr(0x14, 1); uint32_t high = rd(0x10);
    if (!(high & 1)) return fail("no VERSION_1");
    // VIRGL, EDID (the scanout's mode: its refresh rate), RESOURCE_BLOB, CONTEXT_INIT, CREATE_GUEST_HANDLE.
    wr(0x14, 0); uint32_t low = rd(0x10) & ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6));
    // ACCESS_PLATFORM is offered by pseudo-unprotected crosvm; DMA addresses remain GPAs.
    high &= 3;
    negotiated = (uint64_t(high) << 32) | low;
    wr(0x24, 0); wr(0x20, low); wr(0x24, 1); wr(0x20, high); wr(0x70, 11);
    if (!(rd(0x70) & 8)) return fail("features refused");
    // The control queue holds every slot's two descriptors; the cursor queue stays as small as the device allows.
    const uint32_t sizes[2] = {QueueSize, 4}, availAt[2] = {AvailOffset, 128}, usedAt[2] = {UsedOffset, 256};
    for (unsigned q = 0; q < 2; ++q) {
        uint64_t pa; if (!physical(queues[q], sizeof queues[q], pa)) return fail("noncontiguous queue");
        wr(0x30, q);
        if (rd(0x44) || rd(0x34) < sizes[q]) return fail("queue unavailable or too small");
        memset(queues[q], 0, sizeof queues[q]);
        *reinterpret_cast<uint16_t *>(queues[q] + availAt[q]) = 1; // no interrupts; waiters poll
        wr(0x38, sizes[q]); wr64(0x80, pa); wr64(0x90, pa + availAt[q]); wr64(0xa0, pa + usedAt[q]); wr(0x44, 1);
    }
    barrier(); wr(0x70, 15);
    if ((rd(0x70) & 0x4f) != 15) return fail("device not ready");
    droidvm_virtio_gpu_state = 2;
    IOLog("virtio-gpu: control queue of %u commands and cursor queue ready, features %llx\n", Slots, negotiated);
    return true;
}
bool ready() { return droidvm_virtio_gpu_state == 2; }
uint64_t features() { return negotiated; }
IOReturn post(const void *data, uint32_t size, const void *extra, uint32_t extraSize, uint32_t capacity, bool globalFence,
              uint32_t *ticket) {
    if (ticket) *ticket = 0;
    if (!ready()) return kIOReturnNotReady;
    if (!ticket || !data || size < sizeof(Header) || size > MaxCommand || extraSize > MaxCommand - size || (extraSize && !extra) ||
        capacity < sizeof(Header) || capacity > MaxCommand) return kIOReturnBadArgument;
    const bool large = size + extraSize > SlotRequest || capacity > SlotResponse;
    const uint64_t begin = nowUs();
    IOLockLock(lock);
    uint32_t s = Slots;
    bool longWait = false;
    while (s == Slots) {
        if (!ready()) { IOLockUnlock(lock); return kIOReturnNotResponding; }
        reap();
        // A short command takes a large slot too when the short ones are all in flight.
        for (uint32_t i = large ? Large : 0; i < Slots && s == Slots; ++i) if (slots[i].state == Free) s = i;
        if (s != Slots) break;
        // Every slot in flight is the GPU's backlog: wait while the device keeps returning commands. Refusing after 5 s turned a
        // heavy frame (GFXBench's Start all) into a submission error, which Turnip takes for a lost device: every later command
        // buffer and buffer of the app was refused. Only a device that returned nothing for 60 s is given up on.
        const uint64_t now = nowUs();
        if (!longWait && now - begin >= 5000000) { longWait = true; droidvm_virtio_gpu_slot_waits_long = droidvm_virtio_gpu_slot_waits_long + 1; }
        if (now - (lastReturnUs > begin ? lastReturnUs : begin) >= 60000000) {
            droidvm_virtio_gpu_timeouts = droidvm_virtio_gpu_timeouts + 1;
            IOLockUnlock(lock); IOLog("virtio-gpu: no command returned in 60 s and no free slot\n"); return kIOReturnNoResources;
        }
        IOLockUnlock(lock); pause(begin); IOLockLock(lock);
    }
    const uint64_t waited = nowUs() - begin;
    if (waited > droidvm_virtio_gpu_slot_wait_max_us) droidvm_virtio_gpu_slot_wait_max_us = waited;
    Slot &slot = slots[s];
    memcpy(slot.request, data, size); if (extraSize) memcpy(slot.request + size, extra, extraSize);
    Header *hdr = reinterpret_cast<Header *>(slot.request);
    if (globalFence) { hdr->flags = (hdr->flags | FlagFence) & ~uint32_t(FlagRingIndex); hdr->fence = ++nextFence; }
    slot.fenced = hdr->flags & FlagFence; slot.fence = hdr->fence; slot.type = hdr->type;
    slot.capacity = capacity; slot.length = 0; slot.result = kIOReturnNotReady;
    memset(slot.response, 0, capacity);
    auto desc = reinterpret_cast<Desc *>(queues[0]);
    desc[2 * s] = {slot.requestPA, size + extraSize, 1, uint16_t(2 * s + 1)};
    desc[2 * s + 1] = {slot.responsePA, capacity, 2, 0};
    slot.state = Posted;
    if (++slot.generation == 0) slot.generation = 1;
    *ticket = uint32_t(slot.generation) << 8 | s;
    const uint32_t inFlight = droidvm_virtio_gpu_in_flight + 1;
    droidvm_virtio_gpu_in_flight = inFlight;
    if (inFlight > droidvm_virtio_gpu_in_flight_peak) droidvm_virtio_gpu_in_flight_peak = inFlight;
    droidvm_virtio_gpu_last_command = hdr->type;
    auto avail = reinterpret_cast<volatile uint16_t *>(queues[0] + AvailOffset);
    avail[2 + available % QueueSize] = uint16_t(2 * s); barrier(); avail[1] = ++available; barrier(); wr(0x50, 0);
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}
bool poll(uint32_t ticket, IOReturn *result, void *reply, uint32_t capacity, uint32_t *actual) {
    if (actual) *actual = 0;
    const uint32_t s = ticket & 0xff;
    if (s >= Slots || !lock) { if (result) *result = kIOReturnBadArgument; return true; }
    IOLockLock(lock);
    reap();
    Slot &slot = slots[s];
    if (slot.generation != ticket >> 8 || (slot.state != Posted && slot.state != Done)) {
        IOLockUnlock(lock); if (result) *result = kIOReturnBadArgument; return true;
    }
    if (slot.state == Posted) {
        const bool broken = !ready();
        IOLockUnlock(lock);
        if (broken && result) *result = kIOReturnNotResponding;
        return broken;
    }
    const uint32_t length = slot.length < capacity ? slot.length : capacity;
    if (reply && length) memcpy(reply, slot.response, length);
    if (actual) *actual = length;
    if (result) *result = slot.result;
    slot.state = Free;
    IOLockUnlock(lock);
    return true;
}
IOReturn wait(uint32_t ticket, uint64_t timeoutUs, void *reply, uint32_t capacity, uint32_t *actual) {
    const uint64_t begin = nowUs();
    IOReturn result = kIOReturnSuccess;
    while (!poll(ticket, &result, reply, capacity, actual)) {
        if (nowUs() - begin >= timeoutUs) {
            // The device still owns the slot's buffers: it is freed only when the device returns it.
            IOLockLock(lock);
            Slot &slot = slots[ticket & 0xff];
            if (slot.generation == ticket >> 8 && slot.state == Posted) slot.state = Abandoned;
            IOLockUnlock(lock);
            droidvm_virtio_gpu_timeouts = droidvm_virtio_gpu_timeouts + 1;
            return kIOReturnTimeout;
        }
        pause(begin);
    }
    return result;
}
IOReturn command(const void *data, uint32_t size, const void *extra, uint32_t extraSize,
                 void *reply, uint32_t capacity, uint32_t *actual) {
    if (actual) *actual = 0;
    uint32_t ticket = 0;
    IOReturn result = post(data, size, extra, extraSize, capacity, true, &ticket);
    if (result) return result;
    result = wait(ticket, 5000000, reply, capacity, actual);
    if (result == kIOReturnTimeout) IOLog("virtio-gpu: command %x timed out\n", reinterpret_cast<const Header *>(data)->type);
    return result;
}
uint32_t resourceId() { IOLockLock(lock); uint32_t id = ++nextResource; IOLockUnlock(lock); return id; }
IOReturn unref(uint32_t id) {
    struct { Header h; uint32_t id, padding; } cmd = {{0x102, 0, 0, 0, 0, {}}, id, 0}; Header reply;
    return command(&cmd, sizeof cmd, nullptr, 0, &reply, sizeof reply);
}
}
