/* The game CPU's 64-bit addresses in the xkphys (mips_core's e_ea, sim/mips_soc): with Status.KX set a base register whose
 * upper word is 0x9000_0000 (CCA 2, uncached) or 0x9800_0000 (CCA 3, cached) and whose lower word is a physical address
 * below 0x2000_0000 reaches that address as KSEG1 or KSEG0 does -- how an open-source libdragon's io_read / io_write reach
 * the RCP's registers and the cartridge. Checked: a store through the uncached xkphys read back through KSEG1 and the
 * other way round (RDRAM), the loads and stores of every width, doublewords (ld, sd), an offset (positive and
 * negative), the cached view read through KSEG0, an RCP register (MI_VERSION) and the cartridge's first word (the PI)
 * against KSEG1's, and that an ordinary 32-bit address is unchanged beside them (a base whose upper word is that of a
 * 32-bit value is no xkphys address, with KX set or clear).
 * Prints PASS or the failures on the UART, then a 0x04 (the bench stops): ROM=<any file> obj_soc/tb_soc build/xkphys_test.bin -c 30. */
#include <stdint.h>
#include <generated/csr.h>
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

/* a load or a store through a 64-bit base register, made in the assembler (the C types here are 32 bits): the upper word U16 << 16
 * (0x9000 uncached, 0x9800 cached) in the register's top half, the physical address P zero-extended in its lower half, the access `op` at OFF from it */
#define XBASE(U16, P) ".set push\n.set mips3\n.set noreorder\n.set noat\n lui $8, %[u]\n dsll32 $8, $8, 0\n lui $9, %[ph]\n ori $9, $9, %[pl]\n or $8, $8, $9\n"
#define XIN(U16, P, OFF) [u] "i"(U16), [ph] "i"((P) >> 16), [pl] "i"((P) & 0xFFFF), [o] "i"(OFF)
#define XLD(op, r, U16, P, OFF) __asm__ volatile(XBASE(U16, P) " " op " %0, %[o]($8)\n nop\n.set pop\n" : "=r"(r) : XIN(U16, P, OFF) : "$8", "$9", "memory")
#define XST(op, v, U16, P, OFF) __asm__ volatile(XBASE(U16, P) " " op " %0, %[o]($8)\n nop\n.set pop\n" :: "r"(v), XIN(U16, P, OFF) : "$8", "$9", "memory")
#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })

int main(void)
{
    puts_("xkphys_test\n");
    uint32_t sr = MFC0(12), v;
    __asm__ volatile("mtc0 %0, $12\n nop\n nop\n nop" :: "r"(sr | 0x80u));          /* KX */
    volatile uint32_t *k1 = (volatile uint32_t *)0xA0000400u, *k0 = (volatile uint32_t *)0x80000400u;
    /* a store through the uncached xkphys, read through KSEG1; a store through KSEG1, read through the xkphys */
    k1[0] = 0; k1[1] = 0; k1[2] = 0; k1[3] = 0;
    XST("sw", 0x12345678u, 0x9000, 0x400u, 0);
    check("sw xk -> KSEG1", k1[0], 0x12345678u);
    k1[1] = 0x9ABCDEF0u;
    XLD("lw", v, 0x9000, 0x400u, 4); check("lw xk <- KSEG1", v, 0x9ABCDEF0u);
    XLD("lw", v, 0x9000, 0x400u, 0); check("lw xk, the first word", v, 0x12345678u);
    /* the other widths and the offsets */
    XLD("lbu", v, 0x9000, 0x400u, 1); check("lbu xk", v, 0x34u);
    XLD("lhu", v, 0x9000, 0x400u, 2); check("lhu xk", v, 0x5678u);
    XST("sb", 0xAAu, 0x9000, 0x400u, 3); check("sb xk", k1[0], 0x123456AAu);
    XST("sh", 0xBBCCu, 0x9000, 0x400u, 0); check("sh xk", k1[0], 0xBBCC56AAu);
    XLD("lw", v, 0x9000, 0x410u, -16); check("lw xk, a negative offset", v, 0xBBCC56AAu);
    XLD("lw", v, 0x9000, 0x3F0u, 16); check("lw xk, a positive offset", v, 0xBBCC56AAu);
    /* doublewords */
    {
        uint32_t hi, lo;
        __asm__ volatile(XBASE(0x9000, 0x400u) " li $10, 0x01234567\n dsll32 $10, $10, 0\n li $11, 0x89ABCDEF\n dsll32 $11, $11, 0\n dsrl32 $11, $11, 0\n"
                         " or $10, $10, $11\n sd $10, 8($8)\n nop\n ld $11, 8($8)\n nop\n dsrl32 %0, $11, 0\n sll %1, $11, 0\n.set pop\n"
                         : "=r"(hi), "=r"(lo) : XIN(0x9000, 0x400u, 0) : "$8", "$9", "$10", "$11", "memory");
        check("sd xk, the upper word", k1[2], 0x01234567u);
        check("sd xk, the lower word", k1[3], 0x89ABCDEFu);
        check("ld xk, the upper word", hi, 0x01234567u);
        check("ld xk, the lower word", lo, 0x89ABCDEFu);
    }
    /* the cached xkphys is KSEG0's: a store there is seen by a load of KSEG0 and the other way round */
    XST("sw", 0x0BADF00Du, 0x9800, 0x400u, 0x20);
    check("sw xk cached -> KSEG0", k0[8], 0x0BADF00Du);
    k0[9] = 0x600DCAFEu;
    XLD("lw", v, 0x9800, 0x400u, 0x24); check("lw xk cached <- KSEG0", v, 0x600DCAFEu);
    /* the RCP's registers and the cartridge, as the libdragon reaches them: MI_VERSION, the cartridge's first words, against KSEG1's */
    XLD("lw", v, 0x9000, 0x04300004u, 0); check("lw xk, MI_VERSION", v, *(volatile uint32_t *)0xA4300004u);
    XLD("lw", v, 0x9000, 0x10000000u, 0); check("lw xk, the cartridge's first word", v, *(volatile uint32_t *)0xB0000000u);
    XLD("lw", v, 0x9000, 0x10000040u, 0); check("lw xk, the cartridge, 0x40", v, *(volatile uint32_t *)0xB0000040u);
    /* an ordinary 32-bit address is as it was beside them */
    check("lw KSEG1 beside it", k1[0], 0xBBCC56AAu);
    puts_(fails ? "xkphys_test: FAILED\n" : "xkphys_test: PASS\n");
    putc_(4);
    for (;;);
}
