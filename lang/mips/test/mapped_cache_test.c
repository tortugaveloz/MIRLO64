/* Hit invalidate through a mapped alias whose physical page has another colour. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)) : "memory")
int main(void)
{
    enum { VA = 0x04000000u, PA = 0x00105000u, SRC = 0x42000000u };
    volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)VA;
    volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)(0xa0000000u + PA);
    volatile uint32_t *src = (volatile uint32_t *)(uintptr_t)SRC;
    *u = 0x11112222u; *src = 0x33334444u;
    MTC0(10, VA); MTC0(23, PA | 1u | 2u | 4u | 32u); MTC0(10, 0);
    flush_cpu_dcache();
    uint32_t before = *v;
    monitor_pidst_write((0x43800000u + PA - 0x40000000u) >> 2);
    monitor_pisrc_write((SRC - 0x40000000u) >> 2);
    monitor_pilen_write(1);
    for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
    uint32_t stale = *v;
    MIPS_CACHE_OP(0x11, v);
    uint32_t after = *v;
    *src = 0x55556666u;
    monitor_pidst_write((0x43800000u + PA - 0x40000000u) >> 2);
    monitor_pisrc_write((SRC - 0x40000000u) >> 2);
    monitor_pilen_write(1);
    for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
    uint32_t stale2 = *v;
    MIPS_CACHE_OP(0x15, v);
    uint32_t after2 = *v;
    /* A CPU store adjacent to a partial DMA must reach memory first. */
    volatile uint32_t *k0 = (volatile uint32_t *)(uintptr_t)(0x80000000u + PA);
    k0[1] = 0xabcdef01u; *src = 0x778899aau;
    MIPS_CACHE_OP(0x11, v);
    monitor_pidst_write((0x43800000u + PA - 0x40000000u) >> 2);
    monitor_pisrc_write((SRC - 0x40000000u) >> 2);
    monitor_pilen_write(1);
    for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
    uint32_t neighbour = u[1], result = *v;
    puts_(before == 0x11112222u && stale == before && after == 0x33334444u &&
          stale2 == after && after2 == 0x55556666u && neighbour == 0xabcdef01u &&
          result == 0x778899aau && !(monitor_pilen_read() >> 31)
          ? "mapped_cache_test: PASS\n" : "mapped_cache_test: FAIL\n");
    putc_(4); for (;;) { }
}
