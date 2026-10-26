// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// kern.droidvm_motion: the motion sensors' latest frame and the device orientation it says (DroidVMVirtioInput.c fills
// droidvm_motion), read only, for the owner tool that tells backboardd the device orientation (gfxprobe orient-follow).
// Registered when the input driver finds a motion-sensor device, and only then: a guest without one has no such name.
// Registered at run time rather than through SYSCTL_PROC's linker set, which an object linked in late never reaches.
//
// The value is struct droidvm_motion_sample below (seq, orientation, accel[3], gyro[3]; 32-bit each, the device's units).
#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysctl.h>

struct droidvm_motion { volatile uint32_t seq; int32_t orientation, accel[3], gyro[3]; };
extern struct droidvm_motion droidvm_motion;

struct droidvm_motion_sample { uint32_t seq; int32_t orientation, accel[3], gyro[3]; };

// A whole frame: the writer's seq is odd while it writes, so a copy taken between two equal even reads of it is one.
static int droidvm_motion_sysctl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req) {
    (void)oidp; (void)arg1; (void)arg2;
    struct droidvm_motion_sample sample;
    for (unsigned tries = 0; tries < 1000; ++tries) {
        uint32_t before = droidvm_motion.seq;
        __asm__ volatile("dmb ish" ::: "memory");
        sample.seq = before;
        sample.orientation = droidvm_motion.orientation;
        for (unsigned i = 0; i < 3; ++i) {
            sample.accel[i] = droidvm_motion.accel[i];
            sample.gyro[i] = droidvm_motion.gyro[i];
        }
        __asm__ volatile("dmb ish" ::: "memory");
        if (!(before & 1) && droidvm_motion.seq == before) break;
    }
    return SYSCTL_OUT(req, &sample, sizeof(sample));
}

static struct sysctl_oid droidvm_motion_oid = {
    .oid_parent = &sysctl__kern_children,
    .oid_number = OID_AUTO,
    // CTLFLAG_OID2: the structure carries oid_descr and oid_version (SYSCTL_OID sets it; arm64 panics without it).
    .oid_kind = CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_name = "droidvm_motion",
    .oid_handler = droidvm_motion_sysctl,
    .oid_fmt = "S,droidvm_motion",
    .oid_descr = "motion sensors: seq, orientation (UIDeviceOrientation), acceleration[3], angular velocity[3]",
    .oid_version = SYSCTL_OID_VERSION,
};

void droidvm_motion_register(void) {
    static bool registered;
    if (registered) return;
    registered = true;
    sysctl_register_oid(&droidvm_motion_oid);
}
