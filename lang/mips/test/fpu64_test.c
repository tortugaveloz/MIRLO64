/* The FPU's 64-bit moves and conversions as mips_core / fpu_n64 do them without FP64: dmtc1 then dmfc1 (a 64-bit
 * register through an even/odd pair and back), cvt.d.l (a 64-bit integer to a double, through a float), trunc.l.s (a
 * float to a 64-bit integer, as to a word, sign-extended), and c.eq.d, c.lt.d, c.le.d, which compare the doubles' own 64
 * bits (not their conversions to float: a game that compares 64-bit display-list commands as doubles to find the ones
 * that repeat took d7000002ffffffff for d7000000ffffffff, both out of a float's range). Prints PASS or the failures on
 * the UART, then a 0x04.
 * ROM=<any file> obj/tb_soc build/fpu64_test.bin -c 30. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *w, uint32_t gh, uint32_t gl, uint32_t wh, uint32_t wl)
{
    if (gh == wh && gl == wl) return;
    fails++; puts_("FAIL "); puts_(w); puts_(" got "); puthex(gh); puthex(gl); puts_(" want "); puthex(wh); puthex(wl); puts_("\n");
}
static const uint32_t mv[][2] __attribute__((aligned(8))) = { {0x01234567, 0x89abcdef}, {0, 0x80000000}, {0xffffffff, 0x7fffffff}, {0, 5}, {0xffffffff, 0xfffffffd} };

int main(void)
{
    uint32_t sr, o[2] __attribute__((aligned(8)));
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr |= 0x20000000u;
    __asm__ volatile("mtc0 %0, $12; nop; nop" :: "r"(sr));
    for (unsigned i = 0; i < 5; i++) {                   /* dmtc1 / dmfc1 */
        __asm__ volatile(".set push; .set mips3; .set noreorder\n"
            "ld $8, 0(%1)\n .word 0x44A82000\n nop\n .word 0x44292000\n sd $9, 0(%0)\n .set pop\n"
            :: "r"(o), "r"(mv[i]) : "$8", "$9", "memory");
        check("dmtc1/dmfc1", o[0], o[1], mv[i][0], mv[i][1]);
    }
    {                                                    /* cvt.d.l: 12345 and -7 */
        static const uint32_t in[2][2] __attribute__((aligned(8))) = { {0, 12345}, {0xffffffff, 0xfffffff9} };
        static const uint32_t want[2][2] = { {0x40C81C80, 0}, {0xC01C0000, 0} };
        for (int i = 0; i < 2; i++) {
            uint32_t h, l;
            __asm__ volatile(".set push; .set mips3; .set noreorder\n"
                "ld $8, 0(%2)\n .word 0x44A82000\n nop\n .word 0x46A021A1\n nop\n .word 0x440A3000\n .word 0x440B3800\n"
                "move %0, $11; move %1, $10\n .set pop\n"
                : "=r"(h), "=r"(l) : "r"(in[i]) : "$8", "$10", "$11", "memory");
            check("cvt.d.l", h, l, want[i][0], want[i][1]);
        }
    }
    {                                                    /* trunc.l.s: 5.5 and -3.0 */
        static const uint32_t f[2] = { 0x40B00000, 0xC0400000 };
        static const uint32_t want[2][2] = { {0, 5}, {0xffffffff, 0xfffffffd} };
        for (int i = 0; i < 2; i++) {
            uint32_t h, l;
            __asm__ volatile(".set push; .set noreorder\n"
                "move $8, %2\n .word 0x44882000\n nop\n .word 0x46002189\n nop\n .word 0x440A3000\n .word 0x440B3800\n"
                "move %0, $11; move %1, $10\n .set pop\n"
                : "=r"(h), "=r"(l) : "r"(f[i]) : "$8", "$10", "$11");
            check("trunc.l.s", h, l, want[i][0], want[i][1]);
        }
    }
    {                                                    /* c.eq.d, c.lt.d, c.le.d on doubles that a float cannot tell apart */
        static const uint32_t dp[][4] __attribute__((aligned(8))) = {      /* a (hi, lo), b (hi, lo) */
            {0xd7000000, 0xffffffff, 0xd7000002, 0xffffffff},   /* both below -2^128: a is above b */
            {0x3ff00000, 0x00000000, 0x3ff00000, 0x00000001},   /* one unit in the last place */
            {0x3ff00000, 0x10000000, 0x3ff00000, 0x10000001},
            {0x00000000, 0x00000000, 0x80000000, 0x00000000},   /* +0 and -0 */
            {0x7ff80000, 0x00000000, 0x3ff00000, 0x00000000},   /* a NaN */
            {0x40090000, 0x00000000, 0x40090000, 0x00000000},   /* equal */
            {0xc0090000, 0x00000000, 0x40090000, 0x00000000},   /* -3 and 3 */
            {0x7ff00000, 0x00000000, 0x47f00000, 0x00000000},   /* +inf and 2^128 */
        };
        /* eq, lt, le of (a, b) */
        static const unsigned char want[][3] = { {0,0,0}, {0,1,1}, {0,1,1}, {1,0,1}, {0,0,0}, {1,0,1}, {0,1,1}, {0,0,0} };
        for (unsigned i = 0; i < 8; i++) {
            uint32_t r[3];
            __asm__ volatile(".set push; .set mips3; .set noreorder\n"
                "ld $8, 0(%3)\n ld $9, 8(%3)\n .word 0x44A82000\n .word 0x44A93000\n nop\n nop\n"
                ".word 0x46262032\n nop\n nop\n .word 0x444AF800\n nop\n srl %0, $10, 23\n"        /* c.eq.d f4, f6; cfc1 */
                ".word 0x4626203C\n nop\n nop\n .word 0x444AF800\n nop\n srl %1, $10, 23\n"        /* c.lt.d */
                ".word 0x4626203E\n nop\n nop\n .word 0x444AF800\n nop\n srl %2, $10, 23\n .set pop\n"   /* c.le.d */
                : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]) : "r"(dp[i]) : "$8", "$9", "$10", "memory");
            check("c.eq.d", 0, r[0] & 1, 0, want[i][0]);
            check("c.lt.d", 0, r[1] & 1, 0, want[i][1]);
            check("c.le.d", 0, r[2] & 1, 0, want[i][2]);
        }
    }
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
