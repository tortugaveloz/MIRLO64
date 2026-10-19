/* ddiv of two words (32-bit operands, sign-extended) on the 32-bit divider (mips_core without W64_DIV): the 64-bit
 * quotient (LO) and remainder (HI), read with dmflo / dmfhi through sd, against a table computed off-line (Python,
 * exact integers: ddiv_vec.h), -2^31 / -1 (a quotient of 2^31) among them.
 * Prints PASS or the failures on the UART, then a 0x04. ROM=<any file> obj/tb_soc build/ddiv_test.bin -c 40. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
#include "ddiv_vec.h"

int main(void)
{
    int fails = 0;
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        uint32_t out[4];
        __asm__ volatile(
            ".set push; .set mips3; .set noreorder\n"
            "ddiv $0, %1, %2\n"
            "mflo $8\n"
            "mfhi $9\n"
            "sd $8, 0(%0)\n"
            "sd $9, 8(%0)\n"
            ".set pop\n"
            :: "r"(out), "r"(vec[i].a), "r"(vec[i].b) : "$8", "$9", "hi", "lo", "memory");
        if (out[0] != vec[i].qh || out[1] != vec[i].ql || out[2] != vec[i].rh || out[3] != vec[i].rl) {
            if (++fails <= 20) {
                puts_("FAIL #"); puthex(i); puts_(" q "); puthex(out[0]); puthex(out[1]); puts_(" r "); puthex(out[2]); puthex(out[3]);
                puts_(" want "); puthex(vec[i].qh); puthex(vec[i].ql); puts_(" "); puthex(vec[i].rh); puthex(vec[i].rl); puts_("\n");
            }
        }
    }
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
