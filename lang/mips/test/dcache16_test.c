/* The two 16-byte halves of an SDRAM burst remain separate WT-NWA lines. */
#include <stdint.h>
#include <generated/csr.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static int fails;
static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) { puts_("FAIL "); puts_(name); puts_("\n"); ++fails; }
}

int main(void)
{
    volatile uint32_t *c = (volatile uint32_t *)0x80020000u;
    volatile uint32_t *u = (volatile uint32_t *)0xa0020000u;
    flush_cpu_dcache();
    for (unsigned i = 0; i < 8; ++i) u[i] = 0x10203000u + i;
    for (unsigned i = 0; i < 8; ++i) check("fill each half", c[i], 0x10203000u + i);
    c[1] = 0x11112222u; c[5] = 0x33334444u;
    check("WT first half", u[1], 0x11112222u);
    check("WT second half", u[5], 0x33334444u);
    MIPS_CACHE_OP(0x11, &c[4]);
    u[5] = 0x55556666u;
    check("invalidate second half", c[5], 0x55556666u);
    check("first half preserved", c[1], 0x11112222u);
    MIPS_CACHE_OP(0x11, c);
    u[1] = 0x77778888u;
    check("invalidate first half", c[1], 0x77778888u);
    check("second half preserved", c[5], 0x55556666u);
    puts_(fails ? "dcache16_test: FAIL\n" : "dcache16_test: PASS\n");
    putc_(4);
    for (;;);
}
