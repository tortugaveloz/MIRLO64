/* PI DMA from both sides of the ROM split to RDRAM. Run with the
 * synthetic rom64-48split.bin image used by coverage_ps_test. */
#include <stdint.h>
#include <generated/csr.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void print_(const char *s) { while (*s) putc_(*s++); }
void trap_handler(void) { print_("ROM TAIL DMA TRAP\n"); for (;;) {} }
static int check(uint32_t offset, uint32_t expected)
{
    volatile uint32_t *dst = (volatile uint32_t *)0xa0030000u;
    *dst = 0;
    monitor_pidst_write(0x0030000u >> 2);
    uint32_t src = offset < 0x01e00000u ? (offset + 0x00200000u) : (offset - 0x01d00000u);
    monitor_pisrc_write((src >> 2) | ((offset < 0x01e00000u) ? (1u << 25) : 0));
    monitor_pilen_write(1);
    unsigned t = 0;
    while ((monitor_pilen_read() >> 31) && ++t < 4000000u) {}
    return t < 4000000u && *dst == expected;
}
int main(void)
{
    int ok = check(0x00000000u, 0x11223344u);
    ok &= check(0x00dffffcu, 0x81a2b3c4u);
    ok &= check(0x00e00000u, 0x92b3c4d5u);
    ok &= check(0x01dffffcu, 0x55667788u);
    ok &= check(0x01e00000u, 0x99aabbccu);
    ok &= check(0x03700000u, 0x89abcdefu);
    ok &= check(0x03fffffcu, 0xdeadbeefu);
    print_(ok ? "ROM TAIL DMA PASS\n" : "ROM TAIL DMA FAIL\n");
    putc_(4);
    for (;;) {}
}
