/* n64rt: the RSP's graphics task. The geom core processes the game's display
 * list itself, as the N64's RSP did: its firmware (lang/c/geom, loaded here
 * at boot from GEOM_FW_HEADER) carries this runtime's translator (f3d_emit.c,
 * Mirlo-N64's: F3DEX2 and the other microcodes, the N64's texture formats,
 * combiners and render modes), which it runs a piece at a time and walks
 * (GDL_F3D), and RDP draws -- into the colour image the game set, in its own
 * RDRAM, as the RDP would. The game CPU only tells which microcode it is.
 *
 * Pipelined, as on the N64, where the RSP's done (SP) only means the display
 * list has been read and the RDP draws on until its full sync (DP): the SP
 * event comes as soon as the list is translated -- the GDL carries what the
 * geom core needs of it (vertices, matrices, lights); only textures are read
 * from RDRAM later, as the RDP reads them -- so the game builds and submits
 * its next frame while the geom core walks this one. Two GDL buffers: one
 * the geom core walks, one translated and queued behind it; the timer's tick
 * polls (n64rt_gfx_poll), kicks the queued one and posts each task's DP
 * event, in order, when RDP has drawn it. */
#include <stdio.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>
#include "n64rt.h"
#include "gdl_build.h"
#include "geom_rdp.h"
#include "f3d_emit.h"
#include "geom_load.h"
#include "geom_cfg.h"
#include GEOM_FW_HEADER

extern int f3d_statelog;

void n64rt_gfx_init(void)
{
    *(volatile u32 *)(uintptr_t)GEOM_CFG_ADDR = 0;   /* the geom core's options: the defaults */
    geom_load(geom_rom_a_words, GEOM_ROM_A_NWORDS, geom_rom_b_words, GEOM_ROM_B_NWORDS);
    u32 e = geom_start();
    n64rt_log("n64rt: geom core firmware %u+%u words, %s (%08lx)\n", GEOM_ROM_A_NWORDS, GEOM_ROM_B_NWORDS,
              e ? "NOT running -- no graphics" : "running", (unsigned long)e);
}

/* two buffers of 1 MiB (a frame's GDL is ~100 KiB): 0x4120_0000 on is the
 * geom core's (geom_rdp.h's texture staging ring, geom_cfg.h at
 * 0x4130_0000) -- a second buffer there had the staged textures overwrite
 * the list, and RDP drew over the game's code */
#define GDL_BUF   0x41000000u
#define GDL_BYTES 0x00100000u
#define GDL_WORDS (GDL_BYTES / 4u)


static u32 s_ud = ~0u;
static int s_walk = -1;                     /* the buffer the geom core walks */
static int s_queued = -1;                   /* translated, waiting for the geom core */
static u32 s_kicked, s_dp_posted, s_sync0;  /* tasks given to the geom core; DP events; RDP's syncs at the first */
/* each stage's time (sys cycles), summed over 64 tasks */
static inline u32 cyc(void) { timer0_uptime_latch_write(1); return (u32)timer0_uptime_cycles_read(); }
static u32 s_t_start, s_t_kick, s_n, s_sum_tr, s_sum_geom, s_sum_wait, s_sum_period, s_words;
static u32 s_t_trend, s_sum_tail;            /* the geom core's time after the translation ended */
static u32 s_t_dp, s_sum_dp2next, s_n_dp2next, s_overlap;   /* the game's time from a DP to its next task; tasks that came while one was under way */

static void kick(int b)
{
    if (s_kicked == 0) s_sync0 = rdp_sync_count_read();
    s_kicked++;
    s_walk = b;
    s_t_kick = cyc();
    mailbox_game_msg_write(GDL_BUF + (u32)b * GDL_BYTES);
    mailbox_game_kick_write(1);
}

/* The SP event once the geom core has walked the list, not as soon as it is
 * translated (above): Pokemon Stadium's
 * scheduler waits for each frame's DP before it sends the next anyway (none
 * of its tasks came while the last was being drawn), so pipelining gained it
 * nothing -- and broke its title -> menu change (2026-10-01). */
const int n64rt_gfx_sp_early = 0;

/* the events due (bit OS_EVENT_SP, OS_EVENT_DP); interrupts off */
u32 n64rt_gfx_poll(void)
{
    u32 ev = 0;
    if (s_walk >= 0 && mailbox_status_game_pending_read()) {
        mailbox_game_ack_write(1);
        if (!n64rt_gfx_sp_early) ev |= 1u << OS_EVENT_SP;
        { u32 now = cyc(); s_sum_geom += now - s_t_kick; s_sum_tail += now - s_t_trend; }
        s_walk = -1;
        if (s_queued >= 0) { int b = s_queued; s_queued = -1; kick(b); }
    }
    /* one full sync per task, at its end: one DP event each, in order (one
     * per poll: the next tick posts the next) */
    if (s_dp_posted < s_kicked && rdp_sync_count_read() - s_sync0 > s_dp_posted) {
        s_dp_posted++;
        s_t_dp = cyc();
        ev |= 1u << OS_EVENT_DP;
    }
    return ev;
}
int n64rt_gfx_busy(void) { return s_walk >= 0 || s_queued >= 0 || s_dp_posted < s_kicked; }

void n64rt_gfx_task(OSTask *t)
{
    /* both buffers in use (one walked, one queued): wait for the geom core */
    u32 tw = cyc();
    for (u32 n = 0; s_queued >= 0 && n < 200000000u; n++)
        if (n64rt_gfx_poll() & (1u << OS_EVENT_DP)) n64rt_post_event(OS_EVENT_DP);
    if (s_queued >= 0) n64rt_fatal("n64rt: the geom core never finished a display list\n");
    u32 ud = N64RT_PHYS(t->t.ucode_data);
    if (ud != s_ud) {                        /* which microcode: once per ucode data segment */
        s_ud = ud;
        f3d_gbi1 = f3d_ucode_kind((const uint8_t *)(uintptr_t)N64RT_K0(ud), t->t.ucode_data_size, 0);
        n64rt_log("n64rt: graphics microcode kind %d\n", f3d_gbi1);
    }
    /* Select + L on the Pocket: one task's state commands to the log (f3d_emit.c) */
    {
        static int s_held;
        u32 k = apf_input_cont1_key_read(), combo = (1u << 14) | (1u << 8);
        int on = (k & combo) == combo;
        f3d_statelog = on && !s_held;
        s_held = on;
        if (f3d_statelog) n64rt_log("n64rt: gfx task state log, ucode kind %d\n", f3d_gbi1);
    }
    if (s_walk >= 0 || s_dp_posted < s_kicked) s_overlap++;
    else if (s_t_dp) { s_sum_dp2next += tw - s_t_dp; s_n_dp2next++; }
    u32 t0 = cyc();
    s_sum_wait += t0 - tw;
    if (s_n) s_sum_period += t0 - s_t_start;
    s_t_start = t0;
    int b = s_walk == 0 ? 1 : 0;             /* the buffer the geom core is not walking */
    gdl_cur_t c;
    uint32_t *gdl = (uint32_t *)(uintptr_t)(GDL_BUF + (u32)b * GDL_BYTES);
    gdl_begin(&c, gdl, GDL_WORDS - 4u);
    /* the list, for the geom core to translate and walk (GDL_F3D): the RDRAM
     * as it sees it (SDRAM), the microcode, the colour image's width */
    gdl_f3d_cfg(&c, (const void *)(uintptr_t)(N64RT_RDRAM_SDRAM + N64RT_PHYS(t->t.data_ptr)),
                (u32)f3d_gbi1 | (u32)(f3d_statelog != 0) << 8 | 320u << 16, N64RT_RDRAM_SDRAM, 8u << 20,
                0, 0, 0, 0);
    gdl_w(&c, GDL_HDR(GDL_RAW, 2));          /* the RDP's full sync: every write before it is in DRAM */
    gdl_w(&c, (uint32_t)RDP_OP_SYNC_FULL << 24);
    gdl_w(&c, 0);
    gdl_end(&c);
    if (c.p > c.end) n64rt_log("n64rt: GDL overflow (%lu words)\n", (unsigned long)(c.p - gdl));

    s_t_trend = cyc();
    s_sum_tr += s_t_trend - t0;
    s_words += (u32)(c.p - gdl);
    if (++s_n == 64u) {
        const u32 us = CONFIG_CLOCK_FREQUENCY / 1000000u;
        n64rt_log("n64rt: gfx per task: period %lu us, translate %lu us, geom %lu us (%lu after the translation), waited for a buffer %lu us, %lu GDL words; "
                  "%lu/64 came while the last was being drawn, the others %lu us after its DP\n",
                  (unsigned long)(s_sum_period / 63u / us), (unsigned long)(s_sum_tr / 64u / us),
                  (unsigned long)(s_sum_geom / 64u / us), (unsigned long)(s_sum_tail / 64u / us),
                  (unsigned long)(s_sum_wait / 64u / us), (unsigned long)(s_words / 64u),
                  (unsigned long)s_overlap, (unsigned long)(s_n_dp2next ? s_sum_dp2next / s_n_dp2next / us : 0u));
        s_n = 0; s_sum_tr = s_sum_geom = s_sum_wait = s_sum_period = s_words = 0;
        s_overlap = s_sum_dp2next = s_n_dp2next = s_sum_tail = 0;
    }
    if (s_walk < 0) kick(b);
    else s_queued = b;
}
