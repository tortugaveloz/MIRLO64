/* Status.FR as the VR4300 has it. FR set: 32 FPU registers of 64 bits -- a double in any register, the odd ones too, its
 * halves not a pair of registers (mfc1 of an odd register is that register's own lower word). FR clear: the 16 even / odd pairs,
 * the odd register of a pair its even one's upper word. Code compiled for one is wrong in the other: a loop that
 * compares a double in f0 with one in f1 ("c.le.d f0, f1": 1.0 against 0.5) never ends if the two are the same pair. Prints PASS or
 * the failures on the UART, then a 0x04. Also: a register is the same 64 bits in both modes.
 * ROM=<any file> obj/tb_soc build/fr1_test.bin -c 30. */
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
static void status(uint32_t fr)
{
    uint32_t sr;
    __asm__ volatile("mfc0 %0, $12" : "=r"(sr));
    sr = (sr & ~0x04000000u) | 0x20000000u | (fr ? 0x04000000u : 0);
    __asm__ volatile("mtc0 %0, $12; nop; nop; nop" :: "r"(sr));
}
/* doubles: a (hi, lo) as memory has them */
static uint32_t o[8] __attribute__((aligned(8)));
static void memset_o(void) { for (int i = 0; i < 8; i++) o[i] = 0xDEADBEEF; }
static const uint32_t half[2] __attribute__((aligned(8))) = { 0x3FE00000, 0x00000000 };         /* 0.5 */
static const uint32_t one[2] __attribute__((aligned(8))) = { 0x3FF00000, 0x00000000 };          /* 1.0 */
static const uint32_t pat[2] __attribute__((aligned(8))) = { 0x3FE00000, 0x12345678 };

int main(void)
{
    status(1);
    /* ldc1 f0, f1; c.le.d f0, f1 (FR=1): 1.0 <= 0.5 */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5000000  /* ldc1 f0, 0($8) */\n"
        ".word 0xD5210000  /* ldc1 f1, 0($9) */\n"
        "nop\n"
        "nop\n"
        ".word 0x4621003E  /* c.le.d f0, f1 */\n"
        "nop\n"
        "nop\n"
        ".word 0x444CF800  /* cfc1 $12, fcr31 */\n"
        "srl $12, $12, 23\n"
        "andi $12, $12, 1\n"
        "nop\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("ldc1 f0, f1; c.le.d f0, f1 (FR=1): 1.0 <= 0.5", 0, o[0], 0, 0x00000000);
    /* c.le.d f1, f0 (FR=1): 0.5 <= 1.0 */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5000000  /* ldc1 f0, 0($8) */\n"
        ".word 0xD5210000  /* ldc1 f1, 0($9) */\n"
        "nop\n"
        "nop\n"
        ".word 0x4620083E  /* c.le.d f1, f0 */\n"
        "nop\n"
        "nop\n"
        ".word 0x444CF800  /* cfc1 $12, fcr31 */\n"
        "srl $12, $12, 23\n"
        "andi $12, $12, 1\n"
        "nop\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("c.le.d f1, f0 (FR=1): 0.5 <= 1.0", 0, o[0], 0, 0x00000001);
    /* mul.d f3, f1, f1 (FR=1): 0.5 * 0.5, then f0 still 1.0 */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5000000  /* ldc1 f0, 0($8) */\n"
        ".word 0xD5210000  /* ldc1 f1, 0($9) */\n"
        "nop\n"
        "nop\n"
        ".word 0x462108C2  /* mul.d f3, f1, f1 */\n"
        "nop\n"
        "nop\n"
        ".word 0xF5430000  /* sdc1 f3, 0($10) */\n"
        ".word 0xF5400008  /* sdc1 f0, 8($10) */\n"
        "nop\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("mul.d f3, f1, f1 (FR=1): 0.5 * 0.5, then f0 still 1.0", 0, o[0], 0, 0x3FD00000);
    check("mul.d f3, f1, f1 (FR=1): 0.5 * 0.5, then f0 still 1.0", 0, o[1], 0, 0x00000000);
    check("mul.d f3, f1, f1 (FR=1): 0.5 * 0.5, then f0 still 1.0", 0, o[2], 0, 0x3FF00000);
    check("mul.d f3, f1, f1 (FR=1): 0.5 * 0.5, then f0 still 1.0", 0, o[3], 0, 0x00000000);
    /* ldc1 f1 (a pattern): mfc1 f1, dmfc1 f1, mfc1 f0 (FR=1) */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5000000  /* ldc1 f0, 0($8) */\n"
        ".word 0xD5610000  /* ldc1 f1, 0($11) */\n"
        "nop\n"
        "nop\n"
        ".word 0x440C0800  /* mfc1 $12, f1 */\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".word 0x442D0800  /* dmfc1 $13, f1 */\n"
        "nop\n"
        "dsra32 $14, $13, 0\n"
        "nop\n"
        ".word 0xAD4E0004  /* sw $14, 4($10) */\n"
        ".word 0x440C0000  /* mfc1 $12, f0 */\n"
        ".word 0xAD4C0008  /* sw $12, 8($10) */\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("ldc1 f1 (a pattern): mfc1 f1, dmfc1 f1, mfc1 f0 (FR=1)", 0, o[0], 0, 0x12345678);
    check("ldc1 f1 (a pattern): mfc1 f1, dmfc1 f1, mfc1 f0 (FR=1)", 0, o[1], 0, 0x3FE00000);
    check("ldc1 f1 (a pattern): mfc1 f1, dmfc1 f1, mfc1 f0 (FR=1)", 0, o[2], 0, 0x00000000);
    /* dmtc1 f5; mov.d f7, f5; cvt.s.d f9, f7; cvt.d.s f11, f9; add.d f13, f11, f11 (FR=1) */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xDD2C0000  /* ld $12, 0($9)  (0.5) */\n"
        "nop\n"
        ".word 0x44AC2800  /* dmtc1 $12, f5 */\n"
        "nop\n"
        "nop\n"
        ".word 0x462029C6  /* mov.d f7, f5 */\n"
        "nop\n"
        ".word 0x46203A60  /* cvt.s.d f9, f7 */\n"
        "nop\n"
        "nop\n"
        ".word 0x440D4800  /* mfc1 $13, f9 */\n"
        ".word 0xAD4D0000  /* sw $13, 0($10) */\n"
        ".word 0x46004AE1  /* cvt.d.s f11, f9 */\n"
        "nop\n"
        "nop\n"
        ".word 0x462B5B40  /* add.d f13, f11, f11 */\n"
        "nop\n"
        "nop\n"
        ".word 0xF54D0008  /* sdc1 f13, 8($10) */\n"
        "nop\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("dmtc1 f5; mov.d f7, f5; cvt.s.d f9, f7; cvt.d.s f11, f9; add.d f13, f11, f11 (FR=1)", 0, o[0], 0, 0x3F000000);
    check("dmtc1 f5; mov.d f7, f5; cvt.s.d f9, f7; cvt.d.s f11, f9; add.d f13, f11, f11 (FR=1)", 0, o[2], 0, 0x3FF00000);
    check("dmtc1 f5; mov.d f7, f5; cvt.s.d f9, f7; cvt.d.s f11, f9; add.d f13, f11, f11 (FR=1)", 0, o[3], 0, 0x00000000);
    status(0);
    /* FR=0: ldc1 f0; mfc1 f1 is the upper word, mfc1 f0 the lower */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5600000  /* ldc1 f0, 0($11) */\n"
        "nop\n"
        "nop\n"
        ".word 0x440C0800  /* mfc1 $12, f1 */\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".word 0x440C0000  /* mfc1 $12, f0 */\n"
        ".word 0xAD4C0004  /* sw $12, 4($10) */\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("FR=0: ldc1 f0; mfc1 f1 is the upper word, mfc1 f0 the lower", 0, o[0], 0, 0x3FE00000);
    check("FR=0: ldc1 f0; mfc1 f1 is the upper word, mfc1 f0 the lower", 0, o[1], 0, 0x12345678);
    /* FR=0: c.le.d f2, f4 (1.0 <= 0.5) */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $8, %1\n move $9, %2\n move $10, %0\n move $11, %3\n"
        ".word 0xD5020000  /* ldc1 f2, 0($8) */\n"
        ".word 0xD5240000  /* ldc1 f4, 0($9) */\n"
        "nop\n"
        "nop\n"
        ".word 0x4624103E  /* c.le.d f2, f4 */\n"
        "nop\n"
        "nop\n"
        ".word 0x444CF800  /* cfc1 $12, fcr31 */\n"
        "srl $12, $12, 23\n"
        "andi $12, $12, 1\n"
        "nop\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".set pop\n" :: "r"(o), "r"(one), "r"(half), "r"(pat) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory");
    check("FR=0: c.le.d f2, f4 (1.0 <= 0.5)", 0, o[0], 0, 0x00000000);
    /* the registers are the same 64 bits in both modes (the pair's odd register is the even one's upper word): a double loaded with FR clear
     * is a register with FR set, and one loaded with FR set is a pair with it clear */
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $10, %0\n move $11, %1\n"
        ".word 0xD5660000  /* ldc1 f6, 0($11) */\n"
        ".set pop\n" :: "r"(o), "r"(pat) : "$10", "$11", "memory");
    status(1);
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $10, %0\n"
        ".word 0x442C3000  /* dmfc1 $12, f6 */\n"
        ".word 0xFD4C0000  /* sd $12, 0($10) */\n"
        ".word 0x440C3000  /* mfc1 $12, f6 */\n"
        ".word 0xAD4C0008  /* sw $12, 8($10) */\n"
        ".set pop\n" :: "r"(o) : "$10", "$12", "memory");
    check("ldc1 f6 with FR=0; dmfc1 f6 and mfc1 f6 with FR=1", o[0], o[1], 0x3FE00000, 0x12345678);
    check("ldc1 f6 with FR=0; dmfc1 f6 and mfc1 f6 with FR=1", 0, o[2], 0, 0x12345678);
    memset_o();
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $10, %0\n move $11, %1\n"
        ".word 0xD5680000  /* ldc1 f8, 0($11) */\n"
        ".set pop\n" :: "r"(o), "r"(pat) : "$10", "$11", "memory");
    status(0);
    __asm__ volatile(".set push; .set noreorder; .set mips3\n"
        "move $10, %0\n"
        ".word 0x440C4800  /* mfc1 $12, f9 */\n"
        ".word 0xAD4C0000  /* sw $12, 0($10) */\n"
        ".word 0x440C4000  /* mfc1 $12, f8 */\n"
        ".word 0xAD4C0004  /* sw $12, 4($10) */\n"
        ".set pop\n" :: "r"(o) : "$10", "$12", "memory");
    check("ldc1 f8 with FR=1; mfc1 f9 and f8 with FR=0: the upper and the lower word", 0, o[0], 0, 0x3FE00000);
    check("ldc1 f8 with FR=1; mfc1 f9 and f8 with FR=0: the upper and the lower word", 0, o[1], 0, 0x12345678);
    status(1);
    puts_(fails ? "FAILED\n" : "PASS\n");
    putc_(4);
    for (;;);
}
