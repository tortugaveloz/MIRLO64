/* mips_core's TLB_RAM with MON (the SoC built with MIRLO_TLB_RAM and MIRLO_MON; sim/mips_soc): while the game runs
 * (MCTL.GAME), its own tlbwi, tlbwr, tlbr and tlbp are a Reserved Instruction for the monitor level, which keeps the 32
 * entries as the VR4300 would and does them on the game's COP0 registers -- so that a game's own osMapTLB, osUnmapTLB,
 * osUnmapTLBAll and __osProbeTLB run as they are, and nothing of them has to be found and patched.
 *
 * The monitor here is this program's own (init_asm.S: exception_entry, MIRLO_VEC_JUMP), in C behind a trampoline that
 * saves the registers it uses: tlbwi / tlbwr / tlbr / tlbp as n64rt (lang/n64rt/n64rt_os.c, tlb_insn) does them -- an
 * entry's registers kept as they were written, a tlbp searching them, and the translation tables filled, a 4 KiB page at
 * a time, from the entry a TLB exception names. The "game" is this program's main(), with MCTL.GAME set around it.
 *
 * Checked: a pair of pages mapped by tlbwi is read and written through (both pages, 4 KiB and 64 KiB, odd and even); tlbr
 * gives the registers back as the VR4300 does (PageMask, EntryHi without the bits under the mask, G in both EntryLos);
 * tlbp finds an entry by VPN2 and ASID (not a non-global one of another ASID), or sets Index's P bit; tlbwr writes at
 * Random (Wired and up); an entry replaced or invalidated (osUnmapTLB's EntryHi = 0x80000000) leaves no stale page in
 * the tables; a TLB instruction in a branch's delay slot is done and the branch taken; with GAME off the instructions
 * stay the core's own (no monitor entry); and Index's P bit is the monitor's alone to write. */
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
#define TLBWR() __asm__ volatile(".set push\n.set mips3\ntlbwr\n.set pop")
#define TLBR()  __asm__ volatile(".set push\n.set mips3\ntlbr\n.set pop")
#define TLBP()  __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop")

/* ---- the monitor ---------------------------------------------------------------------- */
static struct { uint32_t mask, hi, lo0, lo1; } raw[32];     /* hi as written; a free entry's 0x80000000 */
static volatile uint32_t m_tlbops, m_fills, m_unexpected, m_other;
static volatile uint32_t m_random_seen;
static uint32_t m_lastbad;

/* an entry of the CPU's tables (COP0 23): the page of EntryHi's pair, the physical page, the table, flags */
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
static int entry_used(int i) { return raw[i].hi != 0x80000000u && ((raw[i].lo0 | raw[i].lo1) & 2u); }
static void do_write(uint32_t i)
{
    uint32_t mask = MFC0(5) & 0x01FFE000u;
    int was = entry_used((int)i);
    raw[i].mask = mask;
    raw[i].hi = MFC0(10) & ~(mask | 0x1F00u);
    raw[i].lo0 = MFC0(2) & 0x3FFFFFFFu;
    raw[i].lo1 = MFC0(3) & 0x3FFFFFFFu;
    if (was) tr_flush();                       /* (what the tables took from the entry before) */
}
static int fill(uint32_t badv, uint32_t fetch)
{
    uint32_t asid = MFC0(10) & 0xFF;
    for (int i = 0; i < 32; i++) {
        uint32_t span = (raw[i].mask + 0x2000u) >> 1, va = raw[i].hi & 0xFFFFE000u & ~(2u * span - 1u), off = badv - va;
        if (raw[i].hi == 0x80000000u || off >= 2u * span) continue;
        uint32_t g = raw[i].lo0 & raw[i].lo1 & 1u;
        if (!g && (raw[i].hi & 0xFF) != asid) continue;
        uint32_t lo = off >= span ? raw[i].lo1 : raw[i].lo0;
        if (!(lo & 2u)) return 0;
        uint32_t pa = (((lo >> 6) << 12) & ~(span - 1u)) + (off & (span - 1u) & ~0xFFFu);
        uint32_t hi = MFC0(10);
        MTC0(10, (badv & 0xFFFFE000u) | asid);                 /* (the tables' write is of EntryHi's page pair; a fault leaves the game's EntryHi alone) */
        tr_write(badv, pa, 3u | (g ? 4u : 0u) | (((lo >> 3) & 7u) != 2u ? 32u : 0u) | (fetch ? 8u : 0u));   /* (bit 5: cacheable, as n64rt does) */
        MTC0(10, hi);
        return 1;
    }
    return 0;
}
void mon_c(void)
{
    uint32_t mctl = MFC0(7), code = (mctl >> 3) & 31u, bd = (mctl >> 2) & 1u, epc = MFC0(30);
    if (code == 10) {
        uint32_t pc = epc + (bd ? 4u : 0u), insn = *(volatile uint32_t *)(uintptr_t)pc, next = pc + 4u;
        if ((insn & 0xFFFFFFC0u) == 0x42000000u) {
            uint32_t v, k = insn & 63u;
            m_tlbops++;
            if (k == 0x02) { v = MFC0(0) & 63u; if (v < 32u) do_write(v); }
            else if (k == 0x06) { v = MFC0(1); m_random_seen = v; do_write(v & 31u); }
            else if (k == 0x01) {
                v = MFC0(0) & 31u;
                uint32_t g = raw[v].lo0 & raw[v].lo1 & 1u;
                MTC0(5, raw[v].mask); MTC0(10, raw[v].hi == 0x80000000u ? 0x80000000u : raw[v].hi);
                MTC0(2, (raw[v].lo0 & ~1u) | g); MTC0(3, (raw[v].lo1 & ~1u) | g);
            } else if (k == 0x08) {
                uint32_t hi = MFC0(10), found = 0x80000000u;
                for (uint32_t i = 0; i < 32; i++) {
                    uint32_t vpn = ~(raw[i].mask | 0x1FFFu);
                    if ((hi & vpn) == (raw[i].hi & vpn) && ((raw[i].lo0 & raw[i].lo1 & 1u) || (hi & 0xFF) == (raw[i].hi & 0xFF))) { found = i; break; }
                }
                MTC0(0, found);
            } else { m_other++; }
            if (bd) {                                      /* the branch behind: only `b` (beq $0, $0) is in this test */
                uint32_t b = *(volatile uint32_t *)(uintptr_t)epc;
                if ((b >> 16) == 0x1000u) next = epc + 4u + ((uint32_t)(int32_t)(int16_t)b << 2); else m_other++;
            }
            MTC0(30, next);
            return;
        }
    }
    if (code >= 1 && code <= 3) {                          /* the translation tables miss: from the entries */
        uint32_t badv = MFC0(22), fetch = MFC0(23) >> 3 & 1u;   /* (COP0 22: the fault's address) */
        if (fill(badv, fetch)) { m_fills++; return; }
        m_unexpected++; m_lastbad = badv;
        MTC0(30, MFC0(30) + 4u);                          /* (the access is skipped) */
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

/* ---- the game ------------------------------------------------------------------------- */
/* EntryLo for a physical address, cached, with V / D / G as asked */
#define LO(pa, f) ((((uint32_t)(pa) >> 12) << 6) | 0x18u | (f))
enum { LV = 2, LD = 4, LG = 1 };
static void tlb_wi(uint32_t idx, uint32_t pm, uint32_t hi, uint32_t lo0, uint32_t lo1)
{
    MTC0(0, idx); MTC0(5, pm); MTC0(10, hi); MTC0(2, lo0); MTC0(3, lo1);
    TLBWI();
    __asm__ volatile("nop");                    /* (not into the delay slot of the call that follows: the monitor does a tlbwi in a branch's slot, but a `b`'s only) */
}

int main(void)
{
    puts_("tlbmon_test\n");
    if ((MFC0(23) & ~8u) != 0x54420001u) { puts_("tlbmon_test: no tables with the monitor in this CPU\ntlbmon_test: FAILED\n"); putc_(4); for (;;); }
    if ((monitor_id_read() & 0xFFFF0001u) != 0x4D4F0001u) { puts_("no monitor in this SoC\ntlbmon_test: FAILED\n"); putc_(4); for (;;); }
    for (int i = 0; i < 32; i++) raw[i].hi = 0x80000000u;
    for (uint32_t i = 0; i < 0x200; i++) { K1(0x10000)[i] = 0x10000000u + i; K1(0x12000)[i] = 0x12000000u + i; }
    K1(0x20000)[0] = 0x20000000u; K1(0x25000)[0] = 0x25000000u; K1(0x21000)[0] = 0x21000000u;
    K1(0x21234)[0] = 0xA1234567u; K1(0x31234)[0] = 0xB1234567u;
    K1(0x40000)[0] = 0x40000000u; K1(0x50000)[0] = 0x50000000u;
    flush_cpu_dcache();
    tr_flush();
    monitor_pend_write(0xF); monitor_mask_write(0u); monitor_ctl_write(1u);
    MTC0(7, 2);                                   /* MCTL.GAME: from here the TLB instructions are the monitor's */
    volatile uint32_t *m = (volatile uint32_t *)0x04000000u;

    /* tlbwi: a pair of 4 KiB pages, global, dirty, valid */
    tlb_wi(3, 0, 0x04000000u, LO(0x10000, LV | LD | LG), LO(0x12000, LV | LD | LG));
    check("a tlbwi was a monitor entry", m_tlbops, 1);
    check("even page", m[0], 0x10000000u);
    check("even page 2", m[5], 0x10000005u);
    check("odd page", m[0x400], 0x12000000u);
    check("odd page 2", m[0x400 + 7], 0x12000007u);
    m[1] = 0xCAFE0001u; m[0x401] = 0xCAFE0002u;
    check("store through the even page", K1(0x10000)[1], 0xCAFE0001u);
    check("store through the odd page", K1(0x12000)[1], 0xCAFE0002u);
    check("fills: one a page", m_fills, 2);

    /* tlbr: the registers back, as written (the odd EntryLo has the G bit of both) */
    tlb_wi(4, 0, 0x04002000u | 5u, LO(0x40000, LV | LD), LO(0x50000, LV | LD | LG));     /* not global (the G is the AND of the two) */
    MTC0(10, 0x7000); MTC0(2, 0); MTC0(3, 0); MTC0(5, 0x1E000); MTC0(0, 4);
    TLBR();
    check("tlbr: EntryHi", MFC0(10), 0x04002005u);
    check("tlbr: EntryLo0", MFC0(2), LO(0x40000, LV | LD));
    check("tlbr: EntryLo1", MFC0(3), LO(0x50000, LV | LD));
    check("tlbr: PageMask", MFC0(5), 0u);
    MTC0(0, 3);
    TLBR();
    check("tlbr (global): EntryHi", MFC0(10), 0x04000000u);
    check("tlbr (global): EntryLo0", MFC0(2), LO(0x10000, LV | LD | LG));
    check("tlbr (global): EntryLo1", MFC0(3), LO(0x12000, LV | LD | LG));

    /* tlbp: by VPN2 and ASID; a non-global entry is its ASID's only; P when there is none */
    MTC0(10, 0x04000000u); TLBP();
    check("tlbp: global entry", MFC0(0), 3);
    MTC0(10, 0x04001000u | 9u); TLBP();               /* (the odd page of the pair: same VPN2; ASID does not matter for a global one) */
    check("tlbp: odd page, other ASID", MFC0(0), 3);
    MTC0(10, 0x04002000u | 5u); TLBP();
    check("tlbp: ASID 5", MFC0(0), 4);
    MTC0(10, 0x04002000u | 6u); TLBP();
    check("tlbp: another ASID", MFC0(0), 0x80000000u);
    MTC0(10, 0x04004000u); TLBP();
    check("tlbp: no entry", MFC0(0), 0x80000000u);

    /* Index's P bit is not the game's to write */
    MTC0(10, 0x04000000u); TLBP();                    /* (a hit: P is clear) */
    MTC0(0, 0x80000005u);
    check("mtc0 Index: no P", MFC0(0), 5u);
    MTC0(10, 0x04004000u); TLBP();                    /* (a miss: P is set, and an mtc0 does not clear it) */
    MTC0(0, 6u);
    check("mtc0 Index: P stays", MFC0(0), 0x80000006u);

    /* the ASID of the non-global entry: a page of it is mapped for ASID 5 only */
    MTC0(10, 5);
    check("ASID 5: the page", *(volatile uint32_t *)0x04002000u, 0x40000000u);
    check("ASID 5: the odd page", *(volatile uint32_t *)0x04003000u, 0x50000000u);
    MTC0(10, 0);

    /* tlbwr: at Random, Wired and up */
    MTC0(6, 8);                                      /* Wired */
    MTC0(5, 0); MTC0(10, 0x04100000u); MTC0(2, LO(0x40000, LV | LD | LG)); MTC0(3, LO(0x50000, LV | LD | LG));
    TLBWR();
    check("tlbwr: Random from Wired up", m_random_seen >= 8u && m_random_seen <= 31u, 1);
    check("tlbwr: the entry is there", raw[m_random_seen].hi, 0x04100000u);
    MTC0(10, 0x04100000u); TLBP();
    check("tlbp finds the tlbwr's", MFC0(0), m_random_seen);
    check("tlbwr: mapped", *(volatile uint32_t *)0x04100000u, 0x40000000u);
    check("tlbwr: odd", *(volatile uint32_t *)0x04101000u, 0x50000000u);
    MTC0(6, 0);

    /* 64 KiB pages (PageMask 0x1E000): a pair of two, any 4 KiB page of them, VPN2 under the mask */
    tlb_wi(6, 0x1E000u, 0x08000000u | 0x7000u, LO(0x20000, LV | LD | LG), LO(0x30000, LV | LD | LG));
    check("64 KiB: first page, offset 0x5000", *(volatile uint32_t *)0x08005000u, 0x25000000u);
    check("64 KiB: first page, offset 0x1000", *(volatile uint32_t *)0x08001000u, 0x21000000u);
    check("64 KiB: first page, offset 0x1234", *(volatile uint32_t *)0x08001234u, 0xA1234567u);
    check("64 KiB: second page, offset 0x1234", *(volatile uint32_t *)0x08011234u, 0xB1234567u);
    MTC0(0, 6); TLBR();
    check("64 KiB tlbr: PageMask", MFC0(5), 0x1E000u);
    check("64 KiB tlbr: EntryHi (VPN2 under the mask)", MFC0(10), 0x08000000u);
    MTC0(10, 0x08010000u); TLBP();
    check("64 KiB tlbp: an address in the second page", MFC0(0), 6);

    /* an entry replaced: the page the tables took is gone */
    check("before: even page", m[0], 0x10000000u);
    uint32_t u0 = m_unexpected;
    tlb_wi(3, 0, 0x04000000u, LO(0x20000, LV | LD | LG), LO(0x12000, LV | LD | LG));
    check("replaced: the new even page", m[0], 0x20000000u);
    check("replaced: the odd page", m[0x400], 0x12000000u);
    check("replaced: nothing unexpected", m_unexpected, u0);

    /* osUnmapTLB's: EntryHi 0x80000000, EntryLo 0 -- the pages are gone, tlbp finds none */
    tlb_wi(3, 0, 0x80000000u, 0, 0);
    MTC0(10, 0x04000000u); TLBP();
    check("unmapped: tlbp", MFC0(0), 0x80000000u);
    u0 = m_unexpected;
    volatile uint32_t r = 0x13572468u;
    r = m[0];                                         /* (no entry: the monitor skips the load) */
    check("unmapped: the access took the TLB exception", m_unexpected, u0 + 1);
    check("unmapped: at the page", m_lastbad, 0x04000000u);
    (void)r;

    /* an entry with the valid bit clear: the page is not mapped, the other of the pair is */
    tlb_wi(7, 0, 0x04200000u, LO(0x40000, LD | LG), LO(0x50000, LV | LD | LG));
    u0 = m_unexpected;
    r = *(volatile uint32_t *)0x04200000u;
    check("invalid even page", m_unexpected, u0 + 1);
    check("valid odd page", *(volatile uint32_t *)0x04201000u, 0x50000000u);

    /* a TLB instruction in a branch's delay slot: done, and the branch taken (the instruction behind the slot not run) */
    {
        uint32_t x = 0;
        MTC0(0, 9); MTC0(5, 0); MTC0(10, 0x04300000u); MTC0(2, LO(0x40000, LV | LD | LG)); MTC0(3, LO(0x50000, LV | LD | LG));
        uint32_t ops = m_tlbops;
        __asm__ volatile(".set push\n.set noreorder\n.set mips3\n"
                         "    b 1f\n"
                         "    tlbwi\n"
                         "    addiu %0, %0, 100\n"                 /* (skipped by the branch) */
                         "1:  addiu %0, %0, 1\n"
                         ".set pop" : "+r"(x));
        check("delay slot: the branch taken", x, 1);
        check("delay slot: a monitor entry", m_tlbops, ops + 1);
        check("delay slot: the entry written", *(volatile uint32_t *)0x04300000u, 0x40000000u);
    }

    /* code from cacheable mapped pages (the fetch page registers): an entry replaced by the game's tlbwi, a 64 KiB page */
    {
        const uint32_t addiu = 0x24020000u;
        K1(0x60000)[0] = addiu | 0x6001; K1(0x60000)[1] = 0x03E00008u; K1(0x60000)[2] = 0;
        K1(0x62000)[0] = addiu | 0x6002; K1(0x62000)[1] = 0x03E00008u; K1(0x62000)[2] = 0;
        K1(0x75000)[0] = addiu | 0x7005; K1(0x75000)[1] = 0x03E00008u; K1(0x75000)[2] = 0;
        K1(0x7F000)[0] = addiu | 0x700F; K1(0x7F000)[1] = 0x03E00008u; K1(0x7F000)[2] = 0;
        uint32_t f0 = m_fills;
        typedef uint32_t (*fn_t)(void);
        tlb_wi(10, 0, 0x04400000u, LO(0x60000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: first run", ((fn_t)0x04400000u)(), 0x6001);
        check("code: again", ((fn_t)0x04400000u)(), 0x6001);
        check("code: one fill", m_fills, f0 + 1);
        tlb_wi(10, 0, 0x04400000u, LO(0x62000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: the entry replaced by a tlbwi: the new frame", ((fn_t)0x04400000u)(), 0x6002);
        check("code: and again", ((fn_t)0x04400000u)(), 0x6002);
        tlb_wi(10, 0, 0x04400000u, LO(0x60000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: back to the first", ((fn_t)0x04400000u)(), 0x6001);
        check("code: fills", m_fills, f0 + 3);
        tlb_wi(11, 0x1E000u, 0x08100000u, LO(0x70000, LV | LD | LG), LO(0x80000, LV | LD | LG));
        check("code in a 64 KiB page", ((fn_t)0x08105000u)(), 0x7005);
        check("code in a 64 KiB page, again", ((fn_t)0x08105000u)(), 0x7005);
        check("code in the page's last 4 KiB", ((fn_t)0x0810F000u)(), 0x700F);
    }

    /* with GAME off the instructions are the core's own: no monitor entry, tlbp finds nothing */
    MTC0(7, 0);
    uint32_t ops = m_tlbops;
    MTC0(10, 0x04100000u); MTC0(0, 0); TLBP();
    uint32_t idx = MFC0(0);
    TLBWI();
    check("GAME off: no monitor entry", m_tlbops, ops);
    check("GAME off: the core's tlbp finds none", idx >> 31, 1);

    check("no other exception", m_other, 0);
    puts_("monitor entries "); puthex(m_tlbops); puts_(", fills "); puthex(m_fills); puts_(", unexpected "); puthex(m_unexpected); puts_("\n");
    puts_(fails ? "tlbmon_test: FAILED\n" : "tlbmon_test: PASS\n");
    putc_(4);
    for (;;);
}
