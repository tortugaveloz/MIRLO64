/* The FPU's double precision (mips_core's FPD64, fpu_d64): add.d, sub.d, mul.d, abs.d, neg.d and c.cond.d on the pairs of
 * fpu_d64_vec.h (fpu_d64_vec.py, exact integers: the specials in every pair, ties, cancellation, carry-out, overflow to
 * infinity, results and inputs below the normals, signed zeros, NaNs, a product with 0 and with infinity), each result
 * compared with the table's. Without FPD64 the FPU computes doubles as floats, and this fails (COP0 register 7's bit 26 says).
 * Prints PASS or the failures on the UART, then a 0x04. ROM=<any file> obj/tb_soc build/fpu_d64_test.bin -c 30.
 * -DLOCKSTEP: a program for the CPU's lockstep bench (sim/mips/fpd64_rom.sh): the number of failures in `fails`. */
#include <stdint.h>
#ifndef LOCKSTEP
#include <generated/csr.h>
#include <system.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
#endif
#include "fpu_d64_vec.h"

volatile uint32_t fails, done;

/* COP1 double: fmt 0x11, ft, fs, fd, function; ldc1 / sdc1 f, off($8); c.cond.d; cfc1 $9, $31; the FPU's registers are f4 f6 f8 */
#define FOP(fn, fd, fs, ft) (0x46200000u | (ft) << 16 | (fs) << 11 | (fd) << 6 | (fn))
#define LDC1(f, off) (0xD5000000u | (f) << 16 | (off))
#define SDC1(f, off) (0xF5000000u | (f) << 16 | (off))
#define CFC1_31 0x4449F800u

/* buf: x, y, and where the result goes */
static uint64_t run(unsigned fn, uint64_t *buf, uint32_t *c)
{
    uint32_t cc = 0;
    switch (fn) {
#define CASE(F) case F: __asm__ volatile(".set push\n.set noreorder\n.set noat\n.set mips3\n move $8, %1\n .word %2\n .word %3\n nop\n nop\n .word %4\n nop\n nop\n .word %5\n nop\n nop\n .set pop\n" \
        : "+m"(*(uint64_t (*)[3])buf) : "r"(buf), "i"(LDC1(4, 0)), "i"(LDC1(6, 8)), "i"(FOP(F, 8, 4, 6)), "i"(SDC1(8, 16)) : "$8", "memory"); break;
    CASE(0x00) CASE(0x01) CASE(0x02) CASE(0x05) CASE(0x07)
#undef CASE
#define CMP(F) case F: __asm__ volatile(".set push\n.set noreorder\n.set noat\n.set mips3\n move $8, %1\n .word %2\n .word %3\n nop\n nop\n .word %4\n nop\n nop\n .word %5\n nop\n nop\n move %0, $9\n .set pop\n" \
        : "=r"(cc) : "r"(buf), "i"(LDC1(4, 0)), "i"(LDC1(6, 8)), "i"(FOP(F, 0, 4, 6)), "i"(CFC1_31) : "$8", "$9", "memory"); break;
    CMP(0x30) CMP(0x31) CMP(0x32) CMP(0x33) CMP(0x34) CMP(0x35) CMP(0x36) CMP(0x37)
    CMP(0x38) CMP(0x39) CMP(0x3A) CMP(0x3B) CMP(0x3C) CMP(0x3D) CMP(0x3E) CMP(0x3F)
#undef CMP
    default: break;
    }
    *c = cc;
    return buf[2];
}

int main(void)
{
    uint32_t sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr |= 0x20000000u;                                   /* CU1 */
    __asm__ volatile("mtc0 %0, $12; nop; nop" :: "r"(sr));
    for (unsigned i = 0; i < sizeof vec / sizeof vec[0]; i++) {
        uint64_t in[3] __attribute__((aligned(8))) = { vec[i].x, vec[i].y, 0 };
        uint32_t c;
        uint64_t got = run(vec[i].fn, in, &c);
        if (vec[i].fn >= 0x30) got = (c >> 23) & 1;
        if (got != vec[i].want) {
            fails++;
#ifndef LOCKSTEP
            if (fails <= 20) {
                puts_("FAIL #"); puthex(i); puts_(" fn "); puthex(vec[i].fn); puts_(" x "); puthex(vec[i].x >> 32); puthex((uint32_t)vec[i].x);
                puts_(" y "); puthex(vec[i].y >> 32); puthex((uint32_t)vec[i].y); puts_(" got "); puthex(got >> 32); puthex((uint32_t)got);
                puts_(" want "); puthex(vec[i].want >> 32); puthex((uint32_t)vec[i].want); puts_("\n");
            }
#endif
        }
    }
    done = 0x600DC0DEu;
#ifndef LOCKSTEP
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
#endif
    for (;;);
}

#ifdef LOCKSTEP
__asm__(
"    .section .text.start,\"ax\"\n"
"    .set noreorder\n"
"    .globl _start\n"
"_start:\n"
"    lui   $sp, 0x800F\n"
"    ori   $sp, $sp, 0xF000\n"
"    jal   main\n"
"    nop\n"
"1:  b     1b\n"
"    nop\n"
"    .set reorder\n"
"    .text\n");
#endif
