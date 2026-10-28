# Portions derived from LiteX (BSD 2-Clause, (c) Enjoy-Digital & LiteX developers; see NOTICE)
# MIRLO's boot ROM on the MIPS SoC (mips_core resets to 0xBFC0_0000, KSEG1:
# uncached, so nothing here touches SDRAM until the game is in).
#
# The Pocket loads data slot 0 (the game) into SDRAM at 0x4000_0000 after
# the SDRAM said it is ready (rtl/soc/mirlo_mips.sv: boot_ready), then
# releases its reset (APF_BRIDGE HOST_STATE bit 0; bit 1: a slot loaded).
# Nothing else may use SDRAM meanwhile -- the bridge's writes have no
# backpressure -- so the wait below uses registers only (as the RISC-V
# Mirlo's boot ROM did):
#   - reset already released at our start (a bitstream loaded over JTAG):
#     nothing to wait for, and no game: serial boot;
#   - else wait (120 s at most) for the release; then the game if slot 0 was
#     loaded, else serial boot. (The Pocket loads ~1 MB/s: 20 s, the first
#     limit, cut off a 34 MB game -- Pokemon Stadium -- and it serial-booted.)
# Then the caches are invalidated (a relaunch leaves lines of the last
# game), the stack goes to SDRAM's top, and boot.c takes over.
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>

    .set    noreorder
    .set    noat
    .text
    .globl  _start
_start:
    b       reset
    nop
    .word   LAYOUT_MARK             # 0xBFC0_0008: the core's memory layout (layout.h) -- the runtime looks for it

    .org    0x200                   # TLB refill (BEV): no TLB -- never
    b       exception
    nop
    .org    0x380                   # general exception (BEV)
exception:
    la      $sp, 0x43FFFFF0
    mfc0    $a0, $13                # Cause
    mfc0    $a1, $14                # EPC
    mfc0    $a2, $8                 # BadVAddr
    la      $t0, boot_exception
    jr      $t0
    nop

reset:
    lui     $t0, 0x3440             # Status: CU1, CU0, FR, BEV; kernel, interrupts off
    mtc0    $t0, $12
    nop
    lui     $s0, 0xF000             # the registers
    move    $a0, $zero              # a0: 1 = boot the game the Pocket loaded
    lw      $t1, CSR_APF_BRIDGE_HOST_STATE_OFS($s0)
    andi    $t1, $t1, 1
    bnez    $t1, caches             # released already: serial boot
    li      $t9, 1
    sw      $t9, CSR_TIMER0_UPTIME_LATCH_OFS($s0)
    lw      $s1, CSR_TIMER0_UPTIME_CYCLES_OFS + 4($s0)     # the start (low word)
    li      $s2, CONFIG_CLOCK_FREQUENCY     # one second (120 of them overflow 32 bits)
    li      $s3, 120                # seconds left
1:  lw      $t1, CSR_APF_BRIDGE_HOST_STATE_OFS($s0)
    andi    $t2, $t1, 1
    bnez    $t2, 2f
    nop
    sw      $t9, CSR_TIMER0_UPTIME_LATCH_OFS($s0)
    lw      $t3, CSR_TIMER0_UPTIME_CYCLES_OFS + 4($s0)
    subu    $t3, $t3, $s1
    sltu    $t3, $t3, $s2
    bnez    $t3, 1b                 # still within this second
    nop
    addiu   $s3, $s3, -1
    bnez    $s3, 1b
    addu    $s1, $s1, $s2           # (delay slot) the next second starts
    b       caches                  # timed out: serial boot
    nop
2:  srl     $a0, $t1, 1
    andi    $a0, $a0, 1

caches:                             # index-invalidate every line: 16 KiB I, 8 KiB D
    lui     $t0, 0x4000
    li      $t1, 16384 / 32
3:  .set mips3
    cache   0x00, 0($t0)
    .set mips2
    addiu   $t1, $t1, -1
    bnez    $t1, 3b
    addiu   $t0, $t0, 32
    lui     $t0, 0x4000
    li      $t1, 8192 / 16
4:  .set mips3
    cache   0x01, 0($t0)
    .set mips2
    addiu   $t1, $t1, -1
    bnez    $t1, 4b
    addiu   $t0, $t0, 16
    la      $sp, 0x43FFFFF0
    la      $t0, boot_main
    jr      $t0
    nop
