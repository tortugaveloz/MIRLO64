/* ldl, ldr, sdl, sdr on the SoC (sim/mips_soc, or the Pocket): each of the four at every byte address of a doubleword, on
 * data chosen so that a wrong byte, lane or strobe shows, against the same bytes worked out in C (64-bit shifts and masks, no
 * unaligned instruction), in cached memory (KSEG0, and the SDRAM direct), around the cache (KSEG1) and on a mapped page; the pairs a block move is
 * made of (ldl / ldr into one register, sdl / sdr out of it) at every alignment of source and destination; and the
 * instructions in a branch's delay slot and right behind a store to the same doubleword. A core without them (COP0 register 7's
 * bit 27 is W64_UA) is a Reserved Instruction for the runtime, which this test does not have: it says so and passes.
 * Prints PASS or the failures on the UART, then a 0x04 (the bench stops). In sim/mips_soc the bench loads a program
 * only beside a ROM: ROM=<any file> obj/tb_soc build/ualn_test.bin -c 40. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void putdec(uint32_t v) { char b[11]; int n = 0; do { b[n++] = '0' + v % 10; v /= 10; } while (v); while (n) putc_(b[--n]); }
static void put64(uint64_t v) { puthex((uint32_t)(v >> 32)); puthex((uint32_t)v); }
static int fails;
static void check(const char *what, int where, uint32_t n, uint64_t got, uint64_t want)
{
    if (got == want) return;
    if (++fails > 20) return;
    puts_("FAIL "); puts_(what); puts_(" mem "); putdec(where); puts_(" n "); putdec(n);
    puts_(": got "); put64(got); puts_(" want "); put64(want); puts_("\n");
}
static uint32_t count(void) { uint32_t c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }

/* a mapped page (the translation tables of TLB_RAM, as tlbram_test writes them): va's page onto an RDRAM page, valid, dirty, global */
#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)))
static void tlb_map(uint32_t va, uint32_t pa)
{
    uint32_t hi = MFC0(10);
    MTC0(10, (va & ~0x1FFFu));
    MTC0(23, (pa & 0x1FFFF000u) | (va & 0x1000u ? 16u : 0u) | 7u);
    MTC0(10, hi);
}

/* the reference: the instructions' definitions (the VR4300 manual's), on the doubleword m in memory and the register r */
static uint64_t ref_ldl(uint64_t m, uint64_t r, uint32_t n) { return n ? (r & ((1ull << (8 * n)) - 1)) | (m << (8 * n)) : m; }
static uint64_t ref_ldr(uint64_t m, uint64_t r, uint32_t n) { return n == 7 ? m : (r & (~0ull << (8 * (n + 1)))) | (m >> (8 * (7 - n))); }
static uint64_t ref_sdl(uint64_t m, uint64_t r, uint32_t n) { return n ? (m & (~0ull << (8 * (8 - n)))) | (r >> (8 * n)) : r; }
static uint64_t ref_sdr(uint64_t m, uint64_t r, uint32_t n) { return n == 7 ? r : (m & ((1ull << (8 * (7 - n))) - 1)) | (r << (8 * (7 - n))); }

#define LD_(op, out, a, r) __asm__ volatile(".set push\n.set mips3\n.set noreorder\n.set noat\n" \
    "ld $8, 0(%2)\n" op " $8, 0(%1)\n sd $8, 0(%0)\n.set pop\n" : : "r"(out), "r"(a), "r"(r) : "$8", "memory")
static void do_ldl(uint64_t *out, const volatile void *a, const uint64_t *r) { LD_("ldl", out, a, r); }
static void do_ldr(uint64_t *out, const volatile void *a, const uint64_t *r) { LD_("ldr", out, a, r); }
#define ST_(op, a, r) __asm__ volatile(".set push\n.set mips3\n.set noreorder\n.set noat\n" \
    "ld $8, 0(%1)\n" op " $8, 0(%0)\n.set pop\n" : : "r"(a), "r"(r) : "$8", "memory")
static void do_sdl(volatile void *a, const uint64_t *r) { ST_("sdl", a, r); }
static void do_sdr(volatile void *a, const uint64_t *r) { ST_("sdr", a, r); }

/* ldl and ldr in the delay slot of a branch, taken and not */
static void do_slot(uint64_t *out, volatile uint64_t *w, const uint64_t *r, uint32_t n)
{
    volatile uint8_t *a = (volatile uint8_t *)w + n;
    __asm__ volatile(
        ".set push\n.set mips3\n.set noreorder\n.set noat\n"
        "ld   $8, 0(%2)\n"
        "ld   $9, 0(%2)\n"
        "b    1f\n"
        " ldl $8, 0(%1)\n"                          /* the load in the slot of a taken branch */
        "nop\n"
        "1:\n"
        "sd   $8, 0(%0)\n"
        "bne  $0, $0, 2f\n"
        " ldr $9, 0(%1)\n"                          /* ... and of one not taken */
        "sd   $9, 8(%0)\n"
        "2:\n"
        ".set pop\n"
        : : "r"(out), "r"(a), "r"(r) : "$8", "$9", "memory");
}

#define NPAT 6
static const uint64_t mem_pat[NPAT] = { 0x0102030405060708ull, 0xF1E2D3C4B5A69788ull, 0x8000000000000001ull, 0xFFFFFFFFFFFFFFFFull, 0x00000000FFFFFFFFull, 0x7FFFFFFF80000000ull };
static const uint64_t reg_pat[NPAT] = { 0xA1B2C3D4E5F60718ull, 0x0000000000000000ull, 0xFFFFFFFF00000000ull, 0x1122334455667788ull, 0x80000000FFFFFFFFull, 0xFFFFFFFFFFFFFFFFull };

static uint64_t buf_sd[8] __attribute__((aligned(16)));              /* the SDRAM direct (cached) */
static uint64_t out[4] __attribute__((aligned(8)));
static uint64_t rr __attribute__((aligned(8)));
/* the blocks of a move: 24 bytes at an odd source and destination */
static uint8_t src_b[48] __attribute__((aligned(8))), dst_b[48] __attribute__((aligned(8))), want_b[48] __attribute__((aligned(8)));

static void one_mem(const char *name, int where, volatile uint64_t *w)
{
    for (int p = 0; p < NPAT; p++) {
        for (uint32_t n = 0; n < 8; n++) {
            uint64_t m = mem_pat[p], r = reg_pat[(p + n) % NPAT];
            volatile uint8_t *a = (volatile uint8_t *)w + n;
            rr = r;
            w[0] = m; w[1] = ~m;
            do_ldl(out, a, &rr);  check("ldl", where, n, out[0], ref_ldl(m, r, n));
            do_ldr(out, a, &rr);  check("ldr", where, n, out[0], ref_ldr(m, r, n));
            check("ldl/ldr leave memory", where, n, w[0] ^ w[1], m ^ ~m);
            w[0] = m; w[1] = ~m;  do_sdl(a, &rr); check("sdl", where, n, w[0], ref_sdl(m, r, n)); check("sdl next", where, n, w[1], ~m);
            w[0] = m; w[1] = ~m;  do_sdr(a, &rr); check("sdr", where, n, w[0], ref_sdr(m, r, n)); check("sdr next", where, n, w[1], ~m);
            /* the doubleword before: untouched */
            w[-1] = 0x5555555555555555ull; w[0] = m; do_sdl(a, &rr); do_sdr(a, &rr);
            check("sdl, sdr before", where, n, w[-1], 0x5555555555555555ull);
            /* in a delay slot */
            w[0] = m; rr = r;
            do_slot(out, w, &rr, n);
            check("ldl in a slot", where, n, out[0], ref_ldl(m, r, n)); check("ldr in a slot", where, n, out[1], ref_ldr(m, r, n));
        }
    }
    (void)name;
}

/* sdl / sdr right behind a store to the same doubleword, and a load of it behind that: the cache's byte strobes against the write buffer's */
static void behind(const char *name, int where, volatile uint64_t *w)
{
    for (uint32_t n = 0; n < 8; n++) {
        uint64_t r = 0x1122334455667788ull ^ ((uint64_t)n * 0x0101010101010101ull), m = 0xCAFEF00DDEADBEEFull;
        volatile uint8_t *a = (volatile uint8_t *)w + n;
        rr = r;
        w[0] = m; w[1] = 0;
        ((volatile uint8_t *)w)[3] = 0x99;               /* an sb into the doubleword, then the sdl, then the ldr */
        m = (m & ~(0xFFull << 32)) | (0x99ull << 32);
        do_sdl(a, &rr);
        do_ldr(out, a, &rr);
        check("sb, sdl, ldr", where, n, w[0], ref_sdl(m, r, n));
        check("ldr behind sdl", where, n, out[0], ref_ldr(ref_sdl(m, r, n), r, n));
        w[0] = m;
        do_sdr(a, &rr);
        do_ldl(out, a, &rr);
        check("sdr, ldl", where, n, out[0], ref_ldl(ref_sdr(m, r, n), r, n));
    }
    (void)name;
}

/* a 24-byte move at every pair of alignments, as a compiler's block move for a packed structure does it */
static void block_moves(void)
{
    for (uint32_t s = 0; s < 8; s++) {
        for (uint32_t d = 0; d < 8; d++) {
            for (int i = 0; i < 48; i++) { src_b[i] = (uint8_t)(i * 7 + s * 3 + 1); dst_b[i] = 0xEE; want_b[i] = 0xEE; }
            for (int i = 0; i < 24; i++) want_b[8 + d + i] = src_b[8 + s + i];
            uint8_t *sp = src_b + 8 + s, *dp = dst_b + 8 + d;
            for (int i = 0; i < 24; i += 8) {
                __asm__ volatile(".set push\n.set mips3\n.set noreorder\n.set noat\n"
                    "ldl $8, 0(%0)\n ldr $8, 7(%0)\n sdl $8, 0(%1)\n sdr $8, 7(%1)\n.set pop\n"
                    : : "r"(sp + i), "r"(dp + i) : "$8", "memory");
            }
            for (int i = 0; i < 48; i++) if (dst_b[i] != want_b[i]) {
                if (++fails <= 20) { puts_("FAIL block move s "); putdec(s); puts_(" d "); putdec(d); puts_(" byte "); putdec(i); puts_("\n"); }
                break;
            }
        }
    }
}

int main(void)
{
    puts_("ualn_test\n");
    uint32_t m; __asm__ volatile("mfc0 %0, $7" : "=r"(m));
    if (!(m >> 27 & 1)) { puts_("ualn_test: SKIPPED (this core has no ldl / ldr / sdl / sdr)\n"); putc_(4); for (;;); }
    volatile uint64_t *k0 = (volatile uint64_t *)0x80000200u, *k1 = (volatile uint64_t *)0xA0000200u;
    one_mem("kseg0", 0, k0);
    one_mem("kseg1", 1, k1);
    one_mem("sdram", 2, buf_sd + 2);
    behind("kseg0", 0, k0);
    behind("kseg1", 1, k1);
    behind("sdram", 2, buf_sd + 2);
    if ((MFC0(23) & ~9u) == 0x54420000u) {                              /* a mapped page: the slow path, a translation first */
        volatile uint64_t *mp = (volatile uint64_t *)0x04000010u;
        tlb_map(0x04000000u, 0x00010000u);
        one_mem("mapped", 3, mp);
        behind("mapped", 3, mp);
    } else puts_("(no translation tables in this CPU: the mapped pages are skipped)\n");
    block_moves();
    /* what each costs, back to back on cached memory (Count ticks once per two CPU clocks), against a nop in its place */
    {
        volatile uint8_t *p = (volatile uint8_t *)k0 + 3;
        uint32_t t0 = count();
        for (int i = 0; i < 256; i++) __asm__ volatile("nop" ::: "memory");
        uint32_t base = count() - t0;
        static const char *const nm[4] = { "ldl", "ldr", "sdl", "sdr" };
        for (int k = 0; k < 4; k++) {
            t0 = count();
            for (int i = 0; i < 256; i++) {
                if (k == 0) __asm__ volatile(".set push\n.set mips3\n.set noat\nldl $8, 0(%0)\n.set pop" : : "r"(p) : "$8", "memory");
                else if (k == 1) __asm__ volatile(".set push\n.set mips3\n.set noat\nldr $8, 0(%0)\n.set pop" : : "r"(p) : "$8", "memory");
                else if (k == 2) __asm__ volatile(".set push\n.set mips3\n.set noat\nsdl $8, 0(%0)\n.set pop" : : "r"(p) : "$8", "memory");
                else __asm__ volatile(".set push\n.set mips3\n.set noat\nsdr $8, 0(%0)\n.set pop" : : "r"(p) : "$8", "memory");
            }
            puts_(nm[k]); puts_(": CPU clocks each, at byte 3 of a cached doubleword: "); putdec((count() - t0 - base) * 2 / 256); puts_("\n");
        }
    }
    puts_(fails ? "ualn_test: FAILED\n" : "ualn_test: PASS\n");
    putc_(4);
    for (;;);
}
