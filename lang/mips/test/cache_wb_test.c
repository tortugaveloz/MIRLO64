/* Write-back D-cache operations against uncached RDRAM, on the SoC. */
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
#define CACHE(op, p) __asm__ volatile(".set push\n.set mips3\ncache " #op ", 0(%0)\n.set pop" :: "r"(p) : "memory")

int main(void)
{
    volatile uint32_t *a = (volatile uint32_t *)0x80020000u;
    volatile uint32_t *b = (volatile uint32_t *)0x80028000u;
    volatile uint32_t *ua = (volatile uint32_t *)0xa0020000u;
    volatile uint32_t *ub = (volatile uint32_t *)0xa0028000u;
    puts_("cache_wb_test\n");
    *ua = 0x12345678; *ub = 0xabcdef01;
    CACHE(0x11, a); CACHE(0x11, b);
    check("initial fill", *a, 0x12345678);
    *a = 0x87654321;
    check("write stays cached", *ua, 0x12345678);
    CACHE(0x19, a);
    check("hit writeback", *ua, 0x87654321);
    check("writeback retains line", *a, 0x87654321);
    *a = 0x10203040;
    CACHE(0x01, a);
    check("index writeback", *ua, 0x10203040);
    *ua = 0x55667788;
    check("index invalidates", *a, 0x55667788);
    *a = 0xfedcba98;
    check("conflicting fill", *b, 0xabcdef01);
    check("dirty eviction", *ua, 0xfedcba98);
    *b = 0x11223344;
    CACHE(0x15, b);
    check("hit writeback invalidate", *ub, 0x11223344);
    *ub = 0xaabbccdd;
    check("hit invalidated", *b, 0xaabbccdd);
    a[0] = 0x41424344;
    flush_cpu_dcache_range((const void *)&a[4], 16);
    check("partial invalidate preserves neighbor", ua[0], 0x41424344);
    puts_(fails ? "cache_wb_test: FAIL\n" : "cache_wb_test: PASS\n");
    putc_(4);
    for (;;);
}
