/* The FPU's 64-bit moves and conversions as mips_core / fpu_n64 do them without FP64: dmtc1 then dmfc1 (a 64-bit
 * register through an even/odd pair and back), cvt.d.l (a 64-bit integer to a double, through a float), trunc.l.s (a
 * float to a 64-bit integer, as to a word, sign-extended). Prints PASS or the failures on the UART, then a 0x04.
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
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
