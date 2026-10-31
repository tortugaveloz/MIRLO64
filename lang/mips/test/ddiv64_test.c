/* ddiv and ddivu on 64-bit operands (mips_core with W64_DIV), and div and divu: the quotient (LO) and the remainder (HI), read with
 * mflo / mfhi through sd, against a table computed off-line (ddiv64_vec.py, exact integers: ddiv64_vec.h) -- a divisor of zero,
 * -2^63 / -1 and the signs of the remainder among them. Each pair is divided twice, once back to back with the divide before
 * (mflo right behind), once with the quotient and the remainder moved to the other register first (mthi, mtlo, then read).
 * Prints PASS or the failures on the UART, then a 0x04. ROM=<any file> obj/tb_soc build/ddiv64_test.bin -c 40. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
#include "ddiv64_vec.h"

int main(void)
{
    int fails = 0;
    static uint32_t o[16] __attribute__((aligned(8)));
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        for (int k = 0; k < 16; k++) o[k] = 0;
        __asm__ volatile(".set push; .set mips3; .set noreorder\n"
            "ld $8, 0(%1)\n ld $9, 8(%1)\n"
            "ddiv $0, $8, $9\n mflo $10\n mfhi $11\n sd $10, 0(%0)\n sd $11, 8(%0)\n"
            "ddivu $0, $8, $9\n mflo $10\n mfhi $11\n sd $10, 16(%0)\n sd $11, 24(%0)\n"
            /* (the divides one behind the other, HI and LO read after the second; mthi / mtlo between) */
            "ddiv $0, $8, $9\n ddivu $0, $8, $9\n mflo $10\n mfhi $11\n sd $10, 32(%0)\n sd $11, 40(%0)\n"
            "ddiv $0, $8, $9\n mflo $10\n mfhi $11\n mtlo $11\n mthi $10\n mflo $10\n mfhi $11\n sd $10, 48(%0)\n sd $11, 56(%0)\n"
            ".set pop\n" :: "r"(o), "r"(&vec[i]) : "$8", "$9", "$10", "$11", "hi", "lo", "memory");
        for (int k = 0; k < 16; k++) {
            uint32_t want = k < 4 ? vec[i].sq[k] : k < 8 ? vec[i].uq[k - 4] : k < 12 ? vec[i].uq[k - 8]
                          : k == 12 ? vec[i].sq[2] : k == 13 ? vec[i].sq[3] : k == 14 ? vec[i].sq[0] : vec[i].sq[1];
            if (o[k] != want) { if (++fails <= 20) { puts_("FAIL ddiv #"); puthex(i); puts_(" word "); puthex(k); puts_(" got "); puthex(o[k]); puts_(" want "); puthex(want); puts_("\n"); } break; }
        }
        /* the 32-bit forms, on the lower words (as lw gives them: sign-extended) */
        {
            uint32_t w[4] = { 0, 0, 0, 0 };
            __asm__ volatile(".set push; .set mips3; .set noreorder\n"
                "lw $8, 4(%1)\n lw $9, 12(%1)\n"
                "div $0, $8, $9\n mflo $10\n mfhi $11\n sw $10, 0(%0)\n sw $11, 4(%0)\n"
                "divu $0, $8, $9\n mflo $10\n mfhi $11\n sw $10, 8(%0)\n sw $11, 12(%0)\n"
                ".set pop\n" :: "r"(w), "r"(&vec[i]) : "$8", "$9", "$10", "$11", "hi", "lo", "memory");
            if (w[0] != vec[i].d[0] || w[1] != vec[i].d[1] || w[2] != vec[i].du[0] || w[3] != vec[i].du[1]) {
                if (++fails <= 20) { puts_("FAIL div #"); puthex(i); puts_(" got "); puthex(w[0]); puthex(w[1]); puthex(w[2]); puthex(w[3]); puts_("\n"); }
            }
        }
    }
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
