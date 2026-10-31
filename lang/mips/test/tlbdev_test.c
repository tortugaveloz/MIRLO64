/* The real TLB with the monitor (mips_core's TLB_EN with MON; sim/mips_soc): what the CPU translates and the monitor answers.
 *
 * A page the game maps onto an address that is not the RDRAM it has -- a register of the RCP, the cartridge, nothing at all (libultra's
 * debug port, mapped at 0xC000_0000 onto 0x8000_0000) -- is the device access of an unmapped address: an address error for the
 * monitor level (MCTL's cause 4, a load, or 5, a store; the game's Cause, EPC, BadVAddr and EntryHi untouched), which finds the
 * physical address the page gave in COP0 register 22. A page onto the RDRAM is the game's and the CPU's own. The monitor here is
 * this program's (exception_entry, MIRLO_VEC_JUMP): it logs each entry and skips the instruction.
 *
 * Checked: loads, stores, ld, sd, ldl and sdl through pages onto the MI, the cartridge's ROM (a 16 MiB page: the offset bits of
 * the address), 0x8000_0000 and the first byte past the RDRAM; the RDRAM's last page and a 4 MiB page onto it do not trap; the
 * game's own exceptions for such a page (invalid: TLBL, clean: Mod) come first; with the 4 MiB setting (MCTL bit 10) the upper
 * half is a device too; and the mapped ranges of the game's view -- 0x5000_0000 - 0x7FFF_FFFF, 0xC000_0000 - 0xEFFF_FFFF,
 * 0x2000_0000 - 0x4FFF_FFFF (MCTL bit 12; not while Status bit 4 is set, as the runtime's helpers run) - translate, and with the
 * bit off those last are the physical addresses. */
#include "tlbtest.h"

/* ---- the monitor: exception_entry saves the registers C may use, calls mon_c, and erets */
static volatile uint32_t e_n, e_code[64], e_pa[64], e_badv[64], e_ehi[64], e_cause[64], e_epc[64];
static volatile uint32_t g_n, g_cause, g_badv, g_epc;                 /* exceptions of the game's own that came this way (Status.ERL clear) */
void mon_c(void)
{
    uint32_t st = MFC0(12);
    if (!(st & 4u)) {                                       /* not the monitor's: the game's exception, passed here by its vector stub */
        g_n++; g_cause = MFC0(13); g_badv = MFC0(8); g_epc = MFC0(14);
        MTC0(14, g_epc + 4u);
        return;
    }
    uint32_t mctl = MFC0(7), code = (mctl >> 3) & 31u;
    if (e_n < 64u) {
        e_code[e_n] = code; e_pa[e_n] = MFC0(22); e_badv[e_n] = MFC0(8); e_ehi[e_n] = MFC0(10); e_cause[e_n] = MFC0(13); e_epc[e_n] = MFC0(30);
    }
    e_n++;
    MTC0(30, MFC0(30) + 4u);                                /* (the access is skipped; none is in a delay slot) */
}
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"
"    sw    $k1, -0x7FFC($zero)\n"
"    lui   $k0, 0xFFFF\n"
"    ori   $k0, $k0, 0x8100\n"
"    sw    $at, 0($k0)\n"
"    sw    $v0, 4($k0)\n"
"    sw    $v1, 8($k0)\n"
"    sw    $a0, 12($k0)\n"
"    sw    $a1, 16($k0)\n"
"    sw    $a2, 20($k0)\n"
"    sw    $a3, 24($k0)\n"
"    sw    $t0, 28($k0)\n"
"    sw    $t1, 32($k0)\n"
"    sw    $t2, 36($k0)\n"
"    sw    $t3, 40($k0)\n"
"    sw    $t4, 44($k0)\n"
"    sw    $t5, 48($k0)\n"
"    sw    $t6, 52($k0)\n"
"    sw    $t7, 56($k0)\n"
"    sw    $t8, 60($k0)\n"
"    sw    $t9, 64($k0)\n"
"    sw    $ra, 68($k0)\n"
"    sw    $sp, 72($k0)\n"
"    sw    $gp, 76($k0)\n"
"    addiu $sp, $k0, 0x3F0\n"
"    jal   mon_c\n"
"    nop\n"
"    lui   $k0, 0xFFFF\n"
"    ori   $k0, $k0, 0x8100\n"
"    lw    $at, 0($k0)\n"
"    lw    $v0, 4($k0)\n"
"    lw    $v1, 8($k0)\n"
"    lw    $a0, 12($k0)\n"
"    lw    $a1, 16($k0)\n"
"    lw    $a2, 20($k0)\n"
"    lw    $a3, 24($k0)\n"
"    lw    $t0, 28($k0)\n"
"    lw    $t1, 32($k0)\n"
"    lw    $t2, 36($k0)\n"
"    lw    $t3, 40($k0)\n"
"    lw    $t4, 44($k0)\n"
"    lw    $t5, 48($k0)\n"
"    lw    $t6, 52($k0)\n"
"    lw    $t7, 56($k0)\n"
"    lw    $t8, 60($k0)\n"
"    lw    $t9, 64($k0)\n"
"    lw    $ra, 68($k0)\n"
"    lw    $sp, 72($k0)\n"
"    lw    $gp, 76($k0)\n"
"    lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* ---- the game's code, in the RDRAM's KSEG0 window, copied to GC_BASE + 0x1000 (position independent). Each is called by jalr from
 * this program (the SDRAM) and returns with jr ra. The access instruction is preceded by an mtc0 of MCTL (a1) and followed by one
 * of MCTL (a2), with nops between: the game's view of 0x2000_0000 - 0x4FFF_FFFF must be on only while no code of this program's own is
 * fetched.
 *   gd_lw  a0 = address, a1, a2 = MCTL values          v0 = lw 0(a0)
 *   gd_sw  a0 = address, a3 = value                     sw a3, 0(a0)
 *   gd_ld  a0 = address                                 v0, v1 = ld 0(a0) (upper, lower), no MCTL change
 *   gd_sd  a0 = address, a2, a3 = value                 sd (a2:a3), 0(a0)
 *   gd_ldl a0 = address                                 v0, v1 = ldl, ldr of the doubleword
 *   gd_sdl a0 = address                                 sdl of a doubleword, then sdr */
extern char gd_start[], gd_end[], gd_lw[], gd_sw[], gd_ld[], gd_sd[], gd_ldl[], gd_sdl[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl gd_start\n"
"gd_start:\n"
"    .globl gd_lw\n"
"gd_lw:\n"
"    mtc0  $a1, $7\n"
"    nop\n    nop\n    nop\n    nop\n"
"    lw    $v0, 0($a0)\n"
"    nop\n"
"    mtc0  $a2, $7\n"
"    nop\n    nop\n    nop\n    nop\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_sw\n"
"gd_sw:\n"
"    sw    $a3, 0($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_ld\n"
"gd_ld:\n"
"    ld    $v0, 0($a0)\n"
"    dsra32 $v0, $v0, 0\n"
"    lw    $v1, 4($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_sd\n"
"gd_sd:\n"
"    dsll32 $t0, $a2, 0\n"
"    dsll32 $t1, $a3, 0\n"
"    dsrl32 $t1, $t1, 0\n"
"    or    $t0, $t0, $t1\n"
"    sd    $t0, 0($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_ldl\n"
"gd_ldl:\n"
"    li    $t0, 0\n"
"    ldl   $t0, 0($a0)\n"
"    ldr   $t0, 7($a0)\n"
"    dsra32 $v0, $t0, 0\n"
"    move  $v1, $t0\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_sdl\n"
"gd_sdl:\n"
"    lui   $t0, 0x1234\n"
"    ori   $t0, $t0, 0x5678\n"
"    dsll32 $t0, $t0, 0\n"
"    lui   $t1, 0x9ABC\n"
"    ori   $t1, $t1, 0xDEF0\n"
"    dsll32 $t1, $t1, 0\n"
"    dsrl32 $t1, $t1, 0\n"
"    or    $t0, $t0, $t1\n"
"    sdl   $t0, 0($a0)\n"
"    sdr   $t0, 7($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gd_end\n"
"gd_end:\n"
);
#define GD_BASE 0x7F4000u
#define GD(sym) ((uint32_t)(0x80000000u | (GD_BASE + ((uint32_t)(uintptr_t)(sym) - (uint32_t)(uintptr_t)gd_start))))
typedef uint32_t (*gd3_t)(uint32_t, uint32_t, uint32_t);
typedef uint32_t (*gd4_t)(uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t gd_vlo;                 /* the lower word of gd_ld's / gd_ldl's result */
static uint32_t glw(uint32_t va, uint32_t m_on, uint32_t m_off) { return ((gd3_t)GD(gd_lw))(va, m_on, m_off); }
static void gsw(uint32_t va, uint32_t v) { ((gd4_t)GD(gd_sw))(va, 0, 0, v); }
static uint32_t gld2(uint32_t sym, uint32_t va)
{
    register uint32_t a0 __asm__("a0") = va, r0 __asm__("v0"), r1 __asm__("v1");
    __asm__ volatile("jalr %3" : "=r"(r0), "=r"(r1), "+r"(a0) : "r"(sym) : "ra", "memory", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "at");
    gd_vlo = r1;
    return r0;
}
static void gsd(uint32_t va, uint32_t hi, uint32_t lo) { ((gd4_t)GD(gd_sd))(va, 0, hi, lo); }

enum { MCTL_GAME = 0x2, MCTL_VIEW_ON = 0x3000, MCTL_VIEW_OFF = 0x2000, MCTL_M4_ON = 0xC00, MCTL_M4_OFF = 0x800 };

static void expect_ev(const char *what, uint32_t n, uint32_t code, uint32_t pa)
{
    check(what, e_n, n + 1);
    if (e_n >= 1 && n < 64u) { check("  code", e_code[n], code); check("  physical address (COP0 22)", e_pa[n], pa); }
}

int main(void)
{
    puts_("tlbdev_test\n");
    if (!has_real_tlb() || !(MFC0(23) & 1u)) { puts_("tlbdev_test: SKIPPED, no real TLB with the monitor in this CPU\n"); putc_(4); for (;;); }
    if ((monitor_id_read() & 0xFFFF0001u) != 0x4D4F0001u) { puts_("tlbdev_test: SKIPPED, no monitor in this SoC\n"); putc_(4); for (;;); }
    tlb_unmap_all();
    install_game_vectors();
    copy_k1(GD_BASE, gd_start, gd_end);
    for (uint32_t i = 0; i < 0x400; i++) { K1(0x10000)[i] = 0x10000000u + i; K1(0x12000)[i] = 0x12000000u + i; }
    K1(0x20000)[0] = 0x20000000u; K1(0x20004)[0] = 0x20000004u;
    K1(0x7FF000)[1] = 0x7FF00001u; K1(0x400000)[0] = 0x40000000u; K1(0x500000)[0] = 0x50000000u;
    flush_cpu_dcache(); flush_cpu_icache();
    MTC0(7, MCTL_GAME | MCTL_M4_OFF);

    /* ---- 1: a pair of 4 KiB pages onto the MI and the VI, loads and stores */
    tlb_wi(5, 0, 0x04600000u | 0x33u, LOC(0x04300000, 2, LV | LD), LOC(0x04400000, 2, LV | LD));
    MTC0(10, 0x33u);                                        /* (EntryHi: a known page and ASID, for the "left alone" checks) */
    uint32_t n = e_n;
    glw(0x04600008u, MCTL_GAME, MCTL_GAME);
    expect_ev("lw through a page onto the MI", n, 4, 0x04300008u);
    check("  the game's EntryHi", e_ehi[n], 0x33u);
    check("  ErrorEPC: the lw", e_epc[n] >> 28, 0x8u);
    check("  no exception of the game's", g_n, 0);
    n = e_n;
    gsw(0x04601010u, 0xABCD0001u);
    expect_ev("sw through the odd page onto the VI", n, 5, 0x04400010u);
    check("  Cause: not the game's to see", e_cause[n] & 0x7C, MFC0(13) & 0x7C);

    /* the registers hold what the game had: BadVAddr is left alone by a monitor entry */
    uint32_t bv = MFC0(8);
    n = e_n;
    glw(0x04600010u, MCTL_GAME, MCTL_GAME);
    check("BadVAddr after a monitor entry", MFC0(8), bv);
    check("EntryHi after a monitor entry", MFC0(10), 0x33u);
    expect_ev("lw again", n, 4, 0x04300010u);

    /* ---- 2: 64-bit accesses, unaligned doubleword ones */
    n = e_n;
    gld2(GD(gd_ld), 0x04600020u);
    check("ld, and the lw behind it: a monitor entry each", e_n, n + 2);
    if (e_n >= n + 2 && n + 1 < 64u) { check("  ld code", e_code[n], 4); check("  ld physical", e_pa[n], 0x04300020u); check("  lw physical", e_pa[n + 1], 0x04300024u); }
    n = e_n;
    gsd(0x04600028u, 0x11112222u, 0x33334444u);
    expect_ev("sd", n, 5, 0x04300028u);
    n = e_n;
    gld2(GD(gd_ldl), 0x04600030u);
    check("ldl, ldr: a monitor entry each", e_n, n + 2);
    if (e_n >= n + 2 && n + 1 < 64u) { check("  ldl code", e_code[n], 4); check("  ldl physical", e_pa[n], 0x04300030u); check("  ldr code", e_code[n + 1], 4); check("  ldr physical", e_pa[n + 1], 0x04300037u); }
    n = e_n;
    gld2(GD(gd_sdl), 0x04600038u);
    check("sdl, sdr: a monitor entry each", e_n, n + 2);
    if (e_n >= n + 2 && n + 1 < 64u) { check("  sdl code", e_code[n], 5); check("  sdl physical", e_pa[n], 0x04300038u); check("  sdr code", e_code[n + 1], 5); check("  sdr physical", e_pa[n + 1], 0x0430003Fu); }

    /* ---- 3: where a page ends: the RDRAM's last page, the first past it, 0x8000_0000, a 16 MiB page onto the cartridge's ROM */
    tlb_wi(6, 0, 0x05000000u, LOC(0x007FE000, 3, LV | LD | LG), LOC(0x007FF000, 3, LV | LD | LG));
    n = e_n;
    check("the RDRAM's last page: the word", glw(0x05001004u, MCTL_GAME, MCTL_GAME), 0x7FF00001u);
    check("  no entry", e_n, n);
    tlb_wi(7, 0, 0x05200000u, LOC(0x007FF000, 3, LV | LD | LG), LOC(0x00800000, 2, LV | LD | LG));
    n = e_n;
    glw(0x05200004u, MCTL_GAME, MCTL_GAME);
    check("the even page, the RDRAM's last: no entry", e_n, n);
    glw(0x05201004u, MCTL_GAME, MCTL_GAME);               /* the odd page: physical 0x0080_0000 */
    expect_ev("the first page past the RDRAM", n, 4, 0x00800004u);
    tlb_wi(8, 0, 0xC0000000u, LOC(0x80000000u, 2, LV | LD), LOC(0x80001000u, 2, LV | LD));
    n = e_n;
    gsw(0xC0000000u, 0x55AA55AAu);
    expect_ev("a store to the debug port's page (0xC000_0000 -> 0x8000_0000)", n, 5, 0x80000000u);
    n = e_n;
    glw(0xC0001FFCu, MCTL_GAME, MCTL_GAME);
    expect_ev("a load from its odd page", n, 4, 0x80001FFCu);
    tlb_wi(9, 0x01FFE000u, 0x08000000u, LOC(0x10000000, 2, LV | LD | LG), LOC(0x11000000, 2, LV | LD | LG));     /* 16 MiB pages */
    n = e_n;
    glw(0x08ABCDE0u, MCTL_GAME, MCTL_GAME);
    expect_ev("a 16 MiB page onto the cartridge's ROM", n, 4, 0x10ABCDE0u);
    n = e_n;
    glw(0x09123458u, MCTL_GAME, MCTL_GAME);
    expect_ev("  its odd page", n, 4, 0x11123458u);
    tlb_wi(10, 0x007FE000u, 0x0A000000u, LOC(0x00400000, 3, LV | LD | LG), LOC(0x00800000, 3, LV | LD | LG));    /* 4 MiB pages: the first in the RDRAM */
    n = e_n;
    check("a 4 MiB page onto 0x0040_0000: the word", glw(0x0A000000u, MCTL_GAME, MCTL_GAME), 0x40000000u);
    check("  the offset bits of the address (+1 MiB)", glw(0x0A100000u, MCTL_GAME, MCTL_GAME), 0x50000000u);
    check("  no entry", e_n, n);
    glw(0x0A400000u, MCTL_GAME, MCTL_GAME);
    expect_ev("  its odd page, past the RDRAM", n, 4, 0x00800000u);
    check("  the game saw no exception", g_n, 0);

    /* ---- 4: the game's own exceptions come first: an invalid page, a clean one */
    tlb_wi(11, 0, 0x04800000u, LOC(0x04300000, 2, LV), LOC(0x04300000, 2, 0));      /* even: clean; odd: invalid */
    n = e_n;
    glw(0x04801000u, MCTL_GAME, MCTL_GAME);
    check("an invalid page onto a device: the game's TLBL, not the monitor's", e_n, n);
    check("  the game's handler ran (its stub ends here)", g_n, 1);
    check("  TLBL", (g_cause >> 2) & 31, 2);
    gsw(0x04800000u, 1);
    check("a store to a clean page onto a device: Mod", (g_cause >> 2) & 31, 1);
    check("  the monitor did not", e_n, n);
    glw(0x04800004u, MCTL_GAME, MCTL_GAME);
    expect_ev("a load from the valid page", n, 4, 0x04300004u);

    /* ---- 5: the 4 MiB setting: the upper half of the RDRAM is a device */
    tlb_wi(12, 0, 0x04900000u, LOC(0x00400000, 3, LV | LD | LG), LOC(0x007FF000, 3, LV | LD | LG));
    n = e_n;
    glw(0x04900000u, MCTL_GAME | MCTL_M4_ON, MCTL_GAME | MCTL_M4_ON);
    expect_ev("MCTL bit 10: a page onto 0x0040_0000 is not the console's", n, 4, 0x00400000u);
    n = e_n;
    glw(0x04901000u, MCTL_GAME | MCTL_M4_ON, MCTL_GAME | MCTL_M4_OFF);
    expect_ev("  and the last page of the RDRAM", n, 4, 0x007FF000u);
    n = e_n;
    check("with the setting off, the word is there", glw(0x04900000u, MCTL_GAME | MCTL_M4_OFF, MCTL_GAME | MCTL_M4_OFF), 0x40000000u);
    check("  no entry", e_n, n);

    /* ---- 6: the ranges the game's view maps (the RDRAM through each) */
    {
        static const uint32_t va[] = { 0x1FFFE000u, 0x50000000u, 0x5FFFE000u, 0x60000000u, 0x7FFFE000u, 0xC0010000u, 0xDFFFE000u, 0xE0000000u, 0xEFFFE000u };
        for (unsigned i = 0; i < sizeof va / sizeof va[0]; i++) {
            tlb_wi(13 + i, 0, va[i], LOC(0x00010000, 3, LV | LD | LG), LOC(0x00012000, 3, LV | LD | LG));
            n = e_n;
            check("a mapped range: the even page", glw(va[i] + 0x10, MCTL_GAME, MCTL_GAME), 0x10000004u);
            check("  the odd page", glw(va[i] + 0x1010, MCTL_GAME, MCTL_GAME), 0x12000004u);
            gsw(va[i] + 0x20, 0x600D0000u + i);
            check("  a store", *K1(0x10020), 0x600D0000u + i);
            check("  no entry of the monitor's", e_n, n);
            tlb_wi(13 + i, 0, 0x80000000u + ((13 + i) << 13), 0, 0);
        }
    }
    /* the game's view of 0x2000_0000 - 0x4FFF_FFFF */
    tlb_wi(22, 0, 0x40000000u, LOC(0x00010000, 3, LV | LD | LG), LOC(0x00012000, 3, LV | LD | LG));
    tlb_wi(23, 0, 0x20000000u, LOC(0x00010000, 3, LV | LD | LG), LOC(0x00012000, 3, LV | LD | LG));
    tlb_wi(24, 0, 0x4FFFE000u, LOC(0x00010000, 3, LV | LD | LG), LOC(0x00012000, 3, LV | LD | LG));
    n = e_n;
    check("view on: 0x4000_0000 is a mapped page", glw(0x40000010u, MCTL_GAME | MCTL_VIEW_ON, MCTL_GAME | MCTL_VIEW_OFF), 0x10000004u);
    check("  0x2000_0000", glw(0x20001010u, MCTL_GAME | MCTL_VIEW_ON, MCTL_GAME | MCTL_VIEW_OFF), 0x12000004u);
    check("  0x4FFF_FFFF's page", glw(0x4FFFF010u, MCTL_GAME | MCTL_VIEW_ON, MCTL_GAME | MCTL_VIEW_OFF), 0x12000004u);
    check("  no entry", e_n, n);
    n = e_n;
    glw(0x40000000u, MCTL_GAME | MCTL_VIEW_OFF, MCTL_GAME | MCTL_VIEW_OFF);
    check("view off: 0x4000_0000 is the SDRAM, not the game's: a device access for the monitor", e_n, n + 1);
    check("  code", e_code[n], 4);
    check("  the view stays off after it", MFC0(7) & 0x1000u, 0);

    done("tlbdev_test");
    return 0;
}
