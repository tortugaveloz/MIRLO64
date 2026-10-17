/* A TLB exception delivered to the game (mips_core's TLB_RAM with MON; sim/mips_soc): a page that no entry maps -- or one
 * that is invalid, or a store to one that is clean -- is not fatal: the monitor level does what the VR4300 does for an
 * exception it takes itself and the game's own handler runs, as GoldenEye's does for the pages it maps on demand
 * (0x7F000000): BadVAddr, EntryHi (VPN2 and ASID), Context's BadVPN2, Cause.ExcCode (TLBL 2, TLBS 3, Mod 1) and BD, EPC (the
 * branch's, in a delay slot; not touched when EXL was set), Status.EXL, then the vector -- 0x8000_0000 for a refill with EXL
 * clear, 0x8000_0180 for the rest. The handler does mfc0 / mtc0 / tlbwr / eret, which the monitor already handles, and the
 * access runs again.
 *
 * The monitor is this program's own, the algorithm of lang/n64rt/n64rt_os.c (tlb_service, tlb_deliver). The "game" is
 * main(), with MCTL.GAME set; its handlers are code in the RDRAM, as the game's: a refill handler that reads its page pairs
 * from a table by Context (PTEBase 0x8000_0000: 16 bytes a pair, EntryLo0, EntryLo1), and a general one that logs what it
 * is given and, in mode 0, repairs the entry (tlbp, tlbwi) from two words the test gives it.
 *
 * Checked: refill of a load, of a store and of a fetch, with every register the handler sees; the odd page of a pair the
 * handler wrote (no second exception); a refill in a delay slot (EPC the branch, BD); the ASID (an entry of another one
 * does not map the page: a refill); an invalid page (the general vector, TLBL); a store to a clean page (Mod); a refill with
 * EXL set (the general vector, EPC and BD left alone); a pair replaced by the handler's tlbwr (no stale page). */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}

#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)))
#define K0(pa) ((volatile uint32_t *)(0x80000000u | (pa)))
#define K1(pa) ((volatile uint32_t *)(0xA0000000u | (pa)))
#define TLBWI() __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop")

/* ---- the monitor ---------------------------------------------------------------------- */
static struct { uint32_t mask, hi, lo0, lo1; } raw[32];     /* hi as written; a free entry's 0x80000000 */
static volatile uint32_t m_tlbops, m_fills, m_delivered, m_other, m_none;

static void tr_write(uint32_t va, uint32_t pa, uint32_t flags)
{
    MTC0(23, (pa & 0x1FFFF000u) | ((va >> 8) & 16u) | flags);
}
static void tr_flush(void)
{
    uint32_t hi = MFC0(10);
    for (uint32_t va = 0; va < 512u << 12; va += 0x1000u) {
        if (!(va & 0x1000u)) MTC0(10, va);
        tr_write(va, 0, 0); tr_write(va, 0, 8);
    }
    MTC0(10, hi);
}
static void tr_inval(uint32_t va, uint32_t bytes)
{
    if (bytes > 0x40000u) { tr_flush(); return; }
    uint32_t hi = MFC0(10);
    for (uint32_t a = va & ~0x1FFFu; a < va + bytes; a += 0x2000u) {
        MTC0(10, a);
        tr_write(a, 0, 0); tr_write(a, 0, 8); tr_write(a + 0x1000u, 0, 0); tr_write(a + 0x1000u, 0, 8);
    }
    MTC0(10, hi);
}
static int entry_used(int i) { return raw[i].hi != 0x80000000u && ((raw[i].lo0 | raw[i].lo1) & 2u); }
static void do_write(uint32_t i)
{
    uint32_t mask = MFC0(5) & 0x01FFE000u;
    int was = entry_used((int)i);
    uint32_t ova = raw[i].hi & 0xFFFFE000u & ~(raw[i].mask | 0x1FFFu), ospan = ((raw[i].mask + 0x2000u) >> 1) * 2u;
    raw[i].mask = mask;
    raw[i].hi = MFC0(10) & ~(mask | 0x1F00u);
    raw[i].lo0 = MFC0(2) & 0x3FFFFFFFu;
    raw[i].lo1 = MFC0(3) & 0x3FFFFFFFu;
    if (was) tr_inval(ova, ospan);
}
enum { X_FILLED, X_REFILL, X_INVALID, X_MOD, X_NONE };
static int service(uint32_t code)
{
    uint32_t badv = MFC0(22), sel = MFC0(23), hi = MFC0(10);      /* (COP0 22: the fault's address; BadVAddr and EntryHi are the game's) */
    for (int i = 0; i < 32; i++) {
        uint32_t ehi = raw[i].hi, vpn = ~(raw[i].mask | 0x1FFFu), g = raw[i].lo0 & raw[i].lo1 & 1u;
        if (ehi == 0x80000000u || (badv & vpn) != (ehi & vpn) || (!g && (hi & 0xFF) != (ehi & 0xFF))) continue;
        uint32_t span = (raw[i].mask + 0x2000u) >> 1, off = badv - (ehi & 0xFFFFE000u & ~(2u * span - 1u));
        uint32_t lo = off >= span ? raw[i].lo1 : raw[i].lo0;
        if (!(lo & 2u)) return X_INVALID;
        if (code == 1 && !(lo & 4u)) return X_MOD;
        uint32_t pa = (((lo >> 6) << 12) & ~(span - 1u)) + (off & (span - 1u) & ~0xFFFu);
        if (pa >= 0x00800000u) return X_NONE;
        MTC0(10, (badv & 0xFFFFE000u) | (hi & 0xFFu));
        tr_write(badv, pa, 1u | ((lo & 4u) ? 2u : 0u) | (g ? 4u : 0u) | (((lo >> 3) & 7u) != 2u ? 32u : 0u) | (sel & 8u));   /* (bit 5: cacheable, as n64rt does) */
        MTC0(10, hi);
        return X_FILLED;
    }
    return X_REFILL;
}
static void deliver(int res, uint32_t code, uint32_t bd)
{
    uint32_t badv = MFC0(22), cause = MFC0(13), ctxt = MFC0(4), epc = MFC0(30), status = MFC0(12);
    MTC0(8, badv);
    MTC0(10, (badv & 0xFFFFE000u) | (MFC0(10) & 0xFFu));
    uint32_t exl = status & 2u, exc = res == X_MOD ? 1u : code == 3u ? 3u : 2u;
    cause = (cause & ~0x7Cu) | exc << 2;
    if (!exl) { cause = (cause & 0x7FFFFFFFu) | bd << 31; MTC0(14, epc); }
    MTC0(13, cause);
    MTC0(4, (ctxt & 0xFF800000u) | ((badv >> 13) << 4));
    MTC0(12, status | 2u);
    MTC0(30, (res == X_REFILL && !exl) ? 0x80000000u : 0x80000180u);
}
/* the instruction at pc: as it is, or a mapped page's (below 0x2000_0000, 0x5000_0000 - 0x7FFF_FFFF or 0xC000_0000 - 0xEFFF_FFFF)
 * by the entries -- the game's handler may run from one */
static uint32_t insn_at(uint32_t pc)
{
    const uint32_t n = pc >> 28;
    if (!(n < 2u || (n >= 5u && n <= 7u) || (n >= 12u && n <= 14u))) return *(volatile uint32_t *)(uintptr_t)pc;
    for (int i = 0; i < 32; i++) {
        uint32_t span = (raw[i].mask + 0x2000u) >> 1, off = pc - (raw[i].hi & 0xFFFFE000u & ~(2u * span - 1u));
        if (raw[i].hi == 0x80000000u || off >= 2u * span) continue;
        uint32_t lo = off >= span ? raw[i].lo1 : raw[i].lo0;
        if (lo & 2u) return *K1(((((lo >> 6) << 12) & ~(span - 1u)) + (off & (span - 1u))) & 0x7FFFFCu);
    }
    return 0;
}
void mon_c(void)
{
    uint32_t mctl = MFC0(7), code = (mctl >> 3) & 31u, bd = (mctl >> 2) & 1u, epc = MFC0(30);
    if (code == 10) {
        uint32_t pc = epc + (bd ? 4u : 0u), insn = insn_at(pc), next = pc + 4u;
        if ((insn & 0xFFFFFFC0u) == 0x42000000u) {
            uint32_t v, k = insn & 63u;
            m_tlbops++;
            if (k == 0x02) { v = MFC0(0) & 63u; if (v < 32u) do_write(v); }
            else if (k == 0x06) { v = MFC0(1); do_write(v & 31u); }
            else if (k == 0x08) {
                uint32_t hi = MFC0(10), found = 0x80000000u;
                for (uint32_t i = 0; i < 32; i++) {
                    uint32_t vpn = ~(raw[i].mask | 0x1FFFu);
                    if ((hi & vpn) == (raw[i].hi & vpn) && ((raw[i].lo0 & raw[i].lo1 & 1u) || (hi & 0xFF) == (raw[i].hi & 0xFF))) { found = i; break; }
                }
                MTC0(0, found);
            } else { m_other++; }
            MTC0(30, next);
            return;
        }
    }
    if (code >= 1 && code <= 3) {
        int res = service(code);
        if (res == X_FILLED) { m_fills++; return; }
        if (res != X_NONE) { m_delivered++; deliver(res, code, bd); return; }
        m_none++;
        MTC0(30, MFC0(30) + 4u);
        return;
    }
    m_other++;
    MTC0(30, MFC0(30) + 4u);
}
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"                   /* the game's $k0, $k1: the monitor's RAM (0xFFFF8000) is all this has */
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

/* ---- the game's handlers (copied to the RDRAM at 0x8000_0000 and 0x8000_0180) --------------- */
/* the log, at 0x8001_0000: +0 refills, +4.. Cause, BadVAddr, EPC, Context, EntryHi, Status of the last;
 * +0x40 general exceptions, +0x44.. the same; +0x5C mode (0: repair, 1: just return), +0x60 / +0x64 the repair's EntryLo0 / 1 */
extern char h_refill[], h_refill_end[], h_gen[], h_gen_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl h_refill\n"
"h_refill:\n"
"    mfc0  $k0, $4\n"
"    lw    $k1, 0($k0)\n"
"    lw    $k0, 4($k0)\n"
"    mtc0  $k1, $2\n"
"    mtc0  $k0, $3\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0($k0)\n"
"    mfc0  $k1, $13\n    sw    $k1, 4($k0)\n"
"    mfc0  $k1, $8\n     sw    $k1, 8($k0)\n"
"    mfc0  $k1, $14\n    sw    $k1, 12($k0)\n"
"    mfc0  $k1, $4\n     sw    $k1, 16($k0)\n"
"    mfc0  $k1, $10\n    sw    $k1, 20($k0)\n"
"    mfc0  $k1, $12\n    sw    $k1, 24($k0)\n"
"    nop\n"
"    nop\n"
"    tlbwr\n"
"    nop\n"
"    nop\n"
"    eret\n"
"    nop\n"
"    .globl h_refill_end\n"
"h_refill_end:\n"
"    .globl h_gen\n"
"h_gen:\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0x40($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0x40($k0)\n"
"    mfc0  $k1, $13\n    sw    $k1, 0x44($k0)\n"
"    mfc0  $k1, $8\n     sw    $k1, 0x48($k0)\n"
"    mfc0  $k1, $14\n    sw    $k1, 0x4C($k0)\n"
"    mfc0  $k1, $4\n     sw    $k1, 0x50($k0)\n"
"    mfc0  $k1, $10\n    sw    $k1, 0x54($k0)\n"
"    mfc0  $k1, $12\n    sw    $k1, 0x58($k0)\n"
"    lw    $k1, 0x5C($k0)\n"
"    bnez  $k1, 9f\n"
"    nop\n"
"    lw    $k1, 0x60($k0)\n"
"    mtc0  $k1, $2\n"
"    lw    $k1, 0x64($k0)\n"
"    mtc0  $k1, $3\n"
"    nop\n"
"    nop\n"
"    tlbp\n"
"    nop\n"
"    nop\n"
"    tlbwi\n"
"    nop\n"
"    nop\n"
"9:  eret\n"
"    nop\n"
"    .globl h_gen_end\n"
"h_gen_end:\n"
);

/* ---- the game ------------------------------------------------------------------------- */
#define LO(pa, f) ((((uint32_t)(pa) >> 12) << 6) | 0x18u | (f))
enum { LV = 2, LD = 4, LG = 1 };
#define LOG(o) (*K0(0x10000 + (o)))
/* the game's page table: the pair of VA's EntryLo words, where Context (PTEBase 0x8000_0000) points */
static void pte(uint32_t va, uint32_t lo0, uint32_t lo1)
{
    volatile uint32_t *e = K0((va >> 13) << 4);
    e[0] = lo0; e[1] = lo1;
}
static void copy(uint32_t dst, const char *a, const char *b)
{
    for (const uint32_t *p = (const uint32_t *)a; p < (const uint32_t *)b; p++, dst += 4) *K1(dst) = *p;
}
static void clear_log(void) { for (uint32_t o = 0; o < 0x80; o += 4) LOG(o) = 0; LOG(0x60) = 0; }

int main(void)
{
    puts_("tlbgame_test\n");
    if ((MFC0(23) & ~8u) != 0x54420001u) { puts_("tlbgame_test: no tables with the monitor in this CPU\ntlbgame_test: FAILED\n"); putc_(4); for (;;); }
    if ((monitor_id_read() & 0xFFFF0001u) != 0x4D4F0001u) { puts_("no monitor in this SoC\ntlbgame_test: FAILED\n"); putc_(4); for (;;); }
    for (int i = 0; i < 32; i++) raw[i].hi = 0x80000000u;
    for (uint32_t i = 0; i < 0x1000; i += 4) K1(0x20000)[i / 4] = 0;
    /* data pages: physical 0x100000 + n * 0x1000 hold n in every word's top, and their offset */
    for (uint32_t n = 0; n < 32; n++) for (uint32_t w = 0; w < 1024; w += 64) K1(0x100000 + n * 0x1000)[w] = (n << 24) | w;
    /* code in a page: addiu v0, zero, 0x1234 / jr ra / nop */
    K1(0x120000)[0] = 0x24021234u; K1(0x120000)[1] = 0x03E00008u; K1(0x120000)[2] = 0;
    copy(0x0, h_refill, h_refill_end);
    copy(0x180, h_gen, h_gen_end);
    flush_cpu_dcache(); flush_cpu_icache();
    tr_flush();
    clear_log();
    MTC0(4, 0x80000000u);                         /* Context: PTEBase */
    monitor_pend_write(0xF); monitor_mask_write(0u); monitor_ctl_write(1u);
    MTC0(7, 2);                                   /* MCTL.GAME */
    volatile uint32_t *m = (volatile uint32_t *)0x04000000u;
    uint32_t pre;

    /* 1. a load from a page no entry maps: the refill vector, every register as the VR4300 leaves it */
    pte(0x04000000u, LO(0x100000, LV | LD | LG), LO(0x101000, LV | LD | LG));
    uint32_t v1;
    {   /* (the load's address, for EPC) */
        extern char l1[];
        __asm__ volatile(".set push\n.set noreorder\n.globl l1\nl1: lw %0, 0(%1)\n nop\n.set pop" : "=r"(v1) : "r"(m));
        check("refill: the load's value", v1, 0x00000000u);
        check("refill: handler ran once", LOG(0), 1);
        check("refill: Cause.ExcCode TLBL", (LOG(4) >> 2) & 31u, 2);
        check("refill: Cause.BD clear", LOG(4) >> 31, 0);
        check("refill: BadVAddr", LOG(8), 0x04000000u);
        check("refill: EPC the load", LOG(12), (uint32_t)(uintptr_t)l1);
        check("refill: Context BadVPN2", LOG(16), 0x80000000u | (0x2000u << 4));
        check("refill: EntryHi VPN2", LOG(20) & 0xFFFFE000u, 0x04000000u);
        check("refill: Status.EXL, not ERL", LOG(24) & 6u, 2);
        check("refill: not a general exception", LOG(0x40), 0);
        check("after the eret: EXL clear", MFC0(12) & 6u, 0);
    }
    check("refill: even page, second word", m[64], 0x00000040u);
    check("refill: odd page", m[0x400], 0x01000000u);
    check("odd page: no second exception", LOG(0), 1);
    check("fills: one a page", m_fills, 2);

    /* 2. a store to a new pair: TLBS */
    pte(0x04010000u, LO(0x102000, LV | LD | LG), LO(0x103000, LV | LD | LG));
    *(volatile uint32_t *)0x04010004u = 0xC0FFEE01u;
    check("store refill: handler ran", LOG(0), 2);
    check("store refill: Cause.ExcCode TLBS", (LOG(4) >> 2) & 31u, 3);
    check("store refill: BadVAddr", LOG(8), 0x04010004u);
    check("store refill: the store landed", *K1(0x102004), 0xC0FFEE01u);

    /* 3. a refill in a branch's delay slot: EPC the branch, BD, and the branch is taken after the slot ran again */
    pte(0x04020000u, LO(0x104000, LV | LD | LG), LO(0x105000, LV | LD | LG));
    {
        uint32_t x = 0, got = 0;
        extern char l3[];
        __asm__ volatile(".set push\n.set noreorder\n.set mips3\n"
                         ".globl l3\n"
                         "l3: b 1f\n"
                         "    lw %1, 0(%2)\n"
                         "    addiu %0, %0, 100\n"
                         "1:  addiu %0, %0, 1\n"
                         ".set pop" : "+r"(x), "=r"(got) : "r"(0x04020000u));
        check("delay slot: the branch taken", x, 1);
        check("delay slot: the value", got, 0x04000000u);
        check("delay slot: handler ran", LOG(0), 3);
        check("delay slot: Cause.BD", LOG(4) >> 31, 1);
        check("delay slot: EPC the branch", LOG(12), (uint32_t)(uintptr_t)l3);
        check("delay slot: BadVAddr", LOG(8), 0x04020000u);
    }

    /* 4. a fetch: a function in a page no entry maps */
    pte(0x04100000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
    {
        uint32_t r = ((uint32_t (*)(void))0x04100000u)();
        check("fetch refill: the function ran", r, 0x1234);
        check("fetch refill: handler ran", LOG(0), 4);
        check("fetch refill: Cause.ExcCode TLBL", (LOG(4) >> 2) & 31u, 2);
        check("fetch refill: BadVAddr the pc", LOG(8), 0x04100000u);
        check("fetch refill: EPC the pc", LOG(12), 0x04100000u);
    }

    /* 5. ASID: a non-global entry of ASID 7 maps the page for ASID 7; ASID 8 misses and refills (a second entry) */
    pte(0x04200000u, LO(0x108000, LV | LD), LO(0x109000, LV | LD));
    MTC0(10, 7);
    check("asid 7: refill, the page", *(volatile uint32_t *)0x04200000u, 0x08000000u);
    pre = LOG(0);
    check("asid 7: EntryHi ASID in the handler", LOG(20) & 0xFF, 7);
    check("asid 7: again, no exception", *(volatile uint32_t *)0x04200000u, 0x08000000u);
    check("asid 7: no new refill", LOG(0), pre);
    MTC0(10, 8);
    check("asid 8: the page, after a refill", *(volatile uint32_t *)0x04200000u, 0x08000000u);
    check("asid 8: a refill", LOG(0), pre + 1);
    check("asid 8: EntryHi ASID in the handler", LOG(20) & 0xFF, 8);
    MTC0(10, 0);

    /* 6. an invalid page: the entry names it, V clear -- the general vector, TLBL; the handler repairs it (tlbp, tlbwi) */
    pte(0x04300000u, LO(0x10A000, LD | LG), LO(0x10B000, LV | LD | LG));      /* the even page invalid */
    LOG(0x60) = LO(0x10A000, LV | LD | LG); LOG(0x64) = LO(0x10B000, LV | LD | LG);
    pre = LOG(0);
    check("invalid: after the repair the page", *(volatile uint32_t *)0x04300000u, 0x0A000000u);
    check("invalid: a refill first", LOG(0), pre + 1);
    check("invalid: then the general vector", LOG(0x40), 1);
    check("invalid: Cause.ExcCode TLBL", (LOG(0x44) >> 2) & 31u, 2);
    check("invalid: BadVAddr", LOG(0x48), 0x04300000u);
    check("invalid: the odd page was valid", *(volatile uint32_t *)0x04301000u, 0x0B000000u);

    /* 7. Mod: a store to a clean page (D clear); the load first is fine */
    pte(0x04400000u, LO(0x10C000, LV | LG), LO(0x10D000, LV | LG));
    LOG(0x60) = LO(0x10C000, LV | LD | LG); LOG(0x64) = LO(0x10D000, LV | LD | LG);
    check("clean page: a load", *(volatile uint32_t *)0x04400000u, 0x0C000000u);
    uint32_t g0 = LOG(0x40);
    *(volatile uint32_t *)0x04400008u = 0xFEED0002u;
    check("clean page: Mod, the general vector", LOG(0x40), g0 + 1);
    check("clean page: Cause.ExcCode Mod", (LOG(0x44) >> 2) & 31u, 1);
    check("clean page: BadVAddr", LOG(0x48), 0x04400008u);
    check("clean page: the store landed after the repair", *K1(0x10C008), 0xFEED0002u);
    *(volatile uint32_t *)0x0440000Cu = 0xFEED0003u;
    check("clean page: no second Mod", LOG(0x40), g0 + 1);

    /* 8. a refill with EXL set: the general vector, EPC and BD as they were (mode 1: the handler returns to EPC) */
    {
        extern char l8[];
        uint32_t x = 0;
        pte(0x04500000u, LO(0x10E000, LV | LD | LG), LO(0x10F000, LV | LD | LG));
        LOG(0x5C) = 1;
        g0 = LOG(0x40); pre = LOG(0);
        MTC0(14, (uint32_t)(uintptr_t)l8);
        MTC0(13, 0x00000000u);
        __asm__ volatile(".set push\n.set noreorder\n"
                         "mfc0 $t0, $12\n"
                         "ori  $t0, $t0, 2\n"
                         "mtc0 $t0, $12\n"             /* EXL */
                         "lw   $t1, 0(%1)\n"           /* the refill: EXL set -> vector 0x180, EPC untouched */
                         "addiu %0, %0, 100\n"         /* (skipped: the eret goes to EPC = l8) */
                         ".globl l8\n"
                         "l8: addiu %0, %0, 1\n"
                         ".set pop" : "+r"(x) : "r"(0x04500000u) : "t0", "t1");
        check("EXL: went to EPC", x, 1);
        check("EXL: the general vector", LOG(0x40), g0 + 1);
        check("EXL: no refill handler", LOG(0), pre);
        check("EXL: Cause.ExcCode TLBL", (LOG(0x44) >> 2) & 31u, 2);
        check("EXL: EPC as it was", LOG(0x4C), (uint32_t)(uintptr_t)l8);
        check("EXL: BadVAddr", LOG(0x48), 0x04500000u);
        LOG(0x5C) = 0;
    }

    /* 9. a pair replaced by the handler's tlbwr: the old pages are gone (Wired 31: tlbwr writes entry 31) */
    MTC0(6, 31);
    pte(0x04600000u, LO(0x110000, LV | LD | LG), LO(0x111000, LV | LD | LG));
    check("tlbwr 1", *(volatile uint32_t *)0x04600000u, 0x10000000u);
    check("tlbwr 1 odd", *(volatile uint32_t *)0x04601000u, 0x11000000u);
    pte(0x04700000u, LO(0x112000, LV | LD | LG), LO(0x113000, LV | LD | LG));
    check("tlbwr 2", *(volatile uint32_t *)0x04700000u, 0x12000000u);
    pre = LOG(0);
    check("tlbwr: the first pair's pages are gone, it refills", *(volatile uint32_t *)0x04600000u, 0x10000000u);
    check("tlbwr: refilled", LOG(0), pre + 1);
    MTC0(6, 0);

    /* 10. 0x6000_0000 - 0x7FFF_FFFF are mapped too (the core with MAP_HI: GoldenEye maps and runs from 0x7000_0000 and takes
     * 0x7F000000 from its refill handler): a load and a fetch, refill and all */
    pte(0x70000000u, LO(0x11C000, LV | LD | LG), LO(0x11D000, LV | LD | LG));
    pre = LOG(0);
    check("0x7000_0000: a load", *(volatile uint32_t *)0x70000000u, 0x1C000000u);
    check("0x7000_0000: after a refill", LOG(0), pre + 1);
    check("0x7000_0000: BadVAddr", LOG(8), 0x70000000u);
    check("0x7000_0000: the odd page", *(volatile uint32_t *)0x70001000u, 0x1D000000u);
    pte(0x7F000000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
    {
        uint32_t r = ((uint32_t (*)(void))0x7F000000u)();
        check("0x7F00_0000: a function fetched there ran", r, 0x1234);
        check("0x7F00_0000: the refill's BadVAddr", LOG(8), 0x7F000000u);
    }
    /* (and 0x5000_0000 - 0x5FFF_FFFF: a game may map its code there too) */
    pte(0x50000000u, LO(0x11E000, LV | LD | LG), LO(0x120000, LV | LD | LG));
    pre = LOG(0);
    check("0x5000_0000: a load", *(volatile uint32_t *)0x50000000u, 0x1E000000u);
    check("0x5000_0000: after a refill", LOG(0), pre + 1);
    check("0x5000_0000: BadVAddr", LOG(8), 0x50000000u);
    {
        uint32_t r = ((uint32_t (*)(void))0x50001000u)();
        check("0x5000_1000: a function fetched there ran", r, 0x1234);
    }
    /* (and KSEG2 and KSEG3 below 0xF000_0000, mapped as the VR4300 maps them: a game may map the RDRAM at 0xE000_0000 and write
     * and read its data there a byte at a time) */
    pte(0xE0000000u, LO(0x11F000, LV | LD | LG), LO(0x120000, LV | LD | LG));
    pre = LOG(0);
    check("0xE000_0000: a load", *(volatile uint32_t *)0xE0000000u, 0x1F000000u);
    check("0xE000_0000: after a refill", LOG(0), pre + 1);
    check("0xE000_0000: BadVAddr", LOG(8), 0xE0000000u);
    check("0xE000_0000: Context BadVPN2", LOG(16), 0x80000000u | ((0xE0000000u >> 13) << 4));
    *(volatile uint8_t *)0xE0000005u = 0x5A;
    check("0xE000_0005: a byte stored", (*K1(0x11F004) >> 16) & 0xFFu, 0x5A);
    check("0xE000_0005: and loaded", *(volatile uint8_t *)0xE0000005u, 0x5A);
    check("0xE000_0000: no other refill", LOG(0), pre + 1);
    {
        uint32_t r = ((uint32_t (*)(void))0xE0001000u)();
        check("0xE000_1000: a function fetched there ran", r, 0x1234);
    }
    pte(0xC0000000u, LO(0x11E000, LV | LD | LG), LO(0x11F000, LV | LD | LG));
    pre = LOG(0);
    check("0xC000_0000: a load", *(volatile uint32_t *)0xC0000000u, 0x1E000000u);
    check("0xC000_0000: after a refill", LOG(0), pre + 1);
    check("0xC000_0000: BadVAddr", LOG(8), 0xC0000000u);
    pte(0xDFFFE000u, LO(0x11C000, LV | LD | LG), LO(0x11D000, LV | LD | LG));
    check("0xDFFF_F000: a load", *(volatile uint32_t *)0xDFFFF000u, 0x1D000000u);

    /* 11. code from cacheable mapped pages (the fetch page registers), with the game's own refill handler: a function run
     * twice, then its pair replaced by a tlbwi (the new frame's code, none of the old), an ASID switch (the other ASID's entry
     * or a refill, never the first one's page), an entry made invalid and repaired by the general handler, and the speed */
    {
        typedef uint32_t (*fn_t)(void);
        const uint32_t addiu = 0x24020000u;
        K1(0x122000)[0] = addiu | 0x2222; K1(0x122000)[1] = 0x03E00008u; K1(0x122000)[2] = 0;
        K1(0x123000)[0] = addiu | 0x3333; K1(0x123000)[1] = 0x03E00008u; K1(0x123000)[2] = 0;
        pte(0x04800000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        pre = LOG(0);
        check("code 1", ((fn_t)0x04800000u)(), 0x1234);
        check("code 1: a refill", LOG(0), pre + 1);
        check("code 1 again", ((fn_t)0x04800000u)(), 0x1234);
        check("code 1 again: no refill", LOG(0), pre + 1);
        /* the game replaces the entry by a tlbwi (tlbp finds its index; nops keep the tlbwi out of a call's delay slot) */
        MTC0(10, 0x04800000u); __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop\nnop"); uint32_t ix = MFC0(0);
        check("code: the entry is there", ix >> 31, 0);
        MTC0(0, ix); MTC0(5, 0); MTC0(10, 0x04800000u); MTC0(2, LO(0x122000, LV | LD | LG)); MTC0(3, LO(0x123000, LV | LD | LG));
        TLBWI(); __asm__ volatile("nop");
        check("code, replaced by a tlbwi", ((fn_t)0x04800000u)(), 0x2222);
        MTC0(0, ix); MTC0(5, 0); MTC0(10, 0x04800000u); MTC0(2, LO(0x120000, LV | LD | LG)); MTC0(3, LO(0x121000, LV | LD | LG));
        TLBWI(); __asm__ volatile("nop");
        check("code, replaced back", ((fn_t)0x04800000u)(), 0x1234);
        check("code, replaced: no game exception", LOG(0), pre + 1);
        /* ASIDs: two non-global entries for one page pair */
        MTC0(0, 21); MTC0(5, 0); MTC0(10, 0x04900000u | 3u); MTC0(2, LO(0x120000, LV | LD)); MTC0(3, LO(0x121000, LV | LD)); TLBWI(); __asm__ volatile("nop");
        MTC0(0, 22); MTC0(5, 0); MTC0(10, 0x04900000u | 4u); MTC0(2, LO(0x122000, LV | LD)); MTC0(3, LO(0x123000, LV | LD)); TLBWI(); __asm__ volatile("nop");
        MTC0(10, 3); check("ASID 3's code", ((fn_t)0x04900000u)(), 0x1234);
        MTC0(10, 4); check("ASID 4's code, not ASID 3's", ((fn_t)0x04900000u)(), 0x2222);
        MTC0(10, 3); check("ASID 3's code again", ((fn_t)0x04900000u)(), 0x1234);
        MTC0(10, 0);
        /* pages that differ only in the address's top bits (0x6 and 0xE, 0x1 and 0x5, 0x0 and 0xC): each its own frame's code,
         * in turn, never what the fetch page registers hold for the other */
        pte(0x64800000u, LO(0x122000, LV | LD | LG), LO(0x123000, LV | LD | LG));
        pte(0xE4800000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        pte(0x14800000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        pte(0x54800000u, LO(0x122000, LV | LD | LG), LO(0x123000, LV | LD | LG));
        pte(0xC4800000u, LO(0x122000, LV | LD | LG), LO(0x123000, LV | LD | LG));
        for (int k = 0; k < 2; k++) {
            check("0x6480_0000's code", ((fn_t)0x64800000u)(), 0x2222);
            check("0xE480_0000's code, not 0x6480_0000's", ((fn_t)0xE4800000u)(), 0x1234);
            check("0x1480_0000's code", ((fn_t)0x14800000u)(), 0x1234);
            check("0x5480_0000's code, not 0x1480_0000's", ((fn_t)0x54800000u)(), 0x2222);
            check("0x0480_0000's code", ((fn_t)0x04800000u)(), 0x1234);
            check("0xC480_0000's code, not 0x0480_0000's", ((fn_t)0xC4800000u)(), 0x2222);
        }
        /* an entry made invalid and repaired (by the general handler, tlbp / tlbwi): the repaired frame's code */
        pte(0x04A00000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        check("code, valid", ((fn_t)0x04A00000u)(), 0x1234);
        MTC0(10, 0x04A00000u); __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop\nnop"); ix = MFC0(0);
        MTC0(0, ix); MTC0(5, 0); MTC0(10, 0x04A00000u); MTC0(2, LO(0x120000, LD | LG)); MTC0(3, LO(0x121000, LV | LD | LG)); TLBWI(); __asm__ volatile("nop");
        LOG(0x60) = LO(0x122000, LV | LD | LG); LOG(0x64) = LO(0x123000, LV | LD | LG);
        g0 = LOG(0x40);
        check("code, made invalid and repaired", ((fn_t)0x04A00000u)(), 0x2222);
        check("code, invalid: the general vector", LOG(0x40), g0 + 1);
        check("code, invalid: TLBL", (LOG(0x44) >> 2) & 31u, 2);
        /* the speed: a loop from a mapped page against KSEG0's */
        volatile uint32_t *l = K1(0x124000);
        l[0] = 0x2484FFFFu; l[1] = 0x1480FFFEu; l[2] = 0; l[3] = 0x03E00008u; l[4] = 0x00801021u;
        volatile uint32_t *l2 = K1(0x126000);
        l2[0] = 0x2484FFFFu; l2[1] = 0x1480FFFEu; l2[2] = 0; l2[3] = 0x03E00008u; l2[4] = 0x00801021u;
        pte(0x04B00000u, LO(0x126000, LV | LD | LG), LO(0x127000, LV | LD | LG));
        typedef uint32_t (*fn1_t)(uint32_t);
        ((fn1_t)0x80124000u)(100); ((fn1_t)0x04B00000u)(100);
        uint32_t t = MFC0(9); ((fn1_t)0x80124000u)(3000); uint32_t t_k0 = MFC0(9) - t;
        t = MFC0(9); ((fn1_t)0x04B00000u)(3000); uint32_t t_map = MFC0(9) - t;
        puts_("loop of 3000: KSEG0 "); puthex(t_k0); puts_(", mapped "); puthex(t_map); puts_("\n");
        check("code speed: the mapped loop within 1.3x of KSEG0's", t_map * 10 <= t_k0 * 13, 1);
    }

    /* 12. the game's exception handler on a mapped page (a libultra that runs from 0x7000_0000, an entry the game wires) whose place
     * in the fetch table another page has taken (0x7F010's: the same low nine bits of the page number). The handler's first fetch
     * misses the tables, which the monitor fills -- and BadVAddr and EntryHi are still the fault's when the handler reads them
     * (the VR4300's TLB does not miss there): the general handler's tlbp finds the faulting entry, not its own page's. */
    {
        typedef uint32_t (*fn_t)(void);
        copy(0x128000, h_gen, h_gen_end);
        K1(0x180)[0] = 0x3C1A7001u;                      /* lui k0, 0x7001 */
        K1(0x180)[1] = 0x03400008u;                      /* jr k0 */
        K1(0x180)[2] = 0;
        flush_cpu_dcache(); flush_cpu_icache();
        MTC0(0, 1); MTC0(5, 0); MTC0(10, 0x70010000u); MTC0(2, LO(0x128000, LV | LD | LG)); MTC0(3, LO(0x129000, LV | LD | LG)); TLBWI(); __asm__ volatile("nop");
        MTC0(0, 2); MTC0(5, 0); MTC0(10, 0x7F010000u); MTC0(2, LO(0x120000, LV | LD | LG)); MTC0(3, LO(0x121000, LV | LD | LG)); TLBWI(); __asm__ volatile("nop");
        for (int k = 0; k < 3; k++) {                    /* (the handler's page is back in the table after each: put out again) */
            MTC0(0, 3); MTC0(5, 0); MTC0(10, 0x04C00000u); MTC0(2, LO(0x105000, LD | LG)); MTC0(3, LO(0x106000, LV | LD | LG)); TLBWI(); __asm__ volatile("nop");
            MTC0(10, 0);
            LOG(0x5C) = 0; LOG(0x60) = LO(0x105000, LV | LD | LG); LOG(0x64) = LO(0x106000, LV | LD | LG);
            g0 = LOG(0x40);
            check("mapped handler: 0x7F010's code", ((fn_t)0x7F010000u)(), 0x1234);
            check("mapped handler: the load", *(volatile uint32_t *)0x04C00000u, 0x05000000u);
            check("mapped handler: the general vector, once", LOG(0x40), g0 + 1);
            check("mapped handler: TLBL", (LOG(0x44) >> 2) & 31u, 2);
            check("mapped handler: BadVAddr is the fault's", LOG(0x48), 0x04C00000u);
            check("mapped handler: EntryHi is the fault's page", LOG(0x54) & 0xFFFFE000u, 0x04C00000u);
            check("mapped handler: 0x7F010's entry is its own still", ((fn_t)0x7F010000u)(), 0x1234);
        }
        copy(0x180, h_gen, h_gen_end);
        flush_cpu_dcache(); flush_cpu_icache();
    }

    check("no other exception", m_other, 0);
    check("no unmappable page", m_none, 0);
    puts_("monitor entries "); puthex(m_tlbops); puts_(", fills "); puthex(m_fills); puts_(", delivered "); puthex(m_delivered); puts_("\n");
    puts_(fails ? "tlbgame_test: FAILED\n" : "tlbgame_test: PASS\n");
    putc_(4);
    for (;;);
}
