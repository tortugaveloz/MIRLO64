/* The caches' size and the cache operations at any of them (MIRLO_CPU_IC_LOG2, MIRLO_CPU_DC_LOG2: the VR4300's 16 KiB / 8 KiB
 * are 9 and 9; a bigger cache is a deliberate departure of the Enhanced build). Software written for the console's size must still
 * leave no stale line: an index op drops every line of its 16 KiB / 8 KiB index (the lines the console's cache would have held at
 * that index, wherever the bigger cache has them), a hit op the line of its address, and flush_cpu_[id]cache's loop over the
 * console's index range empties the cache. Over a span of 128 KiB: the lines left behind a write that bypassed the cache (the cache's
 * size, counted and printed), then each operation, and every word read or every stub run must be the new one. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putdec(unsigned v) { char b[12]; int n = 0; do b[n++] = '0' + v % 10; while (v /= 10); while (n) putc_(b[--n]); }
static int fails;
static void report(const char *name, unsigned bad)
{
    if (bad) { puts_("FAIL "); puts_(name); puts_(": "); putdec(bad); puts_(" stale\n"); ++fails; }
}
#define SPAN 0x20000u                       /* 128 KiB */
#define PHYS 0x00200000u                    /* the span's place in the RDRAM, its code's SPAN above it */
#define SRC  0x42000000u                    /* the PI's engine's source (SDRAM): what the engine writes over the span */
static volatile uint32_t *const dc = (volatile uint32_t *)(0x80000000u + PHYS);   /* the span, cached */
static volatile uint32_t *const du = (volatile uint32_t *)(0xa0000000u + PHYS);   /* uncached */
static volatile uint32_t *const sr = (volatile uint32_t *)SRC;
/* The only stale line is one that another master wrote behind (the CPU's own uncached store drops the line it shadows): the PI's
 * engine copies the source over `bytes` of the RDRAM at `off`, 4 KiB at a time. */
static void dma(uint32_t off, uint32_t bytes)
{
    for (uint32_t o = 0; o < bytes; o += 4096u) {
        monitor_pidst_write((0x43800000u + PHYS + off + o - 0x40000000u) >> 2);
        monitor_pisrc_write((SRC + off + o - 0x40000000u) >> 2);
        monitor_pilen_write(1024);
        for (unsigned n = 0; n < 4000000u && (monitor_pilen_read() >> 31); ++n) { }
    }
}
/* D: the span written (uncached, v), read through the cache (every line in), then v2 put over it by the engine: the lines left are stale */
static void dset(volatile uint32_t *p, uint32_t v) { for (unsigned i = 0; i < SPAN / 4; ++i) p[i] = v + i; }
static void dread(void) { volatile uint32_t s = 0; for (unsigned i = 0; i < SPAN / 4; i += 4) s += dc[i]; (void)s; }
/* (read downwards: a cache of one way keeps the line of the highest address of an index, and a read of a lower one would evict a stale line
 * before it was looked at) */
static unsigned dstale(uint32_t v) { unsigned n = 0; for (unsigned i = SPAN / 4; i; i -= 4) n += dc[i - 4] != v + i - 4; return n; }
static unsigned dold(uint32_t v) { unsigned n = 0; for (unsigned i = SPAN / 4; i; i -= 4) n += dc[i - 4] == v + i - 4; return n; }
static void dprep(uint32_t v1, uint32_t v2) { dset(du, v1); dread(); dset(sr, v2); dma(0, SPAN); }

/* I: a stub a line (32 bytes) that returns a constant, at the same places in the span's upper half-neighbour (the span above) */
typedef uint32_t (*fn_t)(void);
static void stubs(volatile uint32_t *u, uint32_t v)
{
    for (unsigned k = 0; k < SPAN / 32; ++k) {
        uint32_t x = v + k;
        u[k * 8 + 0] = 0x3c020000u | (x >> 16);         /* lui v0, hi */
        u[k * 8 + 1] = 0x03e00008u;                     /* jr ra */
        u[k * 8 + 2] = 0x34420000u | (x & 0xffffu);     /* ori v0, v0, lo (the delay slot) */
        u[k * 8 + 3] = 0; u[k * 8 + 4] = 0; u[k * 8 + 5] = 0; u[k * 8 + 6] = 0; u[k * 8 + 7] = 0;
    }
}
static unsigned runstubs(uint32_t v)
{
    unsigned n = 0;
    for (unsigned k = SPAN / 32; k--; ) n += ((fn_t)(uintptr_t)(0x80000000u + PHYS + SPAN + k * 32))() != v + k;       /* (downwards, as dstale) */
    return n;
}
static void iprep(uint32_t v1, uint32_t v2)
{
    stubs(du + SPAN / 4, v1);                                /* the code in, every line fetched (upwards: the highest of an index stays) */
    for (unsigned k = 0; k < SPAN / 32; ++k) ((fn_t)(uintptr_t)(0x80000000u + PHYS + SPAN + k * 32))();
    stubs(sr + SPAN / 4, v2); dma(SPAN, SPAN);               /* the engine writes new code over it */
}

int main(void)
{
    unsigned n;
    /* D: how many lines the cache holds behind the engine's write (its size, counted over the span) */
    flush_cpu_dcache();
    dprep(0x10000000u, 0x20000000u);
    n = dold(0x10000000u);
    puts_("D-cache: "); putdec(n * 16 / 1024); puts_(" KiB of the span stale behind the engine's write\n");
    flush_cpu_dcache();                                       /* the console's loop: 8 KiB of index */
    report("Index_Writeback_Inv_D over the console's 8 KiB", dstale(0x20000000u));
    dprep(0x30000000u, 0x40000000u);
    for (unsigned a = 0; a < SPAN; a += 16) MIPS_CACHE_OP(0x11, (uintptr_t)dc + a);
    report("Hit_Invalidate_D on every line", dstale(0x40000000u));
    dprep(0x50000000u, 0x60000000u);                          /* one index op: its index, in every 8 KiB of the span */
    MIPS_CACHE_OP(0x01, 0x40000000u + 0x120);
    n = 0; for (unsigned k = SPAN / 8192; k--; ) n += dc[(k * 8192 + 0x120) / 4] != 0x60000000u + (k * 8192 + 0x120) / 4;
    report("one Index_Writeback_Inv_D drops its index in every 8 KiB of the span", n);
    /* I */
    flush_cpu_icache();
    iprep(0x11000000u, 0x22000000u);
    n = SPAN / 32 - runstubs(0x11000000u);
    puts_("I-cache: "); putdec(n * 32 / 1024); puts_(" KiB of the span stale behind the engine's write\n");
    flush_cpu_icache();                                       /* the console's loop: 16 KiB of index */
    report("Index_Invalidate_I over the console's 16 KiB", runstubs(0x22000000u));
    iprep(0x33000000u, 0x44000000u);
    for (unsigned a = 0; a < SPAN; a += 32) MIPS_CACHE_OP(0x10, 0x80000000u + PHYS + SPAN + a);       /* Hit_Invalidate_I */
    report("Hit_Invalidate_I on every line", runstubs(0x44000000u));
    iprep(0x55000000u, 0x66000000u);                          /* one index op: its index, in every 16 KiB of the span */
    MIPS_CACHE_OP(0x00, 0x40000000u + 0x340);
    n = 0; for (unsigned k = SPAN / 16384; k--; ) {
        unsigned i = (k * 16384 + 0x340) / 32;
        n += ((fn_t)(uintptr_t)(0x80000000u + PHYS + SPAN + i * 32))() != 0x66000000u + i;
    }
    report("one Index_Invalidate_I drops its index in every 16 KiB of the span", n);
    puts_(fails ? "cache_size_test: FAIL\n" : "cache_size_test: PASS\n");
    putc_(4);
    for (;;);
}
