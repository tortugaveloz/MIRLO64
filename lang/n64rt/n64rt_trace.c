/* Test build only (GBTRACE=1): the runtime's Game Boy save messages (n64rt_gbvirt.c) and the sizes of the Transfer Pak ROM slots, kept in memory and written,
 * a moment after they change, to the file Saves/gbc/common/mirlo_trace.txt by its path (data slot 6, which has no file of its own, and the GameShark memory
 * cartridge 2's RAM, which no ROM uses in the test). */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"

#define TR_MEM  ((volatile u8 *)(uintptr_t)LAYOUT_GS)
#define TR_SLOT LAYOUT_GBSAVE2_SLOT
#define TR_MAX  0xF000u
#define TR_PATH "/Saves/gbc/common/mirlo_trace.txt"
static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
/* (one struct: a restart keeps it, n64rt_trace_keep, and the file goes on from where it was) */
static struct { u32 len, wlen; u64 t0, last; u8 init; u32 sz[2]; int kind[2]; } s_tr = { .kind = { -1, -1 } };
#define s_len  s_tr.len
#define s_wlen s_tr.wlen
#define s_t0   s_tr.t0
#define s_last s_tr.last
#define s_init s_tr.init
#define s_sz   s_tr.sz
#define s_kind s_tr.kind
void *n64rt_trace_keep(u32 *bytes) { *bytes = sizeof s_tr; return &s_tr; }

void n64rt_trace_log(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (!s_init) { s_init = 1; s_t0 = now(); }
    u32 ms = (u32)((now() - s_t0) / (CONFIG_CLOCK_FREQUENCY / 1000u));
    char head[16];
    int h = snprintf(head, sizeof head, "%6lu.%03lu ", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u));
    if (s_len + (u32)h + (u32)n + 1u > TR_MAX) return;
    for (int i = 0; i < h; i++) TR_MEM[s_len++] = (u8)head[i];
    for (int i = 0; i < n; i++) TR_MEM[s_len++] = (u8)line[i];
    if (line[n - 1] != '\n') TR_MEM[s_len++] = '\n';
}

void n64rt_trace_field(void)
{
    if (!s_init) { s_init = 1; s_t0 = now(); n64rt_trace_log("trace start\n"); }
    for (int n = 0; n < 2; n++) {
        u32 sz = n64rt_slot_size(n ? LAYOUT_GBROM2_SLOT : LAYOUT_GBROM_SLOT, 0);
        if (sz != s_sz[n]) { n64rt_trace_log("slot %d size %lu -> %lu (pak kind %d)\n", n ? 9 : 8, (unsigned long)s_sz[n], (unsigned long)sz, n64rt_pak_kind(n)); s_sz[n] = sz; }
    }
    for (int n = 0; n < 2; n++) { int k = n64rt_pak_kind(n); if (k != s_kind[n]) { n64rt_trace_log("controller %d pak kind %d -> %d\n", n + 1, s_kind[n], k); s_kind[n] = k; } }
    if (s_len == s_wlen || now() - s_last < 3u * (u64)CONFIG_CLOCK_FREQUENCY / 2u) return;
    s_last = now();
    s_wlen = s_len;
    flush_cpu_dcache_range((const void *)TR_MEM, s_len);
    volatile u8 *ps = (volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM;
    for (u32 i = 0; i < 0x110u; i++) ps[i] = 0;
    for (u32 i = 0; TR_PATH[i]; i++) ps[i] = (u8)TR_PATH[i];
    *(volatile u32 *)(uintptr_t)(LAYOUT_GS_PARAM + 0x100u) = 3u;
    *(volatile u32 *)(uintptr_t)(LAYOUT_GS_PARAM + 0x104u) = s_len;
    flush_cpu_dcache_range((const void *)ps, 0x110u);
    apf_bridge_slot_id_write(TR_SLOT);
    apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
    for (volatile int i = 0; i < 256; i++) { }
    (void)apf_bridge_status_read();
    apf_bridge_request_openfile_write(1u);
    u64 end = now() + (u64)CONFIG_CLOCK_FREQUENCY;
    while (!apf_bridge_status_read()) if (now() > end) return;
    int r = (int)(apf_bridge_command_result_code_read() & 7u);
    if (r != 0 && r != 1) return;
    apf_bridge_data_offset_write(0);
    apf_bridge_transfer_length_write(s_len);
    apf_bridge_ram_data_address_write(LAYOUT_GS);
    for (volatile int i = 0; i < 256; i++) { }
    (void)apf_bridge_status_read();
    apf_bridge_request_write_write(1u);
    end = now() + 2u * (u64)CONFIG_CLOCK_FREQUENCY;
    while (!apf_bridge_status_read()) if (now() > end) return;
}
