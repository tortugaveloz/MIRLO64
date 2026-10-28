#ifndef N64RT_PERSIST_H
#define N64RT_PERSIST_H
#include <generated/csr.h>
#include <system.h>

/* The Pocket and the PI read SDRAM directly. Complete the writeback and
 * drain the CPU's store FIFO before acknowledging a persistent write. */
static inline void n64rt_persist(const void *p, unsigned long n)
{
    if (!n) return;
    if (n >= MIPS_DCACHE_BYTES) flush_cpu_dcache();
    else writeback_cpu_dcache_range(p, n);
    (void)monitor_id_read();
}
#endif
