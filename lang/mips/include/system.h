/* System interface for MIRLO's MIPS game CPU (rtl/mips/mips_core.sv).
 * The D-cache is write-back. Hit writeback keeps a line valid; hit invalidate
 * drops it for a DMA destination. Index writeback-invalidate also writes back
 * dirty lines before dropping them. The runtime's all-cache writeback walks
 * the CPU's 64 MiB flat cached SDRAM window with hit operations. */
#ifndef __SYSTEM_H
#define __SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#define MIPS_ICACHE_BYTES 16384u
#define MIPS_DCACHE_BYTES 8192u
#define MIPS_LINE_BYTES   16u
#define MIPS_ICACHE_LINE_BYTES 32u
#define MIRLO_CACHEABLE_BASE  0x40000000u
#define MIRLO_CACHEABLE_BYTES 0x04000000u

/* CACHE op on the line at `a`: index operations select by index; hit
 * operations also compare the physical tag. */
#define MIPS_CACHE_OP(op, a) __asm__ volatile(".set push\n.set mips3\ncache %0, 0(%1)\n.set pop" :: "i"(op), "r"(a) : "memory")

static inline void flush_cpu_icache(void)
{
    for (unsigned long a = 0x40000000u; a < 0x40000000u + MIPS_ICACHE_BYTES; a += MIPS_ICACHE_LINE_BYTES)
        MIPS_CACHE_OP(0x00, a);                 /* Index_Invalidate_I */
}
static inline void flush_cpu_dcache(void)
{
    for (unsigned long a = 0x40000000u; a < 0x40000000u + MIPS_DCACHE_BYTES; a += MIPS_LINE_BYTES)
        MIPS_CACHE_OP(0x01, a);                 /* Index_Writeback_Inv_D */
}
/* the lines of [p, p + n) only */
static inline void flush_cpu_dcache_range(const void *p, unsigned long n)
{
    if (!n) return;
    unsigned long start = (unsigned long)p, a = start & ~(MIPS_LINE_BYTES - 1u), e = start + n;
    if (n >= MIPS_DCACHE_BYTES) { flush_cpu_dcache(); return; }
    for (; a < e; a += MIPS_LINE_BYTES) {
        /* Preserve dirty bytes outside the requested range on partial lines. */
        if (a < start || e - a < MIPS_LINE_BYTES) MIPS_CACHE_OP(0x15, a);
        else MIPS_CACHE_OP(0x11, a);
    }
}
static inline void writeback_cpu_dcache_all(void)
{
    for (unsigned long a = MIRLO_CACHEABLE_BASE; a < MIRLO_CACHEABLE_BASE + MIRLO_CACHEABLE_BYTES; a += MIPS_LINE_BYTES)
        MIPS_CACHE_OP(0x19, a);                 /* Hit_Writeback_D */
}
/* write back the lines of [p, p + n), leaving them cached */
static inline void writeback_cpu_dcache_range(const void *p, unsigned long n)
{
    if (!n) return;
    if (n >= MIRLO_CACHEABLE_BYTES) { writeback_cpu_dcache_all(); return; }
    unsigned long a = (unsigned long)p & ~(MIPS_LINE_BYTES - 1u), e = (unsigned long)p + n;
    for (; a < e; a += MIPS_LINE_BYTES) MIPS_CACHE_OP(0x19, a);     /* Hit_Writeback_D */
}
static inline void flush_l2_cache(void) {}

void busy_wait(unsigned int ms);
void busy_wait_us(unsigned int us);

#ifdef __cplusplus
}
#endif

#endif /* __SYSTEM_H */
