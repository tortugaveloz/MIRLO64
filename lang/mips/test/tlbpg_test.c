/* The fetch page registers (mips_core's page registers, with TLB_RAM; sim/mips_soc): code run from a mapped page whose entry in the fetch
 * table has its C bit set (COP0 23's bit 5) goes through the I-cache -- indexed by the virtual address, tagged by the physical
 * one that the page register gives -- instead of one uncached word at a time. Checked here:
 *   - the speed: a loop on a cacheable mapped page runs within 1.3x of the same loop in KSEG0; on a page without the C bit it
 *     is still the slow path (and right);
 *   - more pages than the two registers, in turn: every result right;
 *   - a page unmapped after it ran (the next fetch is the TLB's exception, though its lines are still in the cache), remapped
 *     to another frame (the new frame's code, not the old lines) and back;
 *   - the ASID: an entry of another ASID does not map the page, a global one does;
 *   - the same frame at several virtual colours (the cache's index bits above the page offset) and KSEG0: code rewritten and
 *     dropped by a hit-type cache op through any one of them is the new code through all of them;
 *   - a straight line of code across a page boundary, and a call between pages.
 * On a CPU without the page registers the same program passes but for the speed check (the mapped loop is far slower). */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void putdec(uint32_t v) { char b[12]; int n = 0; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}

#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)))
enum { V = 1, D = 2, G = 4, FETCH = 8, C = 32 };
/* an entry for va's page: EntryHi has the page pair and the ASID */
static void tlb_set(uint32_t va, uint32_t asid, uint32_t pa, uint32_t flags)
{
    uint32_t hi = MFC0(10);
    MTC0(10, (va & ~0x1FFFu) | asid);
    MTC0(23, (pa & 0x1FFFF000u) | (va & 0x1000u ? 16u : 0u) | flags);
    MTC0(10, hi);
}
/* a fetch entry: valid and global; flags: C (cacheable) or 0 */
static void code_page(uint32_t va, uint32_t pa, uint32_t flags) { tlb_set(va, 0, pa, V | G | FETCH | flags); }

/* what the handler fills a faulting page with */
static volatile uint32_t s_fill_pa, s_fill_flags;
static volatile uint32_t s_traps, s_cause, s_badv, s_epc, s_fetch;
/* the checks for the VR4300's own TLB (tlbpg_test_real.c, built into this program): run when COP0 23 says that is the CPU's */
extern int real_main(void);
extern void real_trap_handler(void);
static int s_real;
static int cpu_has_real_tlb(void) { uint32_t c = MFC0(23); return (c >> 8) == 0x544C42u && (c & 2u); }

void trap_handler(void)
{
    if (s_real) { real_trap_handler(); return; }
    s_traps++;
    s_cause = MFC0(13); s_badv = MFC0(8); s_epc = MFC0(14);
    s_fetch = MFC0(23) >> 3 & 1;
    MTC0(23, s_fill_pa | (s_badv & 0x1000u ? 16u : 0u) | s_fill_flags | (s_fetch ? FETCH : 0));
}
#define CODE (s_cause >> 2 & 31)

#define K0(pa) ((volatile uint32_t *)(0x80000000u | (pa)))
#define K1(pa) ((volatile uint32_t *)(0xA0000000u | (pa)))
typedef uint32_t (*fn_t)(uint32_t a);

/* li v0, imm / jr ra / nop, written uncached into the frame at pa */
static void mkfn(uint32_t pa, uint32_t imm)
{
    volatile uint32_t *c = K1(pa);
    c[0] = 0x24020000u | (imm & 0xFFFFu); c[1] = 0x03E00008u; c[2] = 0;
}
/* the loop: addiu a0, a0, -1 / bne a0, zero, -2 / nop / jr ra / addu v0, a0, zero (a0 counts to 0) */
static void mkloop(uint32_t pa)
{
    volatile uint32_t *c = K1(pa);
    c[0] = 0x2484FFFFu; c[1] = 0x1480FFFEu; c[2] = 0; c[3] = 0x03E00008u; c[4] = 0x00801021u;
}
static uint32_t timed(uint32_t va, uint32_t n)
{
    uint32_t t = MFC0(9);
    uint32_t r = ((fn_t)va)(n);
    t = MFC0(9) - t;
    check("loop result", r, 0);
    return t;
}
/* Hit_Invalidate_I (cache op 0x10) on the line of va */
static void icache_hit_inval(uint32_t va) { __asm__ volatile(".set push\n.set mips3\ncache 0x10, 0(%0)\n.set pop" :: "r"(va) : "memory"); }

int main(void)
{
    if (cpu_has_real_tlb()) { s_real = 1; return real_main(); }
    puts_("tlbpg_test\n");
    if ((MFC0(23) & ~9u) != 0x54420000u) { puts_("tlbpg_test: SKIPPED, neither the tables nor the real TLB in this CPU\n"); putc_(4); for (;;); }
    for (uint32_t i = 0; i < 512; i++) { tlb_set(i << 12, 0, 0, 0); tlb_set(i << 12, 0, 0, FETCH); }

    /* ---- the plain case: a cacheable mapped page, run again and again ---- */
    mkfn(0x30000, 0x111);
    code_page(0x05000000u, 0x30000, C);
    check("run 1", ((fn_t)0x05000000u)(0), 0x111);
    check("run 2", ((fn_t)0x05000000u)(0), 0x111);
    check("no trap", s_traps, 0);

    /* ---- the speed ---- */
    mkloop(0x38000); mkloop(0x3A000); mkloop(0x3C000);
    code_page(0x05008000u, 0x3A000, C);                              /* (colour 0; the frame's is 2) */
    code_page(0x0500B000u, 0x3C000, 0);                              /* (no C bit: uncached) */
    (void)timed(0x80038000u, 100);                                   /* (warm) */
    (void)timed(0x05008000u, 100);
    uint32_t t_k0 = timed(0x80038000u, 3000), t_map = timed(0x05008000u, 3000);
    uint32_t t_unc = timed(0x0500B000u, 300);
    puts_("loop of 3000: KSEG0 "); putdec(t_k0); puts_(", mapped cacheable "); putdec(t_map);
    puts_("; loop of 300 uncached mapped "); putdec(t_unc); puts_(" (Count ticks)\n");
    check("mapped cacheable loop within 1.3x of KSEG0's", t_map * 10 <= t_k0 * 13, 1);
    check("uncached mapped: still the slow path", t_unc > t_k0 / 10 * 6, 1);
    check("speed: no trap", s_traps, 0);

    /* ---- more pages than registers, in turn ---- */
    for (uint32_t i = 0; i < 5; i++) { mkfn(0x40000 + 0x2000 * i, 0x200 + i); code_page(0x06000000u + 0x1000 * i, 0x40000 + 0x2000 * i, C); }
    for (int r = 0; r < 6; r++)
        for (uint32_t i = 0; i < 5; i++) check("5 pages in turn", ((fn_t)(0x06000000u + 0x1000 * i))(0), 0x200 + i);
    for (int r = 0; r < 4; r++) {                                    /* two pages alternately, then a third, then back */
        check("A", ((fn_t)0x06000000u)(0), 0x200); check("B", ((fn_t)0x06001000u)(0), 0x201);
    }
    check("C", ((fn_t)0x06002000u)(0), 0x202); check("A", ((fn_t)0x06000000u)(0), 0x200);
    check("B", ((fn_t)0x06001000u)(0), 0x201); check("C", ((fn_t)0x06002000u)(0), 0x202);
    check("no trap in turn", s_traps, 0);

    /* ---- unmapped after it ran: the lines are still in the cache, the page is not there ---- */
    mkfn(0x50000, 0x333); mkfn(0x52000, 0x444);
    code_page(0x07000000u, 0x50000, C);
    check("before", ((fn_t)0x07000000u)(0), 0x333);
    check("before again", ((fn_t)0x07000000u)(0), 0x333);
    tlb_set(0x07000000u, 0, 0, FETCH);                               /* (invalid) */
    s_fill_pa = 0x52000; s_fill_flags = V | G | C;
    check("unmapped, the handler maps another frame", ((fn_t)0x07000000u)(0), 0x444);
    check("unmapped: trap", s_traps, 1);
    check("unmapped: TLBL", CODE, 2);
    check("unmapped: a fetch", s_fetch, 1);
    check("unmapped: badv", s_badv, 0x07000000u);
    check("unmapped: epc", s_epc, 0x07000000u);
    check("then no trap", ((fn_t)0x07000000u)(0), 0x444);
    check("then no trap (count)", s_traps, 1);
    /* remapped by a table write, no exception: the new frame's code, not the lines of the old */
    code_page(0x07000000u, 0x50000, C);
    check("remapped to the first frame", ((fn_t)0x07000000u)(0), 0x333);
    code_page(0x07000000u, 0x52000, C);
    check("remapped to the second", ((fn_t)0x07000000u)(0), 0x444);
    code_page(0x07000000u, 0x50000, C);
    check("and back", ((fn_t)0x07000000u)(0), 0x333);
    check("remaps: no trap", s_traps, 1);
    /* an invalid entry again: the exception, the handler maps the first frame, the code runs */
    tlb_set(0x07000000u, 0, 0, FETCH);
    s_fill_pa = 0x50000; s_fill_flags = V | G | C;
    check("invalid again", ((fn_t)0x07000000u)(0), 0x333);
    check("invalid again: trap", s_traps, 2);

    /* ---- the ASID ---- */
    mkfn(0x54000, 0x555); mkfn(0x56000, 0x666);
    tlb_set(0x08000000u, 5, 0x54000, V | C | FETCH);                 /* (not global) */
    MTC0(10, 5);
    uint32_t t0 = s_traps;
    check("asid 5", ((fn_t)0x08000000u)(0), 0x555);
    check("asid 5 again", ((fn_t)0x08000000u)(0), 0x555);
    check("asid 5: no trap", s_traps, t0);
    MTC0(10, 6);                                                     /* the register holds ASID 5's page: not this one's */
    s_fill_pa = 0x56000; s_fill_flags = V | C;                       /* (filled for the ASID in EntryHi: 6) */
    check("asid 6", ((fn_t)0x08000000u)(0), 0x666);
    check("asid 6: trap", s_traps, t0 + 1);
    check("asid 6: code", CODE, 2);
    check("asid 6 again", ((fn_t)0x08000000u)(0), 0x666);
    check("asid 6 again: no trap", s_traps, t0 + 1);
    MTC0(10, 5);
    s_fill_pa = 0x54000;
    check("asid 5 again after 6", ((fn_t)0x08000000u)(0), 0x555);
    check("asid 5 after 6: trap", s_traps, t0 + 2);
    MTC0(10, 0);
    /* a global entry maps the page for every ASID */
    code_page(0x08002000u, 0x54000, C);
    MTC0(10, 9); check("global, asid 9", ((fn_t)0x08002000u)(0), 0x555);
    MTC0(10, 3); check("global, asid 3", ((fn_t)0x08002000u)(0), 0x555);
    MTC0(10, 0);
    check("global: no more traps", s_traps, t0 + 2);

    /* ---- one frame, several colours ---- */
    mkfn(0x34000, 0x700);
    code_page(0x09000000u, 0x34000, C);                              /* colour 0 (the frame's: 4) */
    code_page(0x09003000u, 0x34000, C);                              /* colour 3 */
    code_page(0x09004000u, 0x34000, C);                              /* colour 4, the frame's own: the same lines as KSEG0's */
    const uint32_t al[4] = { 0x09000000u, 0x09003000u, 0x09004000u, 0x80034000u };
    for (int i = 0; i < 4; i++) check("alias, old code", ((fn_t)al[i])(0), 0x700);
    for (int k = 0; k < 4; k++) {                                    /* the code rewritten, dropped through alias k alone */
        mkfn(0x34000, 0x710 + k);
        icache_hit_inval(al[k]);
        for (int i = 0; i < 4; i++) check("alias, new code", ((fn_t)al[(i + k) & 3])(0), 0x710 + k);
    }
    check("alias: no trap", s_traps, t0 + 2);

    /* ---- straight code across a page boundary, a call between pages ---- */
    {
        volatile uint32_t *c = K1(0x60000);
        for (int i = 0; i < 2048; i++) c[i] = 0;                    /* 8 KiB of nops... */
        c[0x3FE] = 0x24020777u;                                      /* li v0, 0x777: the page's last but one word */
        c[0x400] = 0x24420001u;                                      /* addiu v0, v0, 1: the next page's first */
        c[0x401] = 0x03E00008u;                                      /* jr ra */
        code_page(0x0A000000u, 0x60000, C); code_page(0x0A001000u, 0x61000, C);
        check("across a page", ((fn_t)0x0A000000u)(0), 0x778);
        check("across a page, again", ((fn_t)0x0A000000u)(0), 0x778);
        volatile uint32_t *f = K1(0x62000);
        f[0] = 0x24420005u; f[1] = 0x03E00008u; f[2] = 0;           /* addiu v0, v0, 5 / jr ra / nop */
        code_page(0x0B003000u, 0x62000, C);
        volatile uint32_t *g = K1(0x64000);
        g[0] = 0x03E01825u;                                          /* or v1, ra, zero */
        g[1] = 0x0C000000u | (0x0B003000u >> 2);                     /* jal 0x0B003000 */
        g[2] = 0x24020010u;                                          /* (delay slot) li v0, 0x10 */
        g[3] = 0x24420001u;                                          /* addiu v0, v0, 1 */
        g[4] = 0x00600008u; g[5] = 0;                                /* jr v1 */
        code_page(0x0C002000u, 0x64000, C);
        check("a call between pages", ((fn_t)0x0C002000u)(0), 0x16);
        check("and again", ((fn_t)0x0C002000u)(0), 0x16);
    }
    /* ---- a branch in a page's last word, its delay slot on the next page; a loop across the boundary; three pages in a loop
     * (two registers: every turn misses) ---- */
    {
        volatile uint32_t *c = K1(0x66000);
        c[0x3FE] = 0x24020040u;                                      /* li v0, 0x40 */
        c[0x3FF] = 0x03E00008u;                                      /* jr ra (the last word of the page) */
        c[0x400] = 0x24420002u;                                      /* its delay slot: addiu v0, v0, 2 */
        code_page(0x0D000000u, 0x66000, C); code_page(0x0D001000u, 0x67000, C);
        check("slot on the next page", ((fn_t)0x0D000FF8u)(0), 0x42);
        check("slot on the next page, again", ((fn_t)0x0D000FF8u)(0), 0x42);
        for (uint32_t i = 0; i < 3; i++) { mkfn(0x70000 + 0x2000 * i, 0x900 + i); code_page(0x10020000u + 0x1000 * i, 0x70000 + 0x2000 * i, C); }
        for (uint32_t i = 0; i < 3; i++) check("(pages that evict)", ((fn_t)(0x10020000u + 0x1000 * i))(0), 0x900 + i);   /* (the registers hold other pages now) */
        check("slot on the next page, registers elsewhere", ((fn_t)0x0D000FF8u)(0), 0x42);
        volatile uint32_t *l = K1(0x68000);
        l[0x3FE] = 0x2484FFFFu; l[0x3FF] = 0x1480FFFEu;              /* addiu a0, a0, -1 / bne a0, zero, back (the last word) */
        l[0x400] = 0; l[0x401] = 0x03E00008u; l[0x402] = 0x00801021u;  /* nop (the slot, next page) / jr ra / addu v0, a0, zero */
        code_page(0x0E000000u, 0x68000, C); code_page(0x0E001000u, 0x69000, C);
        check("a loop across the boundary", ((fn_t)0x0E000FF8u)(500), 0);
        volatile uint32_t *a = K1(0x6A000), *b = K1(0x6C000), *d = K1(0x6E000);
        a[0] = 0x2484FFFFu; a[1] = 0x08000000u | (0x0F001000u >> 2); a[2] = 0;               /* addiu a0, a0, -1 / j B / nop */
        b[0] = 0x08000000u | (0x0F002000u >> 2); b[1] = 0;                                    /* j C / nop */
        d[0] = 0x1480FFFFu - 0x800u; d[1] = 0; d[2] = 0x03E00008u; d[3] = 0x00801021u;       /* bne a0, zero, A (-0x801 words) / nop / jr ra / addu v0, a0, zero */
        code_page(0x0F000000u, 0x6A000, C); code_page(0x0F001000u, 0x6C000, C); code_page(0x0F002000u, 0x6E000, C);
        check("three pages in a loop", ((fn_t)0x0F000000u)(300), 0);
    }
    check("end: traps", s_traps, t0 + 2);
    puts_(fails ? "tlbpg_test: FAILED\n" : "tlbpg_test: PASS\n");
    putc_(4);
    for (;;);
}
