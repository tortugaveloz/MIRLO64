/* System interface for MIRLO's MIPS game CPU (rtl/mips/mips_core.sv).
 * Its caches: 16 KiB I, 8 KiB D, direct mapped, 32-byte lines; the D-cache
 * writes through, so a "flush" only has to drop lines -- memory is always
 * current, but lines may be stale after another master (RDP, the geom or
 * audio core, the Pocket's bridge) wrote SDRAM. A core built with larger
 * caches (MIRLO_CPU_IC_LOG2, MIRLO_CPU_DC_LOG2) needs nothing else here: an
 * index op drops every line its 16 KiB / 8 KiB index stands for. */
#ifndef __SYSTEM_H
#define __SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#define MIPS_ICACHE_BYTES 16384u
#define MIPS_DCACHE_BYTES 8192u
#define MIPS_LINE_BYTES   32u

/* CACHE op on the line at `a` (op[17:16]: 0 I, 1 D; op[20:18]: 0 an index
 * op, 4 a hit op -- the core drops the line either way, never looking at its
 * tag: a hit op is the one line `a` itself can be in) */
#define MIPS_CACHE_OP(op, a) __asm__ volatile(".set push\n.set mips3\ncache %0, 0(%1)\n.set pop" :: "i"(op), "r"(a) : "memory")

static inline void flush_cpu_icache(void)
{
    for (unsigned long a = 0x40000000u; a < 0x40000000u + MIPS_ICACHE_BYTES; a += MIPS_LINE_BYTES)
        MIPS_CACHE_OP(0x00, a);                 /* Index_Invalidate_I */
}
static inline void flush_cpu_dcache(void)
{
    for (unsigned long a = 0x40000000u; a < 0x40000000u + MIPS_DCACHE_BYTES; a += MIPS_LINE_BYTES)
        MIPS_CACHE_OP(0x01, a);                 /* Index_Writeback_Inv_D: write-through, so just invalid */
}
/* the lines of [p, p + n) only */
static inline void flush_cpu_dcache_range(const void *p, unsigned long n)
{
    unsigned long a = (unsigned long)p & ~(MIPS_LINE_BYTES - 1u), e = (unsigned long)p + n;
    if (n >= MIPS_DCACHE_BYTES) { flush_cpu_dcache(); return; }
    for (; a < e; a += MIPS_LINE_BYTES) MIPS_CACHE_OP(0x11, a);     /* Hit_Invalidate_D */
}
static inline void flush_l2_cache(void) {}

void busy_wait(unsigned int ms);
void busy_wait_us(unsigned int us);

#ifdef __cplusplus
}
#endif

#endif /* __SYSTEM_H */
