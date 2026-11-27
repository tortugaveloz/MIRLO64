/* MI's repeat mode (rtl/soc/rcp_mi.sv, cpu_port.sv), as an open-source libdragon's hardware memset and its start-up self test use it: MI_MODE
 * written with bit 8 (set init mode) and a count of n - 1 in bits 6:0, then ONE uncached doubleword store to the RDRAM, which goes over n bytes
 * (the doubleword's two words alternately); the mode ends with that store. Checked: n = 16 (the self test's), 128 (the most), 8 (one doubleword),
 * 24 and 40; the memory beside the run untouched; the mode over after it (INIT clear in MI_MODE, a next store a store as ever); the mode cleared
 * by a write of bit 7 before any store; a word store at an odd address ends the mode and is no repeat.
 * Prints PASS or the failures on the UART, then a 0x04 (the bench stops): ROM=<any file> obj_soc/tb_soc build/mirepeat_test.bin -c 30. */
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
#define MI_MODE ((volatile uint32_t *)0xA4300000u)
#define MEM ((volatile uint32_t *)0xA0004000u)          /* RDRAM 0x4000, uncached */

/* MI_MODE written with `mode`, then (nothing between them: any store the CPU makes in between is the one the mode repeats) one uncached
 * doubleword store of hi:lo at MEM + off bytes; with `mode2` != 0 a second write of MI_MODE between them */
static void rsd(uint32_t mode, uint32_t mode2, uint32_t off, uint32_t hi, uint32_t lo)
{
    __asm__ volatile(".set push\n.set mips3\n.set noreorder\n.set noat\n dsll32 $8, %3, 0\n dsll32 $9, %4, 0\n dsrl32 $9, $9, 0\n or $8, $8, $9\n"
                     " sw %1, 0(%0)\n beqz %5, 1f\n nop\n sw %5, 0(%0)\n1: sd $8, 0(%2)\n nop\n.set pop\n"
                     :: "r"(MI_MODE), "r"(mode), "r"((uintptr_t)MEM + off), "r"(hi), "r"(lo), "r"(mode2) : "$8", "$9", "memory");
}
/* the same with a word store at MEM + off in place of the doubleword's */
static void rsw(uint32_t mode, uint32_t off, uint32_t v)
{
    __asm__ volatile(" sw %1, 0(%0)\n sw %3, 0(%2)\n" :: "r"(MI_MODE), "r"(mode), "r"((uintptr_t)MEM + off), "r"(v) : "memory");
}
static void fill(uint32_t v) { for (int i = 0; i < 64; i++) MEM[i] = v; }
/* the words that are the pattern, over n bytes from off: the number */
static uint32_t pattern_words(uint32_t off, uint32_t n, uint32_t hi, uint32_t lo)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n / 4; i++) if (MEM[off / 4 + i] == ((i & 1) ? lo : hi)) c++;
    return c;
}
static uint32_t untouched(uint32_t from, uint32_t to, uint32_t v) { uint32_t c = 0; for (uint32_t i = from / 4; i < to / 4; i++) if (MEM[i] == v) c++; return c; }

int main(void)
{
    puts_("mirepeat_test\n");
    static const uint32_t ns[] = { 16, 128, 8, 24, 40 };
    for (int k = 0; k < 5; k++) {
        const uint32_t n = ns[k], off = 32;
        fill(0x5A5A5A5Au);
        rsd(0x100u | (n - 1), 0, off, 0x01234567u, 0x89ABCDEFu);
        check("the run's words", pattern_words(off, n, 0x01234567u, 0x89ABCDEFu), n / 4);
        check("before the run", untouched(0, off, 0x5A5A5A5Au), off / 4);
        check("after the run", untouched(off + n, 256, 0x5A5A5A5Au), (256 - off - n) / 4);
        check("MI_MODE: init cleared by the store", *MI_MODE & 0x80u, 0u);
        rsd(0x7Fu, 0, off + n, 0xAAAAAAAAu, 0xBBBBBBBBu);                                  /* a store as ever */
        check("the next store: its words", MEM[(off + n) / 4] == 0xAAAAAAAAu && MEM[(off + n) / 4 + 1] == 0xBBBBBBBBu, 1u);
        check("the next store: no more", untouched(off + n + 8, 256, 0x5A5A5A5Au), (256 - off - n - 8) / 4);
    }
    /* cleared before the store: no repeat */
    fill(0x5A5A5A5Au);
    rsd(0x100u | 31u, 0x80u, 32, 0x11111111u, 0x22222222u);
    check("init cleared by bit 7: one doubleword", untouched(32 + 8, 256, 0x5A5A5A5Au), (256 - 40) / 4);
    /* a word at an odd address first: the mode ends, the word is stored as ever */
    fill(0x5A5A5A5Au);
    rsw(0x100u | 31u, 36, 0x33333333u);
    check("an odd word: stored", MEM[9], 0x33333333u);
    check("an odd word: the mode is over", *MI_MODE & 0x80u, 0u);
    check("an odd word: nothing repeated", untouched(0, 256, 0x5A5A5A5Au), 63u);
    puts_(fails ? "mirepeat_test: FAILED\n" : "mirepeat_test: PASS\n");
    putc_(4);
    for (;;);
}
