/* Hit invalidate through a mapped alias whose physical page has another colour. On the translation tables' CPU the mapped load
 * is cached (its line is stale after a DMA until the hit-invalidate drops it); on the VR4300's own TLB it is not (a load through
 * a mapped page goes to the memory), so there what is checked is that no load through the page is ever stale -- not after a DMA,
 * not after a hit-invalidate through the alias, and not behind a store of the CPU's next to a partial DMA. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)) : "memory")
static void check(const char *what, uint32_t got, uint32_t want, int *bad)
{
    if (got == want) return;
    ++*bad;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}
static void dma(uint32_t pa, uint32_t src)
{
    monitor_pidst_write((0x43800000u + pa - 0x40000000u) >> 2);
    monitor_pisrc_write((src - 0x40000000u) >> 2);
    monitor_pilen_write(1);
    for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
}
int main(void)
{
    enum { VA = 0x04000000u, PA = 0x00105000u, SRC = 0x42000000u };
    volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)VA;
    volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)(0xa0000000u + PA);
    volatile uint32_t *src = (volatile uint32_t *)(uintptr_t)SRC;
    *u = 0x11112222u; *src = 0x33334444u;
    {   /* the VR4300's own TLB (COP0 23: 'TLB', bit 1): the page by tlbwi; its loads are not cached */
        uint32_t c23; __asm__ volatile("mfc0 %0, $23" : "=r"(c23));
        if ((c23 >> 8) == 0x544C42u && (c23 & 2u)) {
            int bad = 0;
            /* entry 0: VA's pair, the even page onto PA (cacheable, valid, dirty, global), the odd page invalid; Index 0, PageMask 0 */
            MTC0(0, 0); MTC0(5, 0); MTC0(10, VA);
            MTC0(2, ((PA >> 12) << 6) | (3u << 3) | 2u | 4u | 1u); MTC0(3, 1u);
            __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop" ::: "memory");
            MTC0(10, 0);
            flush_cpu_dcache();
            check("the page's word", *v, 0x11112222u, &bad);
            dma(PA, SRC);
            check("after a DMA over it", *v, 0x33334444u, &bad);
            MIPS_CACHE_OP(0x11, v);
            check("after a hit-invalidate through the alias", *v, 0x33334444u, &bad);
            *src = 0x55556666u;
            dma(PA, SRC);
            check("after a second DMA", *v, 0x55556666u, &bad);
            MIPS_CACHE_OP(0x15, v);
            check("after a hit-writeback-invalidate", *v, 0x55556666u, &bad);
            volatile uint32_t *k0 = (volatile uint32_t *)(uintptr_t)(0x80000000u + PA);
            k0[1] = 0xabcdef01u; *src = 0x778899aau;
            MIPS_CACHE_OP(0x11, v);
            dma(PA, SRC);
            check("the neighbour of a partial DMA", u[1], 0xabcdef01u, &bad);
            check("the DMA's word", *v, 0x778899aau, &bad);
            *v = 0x0badf00du;                      /* a store through the page reaches the memory (and KSEG1 sees it) */
            check("a store through the page", *u, 0x0badf00du, &bad);
            check("the DMA is done", monitor_pilen_read() >> 31, 0, &bad);
            puts_(bad ? "mapped_cache_test: FAIL\n" : "mapped_cache_test: PASS\n");
            putc_(4); for (;;) { }
        }
    }
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
