/* Observe a PI DMA overwrite of a line cached while the DMA is in progress. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }

int main(void)
{
    enum { SRC = 0x42000000u, DST = 0x00100000u, LEN = 1024u };
    volatile uint32_t *src = (volatile uint32_t *)(uintptr_t)SRC;
    volatile uint32_t *cached = (volatile uint32_t *)(uintptr_t)(0x80000000u + DST);
    volatile uint32_t *uncached = (volatile uint32_t *)(uintptr_t)(0xa0000000u + DST);
    for (unsigned i = 0; i < LEN / 4; ++i) { src[i] = 0x12340000u + i; uncached[i] = 0xdead0000u + i; }
    flush_cpu_dcache_range((const void *)cached, LEN);
    monitor_pidst_write((0x43800000u + DST - 0x40000000u) >> 2);
    monitor_pisrc_write((SRC - 0x40000000u) >> 2);
    monitor_pilen_write(LEN / 4);
    uint32_t before = cached[LEN / 4 - 1];
    /* Hit_Writeback_D must leave this clean WT line resident. */
    MIPS_CACHE_OP(0x19, &cached[LEN / 4 - 1]);
    for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
    if (monitor_pilen_read() >> 31 || before != 0xdead00ffu) { puts_("snoop_probe: SETUP FAIL\n"); goto done; }
    uint32_t tag;
    __asm__ volatile(".set push\n.set mips3\nmtc0 %1, $28\ncache 0x05, 0(%2)\nmfc0 %0, $28\n.set pop"
                     : "=r"(tag) : "r"(0x0fffffffu), "r"(cached) : "memory");
    if (tag != 0) { puts_("snoop_probe: dirty tag FAIL\n"); goto done; }
    uint32_t after = cached[LEN / 4 - 1];
    if (after == 0x123400ffu) puts_("snoop_probe: automatically coherent\n");
    else if (after == 0xdead00ffu) puts_("snoop_probe: stale before invalidation\n");
    else { puts_("snoop_probe: UNEXPECTED DATA\n"); goto done; }
    MIPS_CACHE_OP(0x11, &cached[LEN / 4 - 1]);
    puts_(cached[LEN / 4 - 1] == 0x123400ffu ? "snoop_probe: invalidate PASS\n" : "snoop_probe: invalidate FAIL\n");
done:
    putc_(4);
    for (;;) { }
}
