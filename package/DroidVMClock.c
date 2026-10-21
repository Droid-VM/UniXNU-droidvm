// SPDX-License-Identifier: MIT
// The guest's calendar clock from crosvm's PL030 real-time clock, set before launchd starts (native_board_launch_policy).
//
// XNU takes the time of day from the platform's RTC service, and this board has none: the BCM2837 configuration this kernel is
// built from does not wait for one (IOKitInitializeTime), so the calendar starts at the epoch and SpringBoard shows 31 December
// 1969. crosvm's aarch64 machine always has a PL030 at 0x2000 (its FDT: rtc@2000, arm,primecell-periphid 0x41030), whose data
// register (RTCDR, offset 0) reads the host's UNIX time in seconds. Only that register is read, as one 32-bit access: crosvm
// panics on a read of an offset it does not decode. droidvm-rtc=0 leaves the clock alone.
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

enum {
 DROIDVM_RTC_BASE=0x2000,                // crosvm AARCH64_RTC_ADDR
 DROIDVM_RTC_SIZE=0x1000,
 DROIDVM_RTC_EARLIEST=1577836800,        // 2020-01-01: anything earlier is no host clock
};

volatile uint32_t droidvm_rtc_seconds;
volatile int droidvm_rtc_status=-1;

void droidvm_rtc_set_calendar(void) {
 static int done;
 uint32_t enabled=1;
 if(done)return;
 done=1;
 if(PE_parse_boot_argn("droidvm-rtc",&enabled,sizeof(enabled)) && !enabled){droidvm_rtc_status=0;return;}
 const vm_offset_t rtc=ml_io_map(DROIDVM_RTC_BASE,DROIDVM_RTC_SIZE);
 if(!rtc){droidvm_rtc_status=ENOMEM;return;}
 const uint32_t seconds=*(volatile uint32_t *)rtc;
 droidvm_rtc_seconds=seconds;
 if(seconds<DROIDVM_RTC_EARLIEST){droidvm_rtc_status=EINVAL;printf("droidvm rtc: %u is no host time\n",seconds);return;}
 clock_set_calendar_microtime(seconds,0);
 droidvm_rtc_status=0;
 printf("droidvm rtc: calendar set to %u\n",seconds);
}
