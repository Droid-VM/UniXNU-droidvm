// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modern virtio-mmio GPU transport. No renderer or display policy belongs here.
#include "DroidVMVirtioGpu.h"
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#if DROIDVM_VIRTIO_GPU_DYNAMIC
#include <IOKit/IOBufferMemoryDescriptor.h>
#endif
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
// ArmCpu (the GIC): a handler for one SPI, run in the primary interrupt context before the EOI.
extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                                      uint64_t *router);
extern "C" {
volatile uint32_t droidvm_virtio_gpu_state, droidvm_virtio_gpu_last_command, droidvm_virtio_gpu_last_response;
volatile uint64_t droidvm_virtio_gpu_commands, droidvm_virtio_gpu_errors, droidvm_virtio_gpu_timeouts;
volatile uint32_t droidvm_virtio_gpu_in_flight, droidvm_virtio_gpu_in_flight_peak;
// Commands that waited 5 s or more for a free slot, and the longest wait (us): a full queue is backpressure, not a failure.
volatile uint64_t droidvm_virtio_gpu_slot_waits_long, droidvm_virtio_gpu_slot_wait_max_us;
// Used-ring elements that named no command in flight even when read again (skipped, not fatal): [0] count, [1] the last one's
// head, [2] its slot's state, [3] the used index it was read at. [4] commands a closed client left behind (abandon()).
volatile uint64_t droidvm_virtio_gpu_stray[5];
// The waiters' one watch on the used ring (awaitReturn): [0] waits, [1] waits that kept the watch, [2] sleeps behind the watch,
// [3] the watch's looks at the ring, [4] its sleeps on the interrupt, [5] of those not taken (a return found as it armed),
// [6] interrupts, [7] interrupt sleeps that ran to their guard, [8] waits that ran to their deadline.
volatile uint64_t droidvm_virtio_gpu_waits[9];
// The device's interrupt (the ADT's droidvm,intid, 0 without one) and what attaching it answered; the switches: irq 0 keeps the
// watch polling, spin_us is how long it polls before it sleeps on the interrupt.
volatile uint32_t droidvm_virtio_gpu_intid, droidvm_virtio_gpu_irq_attach, droidvm_virtio_gpu_irq = 1, droidvm_virtio_gpu_spin_us = 300;
// 1: command() numbers its command on the global fence ring, as the 2D display's commands once were. crosvm answers such a command
// only when the fence after it signals, and a global fence is vrend's: a GL fence on the host's GL context, queued on the GPU behind
// the guest's own work, which it does not even order against (that is KGSL's, through drm2kgsl). Every CREATE_BLOB, MAP_BLOB and
// unref of a 3D client then waited for the GPU's frame in flight: 10-17 ms each on the render server's display thread as a view
// first came up (a keyboard, Control Center, the widgets), whatever the blob's size. 0, the default: unfenced, answered as soon as
// the device has done it; the display's flips number their own fences (post).
volatile uint32_t droidvm_virtio_gpu_command_fence = 0;
// Synchronous commands' two legs, as wait() collects each: the device's -- from the doorbell to reap() taking the answer off the
// used ring: the kick reaching crosvm, its queue and work, and the answer coming back -- and the waiter's wakeup, from there to it
// holding the answer. By command: [0] unfenced SUBMIT_3D (a ccmd such as GEM_NEW), [1] CREATE_BLOB, [2] unref, [3] the rest; in
// log2 buckets of microseconds, <32, <64 ... <32768, more. Then the last 32 commands whose legs came to 4 ms or more: posted
// when (us), the command, the device's and the wakeup's microseconds, and the commands in flight as it was posted.
volatile uint32_t droidvm_virtio_gpu_legs[4][2][12];
struct DroidVMGpuSlowLeg { uint64_t at; uint32_t type, deviceUs, wakeUs, inFlight; };
DroidVMGpuSlowLeg droidvm_virtio_gpu_slow_legs[32];
volatile uint32_t droidvm_virtio_gpu_slow_legs_count;
// The two transport legs inside the device's: the doorbell's own write (trapped by the hypervisor to crosvm's vCPU thread, which
// signals the queue's worker), and for an answer reaped after the interrupt, from the interrupt to reap(); buckets as above.
volatile uint32_t droidvm_virtio_gpu_kick_us[12], droidvm_virtio_gpu_irq_reap_us[12];
// Commands posted detached (postDetached: nobody waits, reap() frees the slot), and of them those the device answered with an error.
volatile uint64_t droidvm_virtio_gpu_detached[2];
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
    uint64_t postedUs, doneUs;   // the doorbell, and reap() taking its answer (droidvm_virtio_gpu_legs)
    uint32_t postedInFlight;
    bool detached;   // postDetached: nobody collects the answer
    uint32_t requestCapacity, responseCapacity, capacity, length, type;
    uint16_t generation;
    uint8_t state;
    bool fenced;
    IOReturn result;
};
#if DROIDVM_VIRTIO_GPU_DYNAMIC
// A board whose kernel data has no room for them (the iPadOS 27 fileset: the native __DATA must fit the original kernel's slot)
// takes the queues and buffers below from one physically contiguous allocation when the device starts; the arrays become pointers
// to their rows, and physical() answers from the allocation's base.
uint8_t (*queues)[16384];
uint8_t (*requests)[SlotRequest];
uint8_t (*responses)[SlotResponse];
uint8_t (*largeRequest)[MaxCommand], (*largeResponse)[MaxCommand];
uint8_t *hugeRequest;
IOBufferMemoryDescriptor *buffers;
uintptr_t buffersBase;
uint64_t buffersPA, buffersBytes;
#else
alignas(16384) uint8_t queues[2][16384];
alignas(16384) uint8_t requests[Large][SlotRequest];
alignas(4096) uint8_t responses[Large][SlotResponse];
alignas(16384) uint8_t largeRequest[LargeSlots][MaxCommand], largeResponse[LargeSlots][MaxCommand];
#endif
// The one request buffer for a command longer than MaxCommand, lent to a large slot until the device returns it (it has read the
// request by then): a CREATE_BLOB_USER over a process's scattered pages -- a 44.5 MiB no-copy buffer is ~11,400 runs, 180 KiB of
// entries -- which was refused, and Metal took the refusal for a lost device (Core Animation ended backboardd: a respring each
// time the wallpaper editor opened). Such commands are rare; another one waits for the buffer as a command waits for a slot.
#if !DROIDVM_VIRTIO_GPU_DYNAMIC
alignas(16384) uint8_t hugeRequest[HugeRequest];
#endif
uint64_t hugeRequestPA;
uint32_t hugeOwner = Slots;   // the slot the buffer is lent to; Slots when free
Slot slots[Slots];
uintptr_t registers;
uint64_t negotiated, nextFence, lastReturnUs, lastIrqUs;
uint32_t nextResource;
uint16_t available, consumed;
IOLock *lock;
bool attempted;
// Commands taken off the used ring so far: what a waiter (awaitReturn) watches for. With the lock held.
uint64_t returned;
// A waiter watches the used ring for every waiter (awaitReturn); the others sleep on `returned` until it moves.
bool watching;
uint32_t rd(unsigned offset) {
    uint32_t v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(registers + offset) : "memory"); return v;
}
void wr(unsigned offset, uint32_t v) {
    __asm__ volatile("str %w0, [%1]\n\tdsb oshst" :: "r"(v), "r"(registers + offset) : "memory");
}
void wr64(unsigned offset, uint64_t v) { wr(offset, uint32_t(v)); wr(offset + 4, uint32_t(v >> 32)); }
void barrier() { __asm__ volatile("dmb osh" ::: "memory"); }
uint64_t nowUs() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
// The log2 bucket of `us` microseconds in the legs' histograms: <32, <64 ... <32768, more.
unsigned usBucket(uint64_t us) { unsigned b = 0; for (uint64_t v = us >> 5; v && b < 11; v >>= 1) ++b; return b; }
unsigned legKind(uint32_t type) { return type == 0x207 ? 0 : type == 0x10c ? 1 : type == 0x102 ? 2 : 3; }
bool physical(void *p, size_t size, uint64_t &pa) {
#if DROIDVM_VIRTIO_GPU_DYNAMIC
    if (buffers && uintptr_t(p) >= buffersBase && uintptr_t(p) - buffersBase <= buffersBytes - size) {
        pa = buffersPA + (uintptr_t(p) - buffersBase);
        return true;
    }
#endif
    pa = ml_static_vtop(uintptr_t(p));
    for (size_t i = 0; i < size; i += 4096) if (ml_static_vtop(uintptr_t(p) + i) != pa + i) return false;
    return pa != 0;
}
bool fail(const char *why) { droidvm_virtio_gpu_state = 3; IOLog("virtio-gpu: %s\n", why); return false; }
// The used ring is guest memory, so looking at it costs no exit; one waiter does it for all of them (awaitReturn), and sleeps on the
// device's interrupt once a wait has gone on for a while. A wait spins for its first 200 us and then sleeps, so the vCPUs of waiting threads go idle (WFI, on which the
// hypervisor blocks them): four vCPUs that spun through commands the host was slow to return came before each host watchdog
// bite on 2026-10-25, when this spun for 2 ms.
// A waiter's step between looks at the used ring (the device raises no interrupt): a short spin, then sleeps of 100 us -- not
// IOSleep(1), whose millisecond (and timer leeway) every command's completion paid.
void pause(uint64_t begin) { if (nowUs() - begin < 50) IODelay(5); else delay_for_interval(100, NSEC_PER_USEC); }
// Takes every command the device has returned off the used ring, in whatever order it returned them (a fenced command comes back
// when its fence signals). Called with the lock held, by whichever thread is waiting.
// The element is read only after a barrier that follows the index read: ARM reorders loads, and without one a waiter that saw the
// new index could take the slot's element from the ring's previous lap -- a command long returned, whose slot was free or done.
// That stopped the transport for good on 2026-11-01 (every later frame refused, the screen frozen) while a burst of clients ended.
// An element that still names no command in flight is skipped and counted (droidvm_virtio_gpu_stray), not taken as a dead device.
bool expected(uint32_t head) {
    return head < QueueSize && !(head & 1) && (slots[head / 2].state == Posted || slots[head / 2].state == Abandoned);
}
void reap() {
    auto used = reinterpret_cast<volatile uint16_t *>(queues[0] + UsedOffset);
    bool any = false;
    for (uint16_t end = used[1]; consumed != end; end = used[1]) {
        barrier();
        while (consumed != end) {
            auto element = reinterpret_cast<volatile Used *>(queues[0] + UsedOffset + 4) + consumed % QueueSize;
            uint32_t head = element->id, length = element->length;
            if (!expected(head)) { barrier(); head = element->id; length = element->length; }
            ++consumed; any = true; lastReturnUs = nowUs();
            if (!expected(head)) {
                droidvm_virtio_gpu_stray[0] = droidvm_virtio_gpu_stray[0] + 1;
                droidvm_virtio_gpu_stray[1] = head;
                droidvm_virtio_gpu_stray[2] = head < QueueSize ? slots[head / 2].state : 0xff;
                droidvm_virtio_gpu_stray[3] = uint16_t(consumed - 1);
                IOLog("virtio-gpu: used element %u names descriptor %u, which no command is waiting on (slot state %u); skipped\n",
                      uint16_t(consumed - 1), head, unsigned(head < QueueSize ? slots[head / 2].state : 0xff));
                continue;
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
            slot.doneUs = nowUs();
            if (!slot.fenced) {
                const uint64_t device = slot.doneUs - slot.postedUs;
                const unsigned kind = legKind(slot.type), bucket = usBucket(device);
                droidvm_virtio_gpu_legs[kind][0][bucket] = droidvm_virtio_gpu_legs[kind][0][bucket] + 1;
                if (lastIrqUs > slot.postedUs) {
                    const unsigned b = usBucket(slot.doneUs - lastIrqUs);
                    droidvm_virtio_gpu_irq_reap_us[b] = droidvm_virtio_gpu_irq_reap_us[b] + 1;
                }
                if (device >= 4000) {
                    const uint32_t n = droidvm_virtio_gpu_slow_legs_count;
                    droidvm_virtio_gpu_slow_legs_count = n + 1;
                    droidvm_virtio_gpu_slow_legs[n % 32] = {slot.postedUs, slot.type, uint32_t(device < 0xffffffffu ? device : 0xffffffffu),
                                                            slot.detached ? 1u : 0u, slot.postedInFlight};
                }
            }
            if (result && slot.detached) droidvm_virtio_gpu_detached[1] = droidvm_virtio_gpu_detached[1] + 1;
            slot.state = slot.state == Abandoned ? Free : Done;
            if (hugeOwner == head / 2) hugeOwner = Slots;
            droidvm_virtio_gpu_in_flight = droidvm_virtio_gpu_in_flight - 1;
        }
    }
    if (any) {
        uint32_t interrupts = rd(0x60) & 3; if (interrupts) wr(0x64, interrupts);
        ++returned;
        IOLockWakeup(lock, &returned, false);
    }
}
// The control queue's interrupt is off (the avail ring's NO_INTERRUPT flag) except while the watch sleeps on it: the device
// returns hundreds of commands a second, and only a wait that has gone past its spin wants to hear of one.
void interruptsOn(bool on) {
    *reinterpret_cast<volatile uint16_t *>(queues[0] + AvailOffset) = on ? 0 : 1;
    __asm__ volatile("dmb osh" ::: "memory");
}
bool returnPending() { return reinterpret_cast<volatile uint16_t *>(queues[0] + UsedOffset)[1] != consumed; }
// The device's interrupt, in the primary interrupt context: the line is level, so the status is acknowledged here; the watch
// reaps. Every waiter is woken, not only the watch: a waiter behind a watch that cannot run (a lower-priority thread) still sees
// its command come back.
void interrupt(void *) {
    lastIrqUs = nowUs();
    uint32_t status = rd(0x60) & 3;
    if (status) wr(0x64, status);
    droidvm_virtio_gpu_waits[6] = droidvm_virtio_gpu_waits[6] + 1;
    thread_wakeup_prim(reinterpret_cast<event_t>(&watching), FALSE, THREAD_AWAKENED);
    thread_wakeup_prim(reinterpret_cast<event_t>(&returned), FALSE, THREAD_AWAKENED);
}
uint64_t deadlineAfter(uint64_t us) { uint64_t abs; clock_interval_to_deadline(uint32_t(us), NSEC_PER_USEC, &abs); return abs; }
}
#if DROIDVM_VIRTIO_GPU_DYNAMIC
// One physically contiguous, wired allocation for the queues and command buffers, each region 16 KiB aligned.
bool allocateBuffers() {
    if (buffers) return true;
    const uint64_t sizes[] = {sizeof(*queues) * 2, sizeof(*requests) * Large, sizeof(*responses) * Large,
                              sizeof(*largeRequest) * LargeSlots, sizeof(*largeResponse) * LargeSlots, HugeRequest};
    uint64_t at[6], total = 0;
    for (unsigned i = 0; i < 6; ++i) { at[i] = total; total = (total + sizes[i] + 16383) & ~uint64_t(16383); }
    buffers = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous),
                                                               total, 0x0000fffffffff000ULL);
    if (!buffers) return false;
    buffersBase = uintptr_t(buffers->getBytesNoCopy());
    buffersPA = buffers->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    buffersBytes = total;
    if (!buffersBase || !buffersPA) return false;
    memset(reinterpret_cast<void *>(buffersBase), 0, total);
    queues = reinterpret_cast<uint8_t (*)[16384]>(buffersBase + at[0]);
    requests = reinterpret_cast<uint8_t (*)[SlotRequest]>(buffersBase + at[1]);
    responses = reinterpret_cast<uint8_t (*)[SlotResponse]>(buffersBase + at[2]);
    largeRequest = reinterpret_cast<uint8_t (*)[MaxCommand]>(buffersBase + at[3]);
    largeResponse = reinterpret_cast<uint8_t (*)[MaxCommand]>(buffersBase + at[4]);
    hugeRequest = reinterpret_cast<uint8_t *>(buffersBase + at[5]);
    return true;
}
#endif
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
#if DROIDVM_VIRTIO_GPU_DYNAMIC
    if (!allocateBuffers()) return fail("no contiguous command buffers");
#endif
    for (uint32_t s = 0; s < Slots; ++s) {
        Slot &slot = slots[s];
        slot.request = s >= Large ? largeRequest[s - Large] : requests[s];
        slot.response = s >= Large ? largeResponse[s - Large] : responses[s];
        slot.requestCapacity = s >= Large ? uint32_t(MaxCommand) : uint32_t(SlotRequest);
        slot.responseCapacity = s >= Large ? uint32_t(MaxCommand) : uint32_t(SlotResponse);
        if (!physical(slot.request, slot.requestCapacity, slot.requestPA) || !physical(slot.response, slot.responseCapacity, slot.responsePA))
            return fail("noncontiguous command buffers");
    }
    if (!physical(hugeRequest, HugeRequest, hugeRequestPA)) return fail("noncontiguous command buffers");
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
    // The window's interrupt (DarwinBoot copies it from the FDT): without one, the watch never sleeps on it and keeps polling.
    if (SecureDTGetProperty(node, "droidvm,intid", &value, &size) == kSuccess && size == sizeof(uint32_t)) {
        uint32_t intid; memcpy(&intid, value, sizeof intid);
        droidvm_virtio_gpu_irq_attach = armcpu_spi_attach(intid, false, interrupt, nullptr, nullptr, nullptr);
        if (droidvm_virtio_gpu_irq_attach == kIOReturnSuccess) droidvm_virtio_gpu_intid = intid;
    }
    droidvm_virtio_gpu_state = 2;
    IOLog("virtio-gpu: control queue of %u commands and cursor queue ready, features %llx, interrupt %u (%x)\n", Slots, negotiated,
          droidvm_virtio_gpu_intid, droidvm_virtio_gpu_irq_attach);
    return true;
}
bool ready() { return droidvm_virtio_gpu_state == 2; }
uint64_t returns() { IOLockLock(lock); uint64_t n = returned; IOLockUnlock(lock); return n; }
// Until the device returns a command after `seen` (returns()), or nowUs() reaches deadlineUs. One waiter keeps the watch on the used
// ring for all of them and the rest sleep: the device returns every command on one ring, so one look serves every process's
// waits, where each waiter polling on its own woke ten thousand times a second (a quarter of a vCPU in page swipes, most of it
// the render server's fence waits). The watch polls for spin_us -- a command the host answers at once comes back without a
// sleep -- then turns the queue's interrupt on and sleeps until it fires. A watch that leaves hands the watch to the waiters.
void awaitReturn(uint64_t seen, uint64_t deadlineUs) {
    if (!lock) return;
    droidvm_virtio_gpu_waits[0] = droidvm_virtio_gpu_waits[0] + 1;
    IOLockLock(lock);
    reap();
    while (returned == seen && ready()) {
        const uint64_t now = nowUs();
        if (now >= deadlineUs) { droidvm_virtio_gpu_waits[8] = droidvm_virtio_gpu_waits[8] + 1; break; }
        if (watching) {
            // Bounded: should the watch's thread not get to run, a waiter looks for itself every 2 ms.
            droidvm_virtio_gpu_waits[2] = droidvm_virtio_gpu_waits[2] + 1;
            const uint64_t step = deadlineUs - now < 2000 ? deadlineUs - now : 2000;
            IOLockSleepDeadline(lock, &returned, deadlineAfter(step), THREAD_UNINT);
            reap();
            continue;
        }
        watching = true;
        droidvm_virtio_gpu_waits[1] = droidvm_virtio_gpu_waits[1] + 1;
        const uint64_t begin = now;
        while (returned == seen && ready() && nowUs() < deadlineUs) {
            const uint64_t at = nowUs();
            if (droidvm_virtio_gpu_intid && droidvm_virtio_gpu_irq && at - begin >= droidvm_virtio_gpu_spin_us) {
                // On the waiter's own wait queue first, then the interrupt on, then one more look: a command returned between
                // the last look and the interrupt raised none, and one returned after it wakes this thread before it blocks.
                // Guarded at 20 ms, so an interrupt that never comes costs a wait that much at worst.
                const uint64_t left = deadlineUs - at, guard = left < 20000 ? left : 20000;
                assert_wait_deadline(reinterpret_cast<event_t>(&watching), THREAD_UNINT, deadlineAfter(guard));
                interruptsOn(true);
                droidvm_virtio_gpu_waits[4] = droidvm_virtio_gpu_waits[4] + 1;
                if (returnPending()) {
                    clear_wait(current_thread(), THREAD_AWAKENED);
                    droidvm_virtio_gpu_waits[5] = droidvm_virtio_gpu_waits[5] + 1;
                } else {
                    IOLockUnlock(lock);
                    if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT && guard == 20000)
                        droidvm_virtio_gpu_waits[7] = droidvm_virtio_gpu_waits[7] + 1;
                    IOLockLock(lock);
                }
                interruptsOn(false);
            } else {
                IOLockUnlock(lock); pause(begin); IOLockLock(lock);
            }
            droidvm_virtio_gpu_waits[3] = droidvm_virtio_gpu_waits[3] + 1;
            reap();
        }
        watching = false;
        IOLockWakeup(lock, &returned, false);
    }
    IOLockUnlock(lock);
}
uint64_t features() { return negotiated; }
IOReturn post(const void *data, uint32_t size, const void *extra, uint32_t extraSize, uint32_t capacity, bool globalFence,
              uint32_t *ticket) {
    if (ticket) *ticket = 0;
    if (!ready()) return kIOReturnNotReady;
    if (!ticket || !data || size < sizeof(Header) || size > MaxCommand || extraSize > HugeRequest - size || (extraSize && !extra) ||
        capacity < sizeof(Header) || capacity > MaxCommand) return kIOReturnBadArgument;
    const bool huge = size + extraSize > MaxCommand;
    const bool large = huge || size + extraSize > SlotRequest || capacity > SlotResponse;
    const uint64_t begin = nowUs();
    IOLockLock(lock);
    uint32_t s = Slots;
    bool longWait = false;
    while (s == Slots) {
        if (!ready()) { IOLockUnlock(lock); return kIOReturnNotResponding; }
        reap();
        // A short command takes a large slot too when the short ones are all in flight; a huge one a large slot and hugeRequest.
        if (!huge || hugeOwner == Slots)
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
        const uint64_t seen = returned;
        IOLockUnlock(lock); awaitReturn(seen, begin + 60000000); IOLockLock(lock);
    }
    const uint64_t waited = nowUs() - begin;
    if (waited > droidvm_virtio_gpu_slot_wait_max_us) droidvm_virtio_gpu_slot_wait_max_us = waited;
    Slot &slot = slots[s];
    uint8_t *const request = huge ? hugeRequest : slot.request;
    if (huge) hugeOwner = s;
    memcpy(request, data, size); if (extraSize) memcpy(request + size, extra, extraSize);
    Header *hdr = reinterpret_cast<Header *>(request);
    if (globalFence) { hdr->flags = (hdr->flags | FlagFence) & ~uint32_t(FlagRingIndex); hdr->fence = ++nextFence; }
    slot.fenced = hdr->flags & FlagFence; slot.fence = hdr->fence; slot.type = hdr->type;
    slot.capacity = capacity; slot.length = 0; slot.result = kIOReturnNotReady;
    memset(slot.response, 0, capacity);
    auto desc = reinterpret_cast<Desc *>(queues[0]);
    desc[2 * s] = {huge ? hugeRequestPA : slot.requestPA, size + extraSize, 1, uint16_t(2 * s + 1)};
    desc[2 * s + 1] = {slot.responsePA, capacity, 2, 0};
    slot.state = Posted;
    if (++slot.generation == 0) slot.generation = 1;
    *ticket = uint32_t(slot.generation) << 8 | s;
    const uint32_t inFlight = droidvm_virtio_gpu_in_flight + 1;
    droidvm_virtio_gpu_in_flight = inFlight;
    if (inFlight > droidvm_virtio_gpu_in_flight_peak) droidvm_virtio_gpu_in_flight_peak = inFlight;
    droidvm_virtio_gpu_last_command = hdr->type;
    auto avail = reinterpret_cast<volatile uint16_t *>(queues[0] + AvailOffset);
    slot.postedUs = nowUs(); slot.postedInFlight = inFlight; slot.detached = false;
    avail[2 + available % QueueSize] = uint16_t(2 * s); barrier(); avail[1] = ++available; barrier();
    const uint64_t kick = nowUs();
    wr(0x50, 0);
    const unsigned kb = usBucket(nowUs() - kick);
    droidvm_virtio_gpu_kick_us[kb] = droidvm_virtio_gpu_kick_us[kb] + 1;
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
    if (!slot.fenced) {   // the waiter's leg; the device's was taken in reap()
        const unsigned kind = legKind(slot.type), bucket = usBucket(nowUs() - slot.doneUs);
        droidvm_virtio_gpu_legs[kind][1][bucket] = droidvm_virtio_gpu_legs[kind][1][bucket] + 1;
    }
    const uint32_t length = slot.length < capacity ? slot.length : capacity;
    if (reply && length) memcpy(reply, slot.response, length);
    if (actual) *actual = length;
    if (result) *result = slot.result;
    slot.state = Free;
    IOLockUnlock(lock);
    return true;
}
// A command nobody waits for: posted as any, its slot goes back to the queue when the device returns it (reap()), the answer
// unread but for an error, which is counted (droidvm_virtio_gpu_detached[1]) and logged. The device takes the control queue's
// commands in order, so a later command that names what this one makes comes after it.
IOReturn postDetached(const void *data, uint32_t size, const void *extra, uint32_t extraSize, uint32_t capacity) {
    uint32_t ticket = 0;
    const IOReturn result = post(data, size, extra, extraSize, capacity, false, &ticket);
    if (result) return result;
    IOLockLock(lock);
    Slot &slot = slots[ticket & 0xff];
    if (slot.generation == ticket >> 8) {
        slot.detached = true;
        if (slot.state == Posted) slot.state = Abandoned;
        else if (slot.state == Done) {
            if (slot.result) droidvm_virtio_gpu_detached[1] = droidvm_virtio_gpu_detached[1] + 1;
            slot.state = Free;
        }
    }
    droidvm_virtio_gpu_detached[0] = droidvm_virtio_gpu_detached[0] + 1;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}
// A command whose response nobody will collect (its client closed before it returned): its slot goes back to the queue when the
// device returns it, or now if it has. Without this every ticket a dying client left behind held its slot for good.
void abandon(uint32_t ticket) {
    const uint32_t s = ticket & 0xff;
    if (s >= Slots || !lock) return;
    IOLockLock(lock);
    reap();
    Slot &slot = slots[s];
    if (slot.generation == ticket >> 8) {
        if (slot.state == Posted) slot.state = Abandoned;
        else if (slot.state == Done) slot.state = Free;
        droidvm_virtio_gpu_stray[4] = droidvm_virtio_gpu_stray[4] + 1;
    }
    IOLockUnlock(lock);
}
IOReturn wait(uint32_t ticket, uint64_t timeoutUs, void *reply, uint32_t capacity, uint32_t *actual) {
    const uint64_t begin = nowUs();
    IOReturn result = kIOReturnSuccess;
    for (uint64_t seen = returns(); !poll(ticket, &result, reply, capacity, actual); seen = returns()) {
        if (nowUs() - begin >= timeoutUs) {
            // The device still owns the slot's buffers: it is freed only when the device returns it.
            IOLockLock(lock);
            Slot &slot = slots[ticket & 0xff];
            if (slot.generation == ticket >> 8 && slot.state == Posted) slot.state = Abandoned;
            IOLockUnlock(lock);
            droidvm_virtio_gpu_timeouts = droidvm_virtio_gpu_timeouts + 1;
            return kIOReturnTimeout;
        }
        awaitReturn(seen, begin + timeoutUs);
    }
    return result;
}
IOReturn command(const void *data, uint32_t size, const void *extra, uint32_t extraSize,
                 void *reply, uint32_t capacity, uint32_t *actual) {
    if (actual) *actual = 0;
    uint32_t ticket = 0;
    IOReturn result = post(data, size, extra, extraSize, capacity, droidvm_virtio_gpu_command_fence != 0, &ticket);
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
