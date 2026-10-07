// SPDX-License-Identifier: GPL-2.0-or-later
// Citrosis Apple Hypervisor EL1 support code. The encodings are embedded in
// src/core/arm/hypervisor/hypervisor_el1.inc; regenerate it with
//   python3 tools/macos/gen_hypervisor_el1.py
// after editing (needs an AArch64 GNU assembler, e.g. aarch64-linux-gnu-as).
//
// Offset 0x0000: VBAR_EL1 (kernel VA page 1). Every vector hands the exception to the
//   host with HVC #0xc175, except "lower EL synchronous" (0x400), which handles common
//   EL0 traps inside the VM:
//     MRS CNTVCT/CNTPCT_EL0 -> (CNTVCT + offset) scaled to 19.2 MHz with UMULH
//     MRS CNTFRQ_EL0 -> 19.2 MHz, MRS CTR_EL0 -> Cortex-A57 geometry
//     WFE/WFI -> skipped
// Offset 0x1000: entry stubs (kernel VA page 2), entered at EL1 with ELR/SPSR set to the
//   guest state; they ERET to EL0 after the requested maintenance.
// Per-vCPU frame (kernel VA page 3, 64 bytes, SP_EL1 = frame + 32 on exception entry):
//   [frame+0..23] saved x0-x2, [frame+32] counter offset, [frame+40] 0.64 tick factor.
// On cancellation the host restores x0-x2 from the frame when SP_EL1 shows they were
// saved. Counter/wait handlers resume at ELR_EL1; host-dispatched exceptions must still
// be delivered (SVC has already advanced ELR beyond the supervisor-call instruction).
    .text
vector_page:
    .org 0x0
    hvc #0xc175
    .org 0x80
    hvc #0xc175
    .org 0x100
    hvc #0xc175
    .org 0x180
    hvc #0xc175
    .org 0x200
    hvc #0xc175
    .org 0x280
    hvc #0xc175
    .org 0x300
    hvc #0xc175
    .org 0x380
    hvc #0xc175
    .org 0x400
lower_sync:                        // copied to VBAR + 0x400 (32 instruction slot)
    stp x0, x1, [sp, #-32]!
    str x2, [sp, #16]
    mrs x0, esr_el1
    lsr x1, x0, #26
    cmp x1, #0x18
    b.eq 10f
    cmp x1, #0x01
    b.eq 20f
1:  ldr x2, [sp, #16]
    ldp x0, x1, [sp], #32
    hvc #0xc175
    // Far targets are placed at VBAR + 0x800 by the loader (see helper below);
    // these branches are patched relative, so keep them as labels into helper.
10: b sysreg_trap
20: b wait_trap

    .org 0x480
    hvc #0xc175
    .org 0x500
    hvc #0xc175
    .org 0x580
    hvc #0xc175
    .org 0x600
    hvc #0xc175
    .org 0x680
    hvc #0xc175
    .org 0x700
    hvc #0xc175
    .org 0x780
    hvc #0xc175
    .org 0x800
helper:                            // copied to VBAR + 0x800
sysreg_trap:
    ubfx x1, x0, #0, #25           // ISS
    bic x1, x1, #0x3e0             // drop Rt
    ubfx x0, x0, #5, #5            // x0 = Rt
    movz w2, #0xf801
    movk w2, #0x34, lsl #16        // MRS CNTVCT_EL0
    cmp w1, w2
    b.eq 30f
    movk w2, #0x32, lsl #16        // MRS CNTPCT_EL0
    cmp w1, w2
    b.eq 30f
    movk w2, #0x30, lsl #16        // MRS CNTFRQ_EL0
    cmp w1, w2
    b.eq 31f
    movz w2, #0xc001
    movk w2, #0x32, lsl #16        // MRS CTR_EL0
    cmp w1, w2
    b.eq 32f
    // Anything else: restore and hand the exception to the host.
    ldr x2, [sp, #16]
    ldp x0, x1, [sp], #32
    hvc #0xc175
30: mrs x1, cntvct_el0
    ldr x2, [sp, #32]
    add x1, x1, x2
    ldr x2, [sp, #40]
    umulh x1, x1, x2
    b 40f
31: movz x1, #0xf800
    movk x1, #0x124, lsl #16       // 19.2 MHz
    b 40f
32: movz x1, #0xc004
    movk x1, #0x8444, lsl #16      // Cortex-A57 cache geometry
40: cmp x0, #3
    b.lo 41f
    cmp x0, #31
    b.eq wait_trap
    adr x2, 50f
    add x2, x2, x0, lsl #3
    br x2
41: str x1, [sp, x0, lsl #3]
wait_trap:                         // WFE/WFI and finished MRS: skip the instruction
    mrs x0, elr_el1
    add x0, x0, #4
    msr elr_el1, x0
    ldr x2, [sp, #16]
    ldp x0, x1, [sp], #32
    eret
50:
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    mov x\n, x1
    b wait_trap
    .endr

    .org 0x1000
stubs:                             // copied to kernel page 2 (FlushAddress)
stub_clrex:                        // +0x00 context switch only
    clrex
    eret
    .balign 16
stub_tlb:                          // +0x10
    // Inner-shareable (broadcast): all vCPUs of the VM share one VMID and ASID, so a
    // stale entry left on any physical core could be used by whichever vCPU runs there next.
    clrex
    dsb ish
    tlbi vmalle1is
    dsb ish
    isb
    eret
    .balign 16
stub_tlb_ic:                       // +0x30
    clrex
    dsb ish
    tlbi vmalle1is
    ic ialluis
    dsb ish
    isb
    eret
    .balign 16
stub_counter:                      // +0x50 calibration
    mrs x0, cntvct_el0
    hvc #0xc176

    .org 0x1100
