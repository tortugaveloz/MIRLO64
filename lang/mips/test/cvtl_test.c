/* The FPU's dmtc1 and cvt.s.l in hardware (mips_core / fpu_n64): a 64-bit register into an even/odd pair (FR=0), the
 * pair read back with mfc1, and the 64-bit integer converted to a float (round to nearest even), on corner values and
 * random ones against a table computed off-line (Python, exact integers: cvtl_vec.h).
 * Prints PASS or the failures on the UART, then a 0x04. ROM=<any file> obj/tb_soc build/cvtl_test.bin -c 40. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
#include "cvtl_vec.h"

int main(void)
{
    int fails = 0;
    uint32_t sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr |= 0x20000000u;                                   /* CU1 */
    __asm__ volatile("mtc0 %0, $12; nop; nop" :: "r"(sr));
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        uint32_t lo, hi, f, in[2] = { vec[i].h, vec[i].l };
        __asm__ volatile(
            ".set push; .set mips3; .set noreorder\n"
            "ld $8, 0(%3)\n"
            ".word 0x44A82000\n"                         /* dmtc1 $8, $f4 */
            "nop\n"
            ".word 0x44092000\n"                         /* mfc1 $9, $f4 */
            ".word 0x440A2800\n"                         /* mfc1 $10, $f5 */
            ".word 0x46A021A0\n"                         /* cvt.s.l $f6, $f4 */
            "nop\n"
            ".word 0x440B3000\n"                         /* mfc1 $11, $f6 */
            "move %0, $9; move %1, $10; move %2, $11\n"
            ".set pop\n"
            : "=r"(lo), "=r"(hi), "=r"(f) : "r"(in) : "$8", "$9", "$10", "$11", "memory");
        if (lo != vec[i].l || hi != vec[i].h || f != vec[i].f) {
            if (++fails <= 20) {
                puts_("FAIL #"); puthex(i); puts_(" in "); puthex(vec[i].h); puthex(vec[i].l);
                puts_(" pair "); puthex(hi); puthex(lo); puts_(" cvt "); puthex(f); puts_(" want "); puthex(vec[i].f); puts_("\n");
            }
        }
    }
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
