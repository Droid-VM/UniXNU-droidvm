// pacprobe: a Linux-Image-format payload for crosvm (Gunyah, pseudo-unprotected) that reports whether the vCPU it runs on gives an
// EL1 guest working ARMv8.3 pointer authentication (what an arm64e XNU needs): the ID registers, the five key registers, and the
// PAC instructions at EL1 and EL0. MMU off, one vCPU, output on the 8250 at 0x3f8. Every exception is counted and stepped over.
        .arch armv8.5-a+pauth
        .section .text.head, "ax"
        .globl _head
_head:
        b       start                   // code0
        .long   0                       // code1
        .quad   0                       // text_offset
        .quad   _end - _head            // image_size
        .quad   0xa                     // flags: little-endian, placed anywhere
        .quad   0, 0, 0
        .ascii  "ARM\x64"
        .long   0

        .text
start:
        msr     daifset, #0xf
        mov     x9, #0x3f8
        mov     w10, #'P'
        strb    w10, [x9]
        adr     x1, stack_top
        mov     sp, x1
        adr     x1, vectors
        msr     vbar_el1, x1
        isb
        bl      probe_main
        b       poweroff

// PSCI SYSTEM_OFF over HVC, again and again: never WFE (a guest parked in WFE, killed from the host, once wedged a host core
// in Gunyah and the watchdog reset the phone).
poweroff:
        mov     x0, #0x0008
        movk    x0, #0x8400, lsl #16
        hvc     #0
        b       poweroff

// x0 = EL0 routine, x1 = its argument (results buffer). Runs it at EL0 (SP_EL0 = el0 stack) and returns here when it does SVC #0.
        .globl  run_el0
run_el0:
        stp     x29, x30, [sp, #-96]!
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     x25, x26, [sp, #64]
        stp     x27, x28, [sp, #80]
        adr     x9, el1_saved_sp
        mov     x10, sp
        str     x10, [x9]
        adr     x9, el0_stack_top
        msr     sp_el0, x9
        msr     elr_el1, x0
        mov     x9, #0x3c0              // EL0t, DAIF masked
        msr     spsr_el1, x9
        mov     x0, x1
        isb
        eret
el0_return:
        adr     x9, el1_saved_sp
        ldr     x10, [x9]
        mov     sp, x10
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x23, x24, [sp, #48]
        ldp     x25, x26, [sp, #64]
        ldp     x27, x28, [sp, #80]
        ldp     x29, x30, [sp], #96
        ret

        .macro  vent target
        .balign 128
        b       \target
        .endm
        .balign 2048
vectors:
        vent    exc_el1         // current EL, SP0
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1         // current EL, SPx: sync
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el0         // lower EL, AArch64: sync
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1
        vent    exc_el1

// Records ESR/ELR/FAR, counts, steps over the instruction.
exc_el1:
        sub     sp, sp, #32
        stp     x0, x1, [sp]
        stp     x2, x3, [sp, #16]
        adr     x1, exc_rec
        mrs     x0, esr_el1
        str     x0, [x1, #0]
        mrs     x0, elr_el1
        str     x0, [x1, #8]
        mrs     x0, far_el1
        str     x0, [x1, #16]
        ldr     x0, [x1, #24]
        add     x0, x0, #1
        str     x0, [x1, #24]
        cmp     x0, #64
        b.hi    poweroff
        mrs     x0, elr_el1
        add     x0, x0, #4
        msr     elr_el1, x0
        ldp     x2, x3, [sp, #16]
        ldp     x0, x1, [sp]
        add     sp, sp, #32
        eret

// From EL0: SVC #0 ends the EL0 routine (back to run_el0's caller at EL1); anything else is recorded and stepped over.
exc_el0:
        sub     sp, sp, #32
        stp     x0, x1, [sp]
        stp     x2, x3, [sp, #16]
        mrs     x0, esr_el1
        ubfx    x2, x0, #26, #6
        cmp     x2, #0x15
        b.eq    2f
        adr     x1, exc_rec
        str     x0, [x1, #32]
        mrs     x0, elr_el1
        str     x0, [x1, #40]
        ldr     x0, [x1, #48]
        add     x0, x0, #1
        str     x0, [x1, #48]
        cmp     x0, #64
        b.hi    poweroff
        mrs     x0, elr_el1
        add     x0, x0, #4
        msr     elr_el1, x0
        ldp     x2, x3, [sp, #16]
        ldp     x0, x1, [sp]
        add     sp, sp, #32
        eret
2:      add     sp, sp, #32
        adr     x0, el0_return
        msr     elr_el1, x0
        mov     x0, #0x3c5              // EL1h, DAIF masked
        msr     spsr_el1, x0
        eret

// EL0 routine: x0 = results (u64[8]): [0] PACIA result, [1] AUTIA result, [2] PACGA result, [3] BLRAA reached, [4] XPACI
        .balign 16
        .globl  el0_tests
el0_tests:
        mov     x19, x0
        movz    x1, #0x1234
        movk    x1, #0x0040, lsl #16    // a pointer-like value 0x401234
        mov     x2, #0x77
        mov     x3, x1
        pacia   x3, x2
        str     x3, [x19, #0]
        autia   x3, x2
        str     x3, [x19, #8]
        pacga   x4, x1, x2
        str     x4, [x19, #16]
        adr     x5, el0_callee
        mov     x6, #0x55
        pacia   x5, x6
        mov     x7, #0
        blraa   x5, x6
        str     x7, [x19, #24]
        mov     x3, x1
        pacia   x3, x2
        xpaci   x3
        str     x3, [x19, #32]
        svc     #0
        b       .
el0_callee:
        mov     x7, #1
        ret

        .bss
        .balign 16
exc_rec:        .skip 64        // el1 esr, elr, far, count; el0 esr, elr, count
        .globl  exc_rec
el1_saved_sp:   .skip 8
        .balign 16
        .skip   16384
stack_top:
        .skip   8192
el0_stack_top:
        .balign 4096
        .globl  _end
_end:
