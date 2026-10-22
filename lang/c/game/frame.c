/* See frame.h. */
#include <stdint.h>
#include <stdio.h>
#include <generated/csr.h>
#include <generated/mem.h>
#include <generated/soc.h>
#include <system.h>   /* flush_cpu_dcache() */

#include "frame.h"
#include "geom_load.h"
#include GEOM_FW_HEADER
#include "log.h"
#include "../geom/geom_gdl.h"
/* RDP, the N64-style rasterizer: every
 * frame's clears lead its GDL and a SYNC FULL ends it (GDL_RAW, forwarded
 * by the geom core in stream order), in serial and pipelined mode alike, so
 * once frame_init() is done the geom core is the only writer of RDP's
 * command FIFO. A frame is complete when sync_count counts its SYNC FULL:
 * RDP counts one only once every write before it has left for DRAM. */
#define FRAME_RDP 1
#include "../rdp/rdp_frame.h"

/* GEOM_TRIS_EMITTED_ADDR: geom_load.h */

/* Multi-buffered RGB565 framebuffers in the video-framebuffer SDRAM region.
 * More than two: the scan-out DMA latches its base at the frame boundary, so a video_flip() can take up to
 * one extra frame to take effect. With only two buffers the game CPU would
 * then start clearing/re-rendering the buffer still on screen -> a black bar /
 * flicker. Four were kept after the flip-ordering fix in frame_submit(); the
 * extra slot costs 256 KiB of otherwise unused SDRAM and nothing else. */
#define FB_BYTES   (VIDEO_FRAMEBUFFER_HRES * VIDEO_FRAMEBUFFER_VRES * 2)
/* The framebuffer's width: VIDEO_FRAMEBUFFER_HRES (268), or 320 when the game
 * file's header asks for the N64's own 320 x 240 (frame_set_video). Every
 * buffer slot below is 256 KiB, room for 320 x 240 x 2. */
static unsigned s_hres = VIDEO_FRAMEBUFFER_HRES;
#define NUM_FB     4
static const uint32_t s_fb[NUM_FB] = {
    VIDEO_FRAMEBUFFER_BASE + 0u * 0x00040000u,
    VIDEO_FRAMEBUFFER_BASE + 1u * 0x00040000u,
    VIDEO_FRAMEBUFFER_BASE + 2u * 0x00040000u,
    VIDEO_FRAMEBUFFER_BASE + 3u * 0x00040000u,
};

/* The depth buffer (16 bits a pixel) -- one, not one per colour buffer: it
 * is cleared at the start of every frame before the geom core emits
 * triangles, so nothing carries over from frame to frame. Past the colour
 * buffers. */
/* ---- per-phase profiler ------------------------------------------------
 * Accumulates cycles per phase and reports averages with the frame stats.
 * Costs 2 CSR reads per phase boundary -- against the 4096 this same function
 * spends polling writes_idle, that is free. */
enum { PH_CLEAR, PH_GEOM, PH_SYNC, PH_IDLE, PH_VBL, PH_N };
static uint64_t s_ph_acc[PH_N];
static uint32_t s_ph_frames;


/* The +4 KiB is load-bearing: it puts every depth pixel in a different SDRAM
 * bank from the same pixel's colour. LiteDRAM maps addresses ROW_BANK_COL, so
 * the bank is byte-address bits 11-12 and repeats every 8 KiB; the colour
 * buffers sit at 256 KiB multiples, and at +1 MiB exactly the depth buffer
 * shared each pixel's bank with a different row. Every depth-tested fragment
 * (read depth, write depth, write colour) then paid a precharge + activate.
 * 4 KiB = two banks over: measured on hardware, depth-tested fill 14.15 ->
 * 11.48 cycles/px, small triangles -16 %. */
#define DEPTH_BANK_OFF 0x1000u
#define DEPTH_BUFFER (VIDEO_FRAMEBUFFER_BASE + 0x00100000u + DEPTH_BANK_OFF)

/* Consecutive reads of perf_status.writes_idle that must all come back set
 * before the frame is considered settled in DRAM. See the wait in
 * frame_submit() for why one sample is not enough. */
#define WR_IDLE_STABLE 4096u

/* Two GDL ring slots so the game CPU can build frame N+1 while the geometry
 * core is still walking frame N. Sized for a full SM64 level frame: thousands
 * of triangles + their vertex payloads (4 words each) + matrix commands.
 *
 * Placed at a fixed SDRAM address past the framebuffer window rather than in
 * .bss: the full-game firmware links into a 12 MiB region below the
 * framebuffer and 2 x 800 KiB of .bss would not fit. Same SDRAM, still cached
 * (flushed before the geom kick), just not linker-allocated. */
#define GDL_WORDS      200000u
#define GDL_RING_BASE  (VIDEO_FRAMEBUFFER_BASE + 0x00400000u)   /* 0x41000000 */
static uint32_t *const s_gdl[2] = {
    (uint32_t *)(uintptr_t)(GDL_RING_BASE + 0u * GDL_WORDS * 4u),
    (uint32_t *)(uintptr_t)(GDL_RING_BASE + 1u * GDL_WORDS * 4u),
};

static int      s_fbi;         /* framebuffer index we render into this frame */
static int      s_gslot;       /* GDL ring slot */
static int      s_geom_busy;   /* a frame is in flight on the geometry core */
static uint32_t s_frame;       /* frame counter (for occasional logging) */
static uint64_t s_t_last;      /* uptime cycles at the end of the last frame */
/* Frame-time stats, accumulated between reports (see frame_submit). */
static uint32_t s_slow_n, s_max_ms, s_max_frame, s_last_print_ms;
static uint32_t s_max_b, s_max_w, s_max_g, s_max_v;

/* ---- low-level ------------------------------------------------------- */
/* frame_init() only: the geom core is idle, so the game CPU may write RDP's
 * FIFO directly. Waits for room rather than dropping a word. */
static void frame_rdp_push(const uint32_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        uint32_t spins = 0;
        while (rdp_cmd_status_full_read()) { if (++spins == 40000000u) break; }
        rdp_cmd_data_write(w[i]);
    }
}
/* frames completed (SYNC FULLs), the counter the pipelined path accumulates */
static inline uint32_t frame_sync_count(void) { return rdp_sync_count_read(); }

/* Pixels a clear has to touch, and the byte offset it starts at. */
#define CLEAR_PIX   ((uint32_t)VIDEO_FRAMEBUFFER_HRES * (uint32_t)VIDEO_FRAMEBUFFER_VRES)
#define CLEAR_OFF16 0u
/* Colour the back buffer is cleared to, as its components and as the RGB565
 * value written to memory; keep the two in sync. */
#define CLEAR_R 0
#define CLEAR_G 0
#define CLEAR_B 0
#define CLEAR_RGB565 ((uint16_t)((((CLEAR_R) & 0xf8) << 8) | (((CLEAR_G) & 0xfc) << 3) | ((CLEAR_B) >> 3)))

/* This frame's clear colour: CLEAR_R/G/B unless the frame asks for another
 * (frame_set_clear_rgb(), e.g. SM64's background colour). In pipelined mode
 * the clear is already in the GDL when the frame is built, so its colour word
 * is patched there (s_clear_word) -- the GDL is flushed at submit, after. */
/* RDP: 0xRRGGBB; s_clear_word is the colour SET FILL's word (an RGB565 pair) */
#define CLEAR_RGB_DEFAULT ((uint32_t)(CLEAR_R) << 16 | (uint32_t)(CLEAR_G) << 8 | (uint32_t)(CLEAR_B))
static uint32_t  s_clear_rgb = CLEAR_RGB_DEFAULT;
static uint32_t *s_clear_word;

void frame_set_clear_rgb(uint32_t rgb) {
    s_clear_rgb = rgb & 0xFFFFFFu;
    if (s_clear_word) {
        uint32_t c565 = ((rgb >> 8) & 0xF800u) | ((rgb >> 5) & 0x07E0u) | ((rgb >> 3) & 0x001Fu);
        *s_clear_word = c565 << 16 | c565;
    }
}

/* returns 1 if a real vblank edge was seen, 0 if it timed out (VTG stalled --
 * the scan-out DMA underflowed and back-pressured the timing generator). */
static uint32_t s_vbl_spins;    /* spins the last wait_vblank_bounded() needed */

static int wait_vblank_bounded(void) {
    uint32_t n = 0;
    while (!apf_video_video_vblank_triggered_read()) {
        if (++n == 4000000u) { s_vbl_spins = n; return 0; }
    }
    s_vbl_spins = n;
    return 1;
}

/* Point the scan-out DMA at `base`. The scan-out DMA reader latches its
 * base at each loop wrap, so
 * this write takes effect atomically on the next frame boundary -- tear-free,
 * no mid-transfer address jump. */
static void video_flip(uint32_t base) {
    video_framebuffer_dma_base_write(base);
}

static uint32_t s_geom_spins;   /* spins the last geom_wait_done() needed */

static int geom_wait_done(void) {   /* returns 1 ok, 0 timed out */
    s_geom_spins = 0;
    if (!s_geom_busy) return 1;
    uint32_t spins = 0;
    int ok = 1;
    while (!mailbox_status_game_pending_read()) {
        if (++spins == 20000000u) { ok = 0; break; }
    }
    s_geom_spins = spins;
    mailbox_game_ack_write(1);
    s_geom_busy = 0;
    return ok;
}

/* Free-running 64-bit cycle counter. NOTE: `csrr cycle`/`instret` are illegal
 * instructions on this SoC's VexRiscv-SMP (only `time` is permitted) and trap
 * into a silent hang -- use timer0's uptime counter instead. */
static uint64_t uptime_cycles(void) {
    timer0_uptime_latch_write(1);
    return timer0_uptime_cycles_read();
}
uint64_t frame_uptime_cycles(void) { return uptime_cycles(); }
uint64_t frame_emit_cyc;


/* ---- API --------------------------------------------------------- */
/* Flags handed to the geom core (lang/c/geom/geom_cfg.h's GEOM_CFG_ADDR).
 * 0: none (the bits are geom_cfg.h's GEOM_CFG_TEX_*).
 */
#define GEOM_CFG 0u

/* 268 (the default) or 320 x 240: the scan-out's timing set, the Pocket's
 * scaler slot with it (the fixed video timing generator,
 * video.json), the DMA's frame length, the clears, the geom core's screen
 * edges (GDL_FBSIZE) and a display-list translator's layout (f3d_fb_hres). */
/* A display-list translator linked in (e.g. an N64 GBI one) reads the
 * framebuffer width here; weak, so programs without one link too. */
int f3d_fb_hres __attribute__((weak));
static void video_program(unsigned hres)
{
    s_hres = hres;
    f3d_fb_hres = (int)hres;
    video_framebuffer_vtg_enable_write(0);
    video_framebuffer_dma_enable_write(0);
    video_framebuffer_dma_length_write(hres * VIDEO_FRAMEBUFFER_VRES * 2u);
    video_framebuffer_vtg_mode_write(hres == 320u);
    video_framebuffer_dma_enable_write(1);
    video_framebuffer_vtg_enable_write(1);
}
/* Which one: the game file's header (there is no menu override). */
static unsigned s_hdr_hres = VIDEO_FRAMEBUFFER_HRES;
/* before frame_init(): the game file's header's choice (268 or 320) */
void frame_set_video(unsigned hdr_hres)
{
    s_hdr_hres = hdr_hres == 320u ? 320u : VIDEO_FRAMEBUFFER_HRES;
    video_program(s_hdr_hres);
    printf("  video %ux%u (game file %u)\n", s_hres, (unsigned)VIDEO_FRAMEBUFFER_VRES, s_hdr_hres);
}
unsigned frame_hres(void) { return s_hres; }

/* every buffer cleared at the current width, then one SYNC FULL: all four
 * are in DRAM once it counts. RDP must be idle (no frame in flight).
 * Returns the spins waited. */
static uint32_t clear_all_buffers(void)
{
    uint32_t w[RDP_FRAME_CLEAR_WORDS], s0 = rdp_sync_count_read(), n = 0;
    for (int i = 0; i < NUM_FB; i++)
        frame_rdp_push(w, rdp_frame_clear(w, s_fb[i], DEPTH_BUFFER, s_hres,
                                      VIDEO_FRAMEBUFFER_VRES, CLEAR_RGB_DEFAULT));
    frame_rdp_push(w, rdp_frame_end(w));
    while (rdp_sync_count_read() == s0) { if (++n == 40000000u) break; }
    return n;
}

void frame_init(void) {
    /* Publish the flags before the geom core can run a single display list. */
    *(volatile uint32_t *)0x41300000u = (uint32_t)(GEOM_CFG);
    flush_cpu_dcache();
    printf("  geom cfg = %lu\n", (unsigned long)(GEOM_CFG));
    /* the geom core's firmware: this program's own (GEOM_FW_HEADER: its
     * build of lang/c/geom, geom_fw.h), started once its configuration is out */
    if (!mailbox_geom_run_read()) {
        geom_load(geom_rom_a_words, GEOM_ROM_A_NWORDS, geom_rom_b_words, GEOM_ROM_B_NWORDS);
        uint32_t e = geom_start();
        if (e) printf("  !! the geom core did not start (heartbeat %08lx)\n", (unsigned long)e);
    }

    /* Clear every buffer so the first frames shown are not SDRAM garbage */
    {
        uint32_t n = clear_all_buffers();
        printf("  fi: %d buffers cleared (sync spin=%lu, status=%08lx, dropped=%lu)\n", NUM_FB,
               (unsigned long)n, (unsigned long)rdp_status_read(), (unsigned long)rdp_cmd_dropped_read());
    }

    s_fbi = 0;
    s_gslot = 0;
    s_geom_busy = 0;
    video_flip(s_fb[NUM_FB - 1]);   /* show the last buffer while we render fb 0 */
}

uint32_t frame_back_buffer(void) { return s_fb[s_fbi]; }
uint32_t frame_last_wspin, frame_last_wait_us;

/* bring-up: trace the first few frames' phase progress over UART */
static uint32_t s_dbg_frames;
volatile uint32_t g_frames_done;   /* flips completed */
/* Bring-up tracing of frame_begin/frame_submit's phases: DBG_FRAMES frames
 * are traced (through the RAM log, log_pump()). */
#define DBG_FRAMES 4
#define DBG_FRAME (s_dbg_frames < DBG_FRAMES)
#define MARK(ch) ((void)0)
#define DBG_LOG(...) log_printf(__VA_ARGS__)

/* ---- pipelined submission (opt-in; see frame.h) ----------------------
 *
 * The geom core and RDP run frame after frame back to back: frame k+1 is
 * kicked as soon as the geom core has finished WALKING frame k, while RDP
 * is still drawing it. Two things make that safe:
 *
 *  - Every framebuffer command travels INSIDE the GDL (GDL_RAW): frame_begin
 *    writes this frame's clears at its head and frame_submit writes a SYNC
 *    FULL at its tail, so the geom core forwards them in stream order. The
 *    game CPU never writes RDP's command FIFO in this mode -- it could not
 *    without waiting for the geom core to stop streaming, which would
 *    serialise every frame behind the previous one.
 *
 *  - Completion comes from the gateware, per frame: RDP counts a SYNC FULL
 *    (sync_count) only once everything drawn before it is in DRAM. A "writes
 *    idle" flag could not answer this -- with frame k+1's clears already
 *    queued, the write channel never goes quiet.
 *
 * Frame k renders into s_kbuf[k % NUM_FB], the buffer frame k-4 used. It may
 * only be cleared once that buffer is off the screen: a newer frame must have
 * been flipped to AND latched by the scan-out DMA, which picks the base up at
 * its loop wrap -- that is ahead of vblank (the DMA prefetches), so a flip only
 * counts as latched after TWO vblanks. s_lat tracks that. */
static int      s_overlap;
static uint32_t s_sub;             /* frames submitted since entering the mode */
static uint32_t s_done;            /* ...of which complete (and hooked) */
static uint16_t s_sync_base;       /* sync_count at the last poll */
static uint32_t s_syncs;           /* SYNC FULLs completed since overlap mode began, 32 bits */
/* All four counters are free-running uint32 and only ever compared by their
 * difference, (int32_t)(a - b): they may wrap (at 2^32 frames), and they are
 * never more than a few frames apart. */
static uint32_t s_shown;           /* frames flipped to (the newest is frame s_shown - 1) */
static uint32_t s_lat;             /* ... of which the scan-out has definitely picked up */
static uint32_t s_flip_vcnt;       /* video frame_counter at the last flip */
static uint32_t s_kbuf[NUM_FB];    /* colour buffer of frame k, at [k % NUM_FB] */
void (*frame_preflip_hook)(uint32_t back);
uint64_t frame_pipe_wait_geom_cyc, frame_pipe_wait_buf_cyc;

static void pipe_poll(void) {
    /* Latch bookkeeping FIRST, before a new flip restarts the vblank count:
     * done the other way round, a frame that had been on screen for a whole
     * geom walk never got recorded as latched, because each poll flipped to a
     * newer one before checking -- and the next frame's buffer-free wait then
     * sat out two vblanks every frame (measured: 14 of 107 ms). */
    if (s_lat != s_shown
        && ((apf_video_video_frame_counter_read() - s_flip_vcnt) & 0xFFFFu) >= 2u)
        s_lat = s_shown;
    /* sync_count is read as 16 bits: accumulate its steps into 32, or after
     * 65536 frames (~45 min of play) `done` wrapped below s_done and the
     * buffer wait below never ended (seen on the Pocket, 2026-09-26) */
    uint16_t cnt = (uint16_t)frame_sync_count();
    s_syncs += (uint16_t)(cnt - s_sync_base);
    s_sync_base = cnt;
    uint32_t done = s_syncs;
    while ((int32_t)(done - s_done) > 0 && (int32_t)(s_sub - s_done) > 0) {
        if (frame_preflip_hook) frame_preflip_hook(s_kbuf[s_done % NUM_FB]);
        s_done++;
        s_frame++;
        g_frames_done++;
    }
    if (s_done != s_shown) {
        s_shown = s_done;
        video_flip(s_kbuf[(s_shown - 1u) % NUM_FB]);
        s_flip_vcnt = apf_video_video_frame_counter_read();
    }
    if (s_lat != s_shown
        && ((apf_video_video_frame_counter_read() - s_flip_vcnt) & 0xFFFFu) >= 2u)
        s_lat = s_shown;
}

/* The game submits nothing while it loads a level -- right after a
 * transition has closed to black -- and pipe_poll() used to run only inside
 * frame_begin()/frame_submit(), so the frames still in flight (the black ones)
 * were never flipped to: the screen kept the last frame before black for the
 * whole load. The 60 Hz timer interrupt now polls too (audio_hal.c
 * timer_tick), except while the game itself is inside the pipeline's
 * bookkeeping (s_pipe_busy), which it would race. */
static volatile uint32_t s_pipe_busy;
void frame_isr_poll(void) { if (s_overlap && !s_pipe_busy) pipe_poll(); }

/* this frame's clears at the head of its GDL (RDP commands, GDL_RAW) */
static void gdl_raw_clears(gdl_cur_t *c, uint32_t back) {
    uint32_t w[RDP_FRAME_CLEAR_WORDS];
    unsigned n = rdp_frame_clear(w, back, DEPTH_BUFFER, s_hres,
                                  VIDEO_FRAMEBUFFER_VRES, s_clear_rgb);
    gdl_fbsize(c, s_hres, VIDEO_FRAMEBUFFER_VRES);   /* the geom core's screen edges */
    gdl_w(c, GDL_HDR(GDL_RAW, n));
    for (unsigned i = 0; i < n; i++) {
        if (i == 13) s_clear_word = c->p < c->end ? c->p : 0;   /* the colour SET FILL's word */
        gdl_w(c, w[i]);
    }
}
/* ...and the SYNC FULL before its GDL_END (the caller ended the list) */
static void gdl_raw_sync(gdl_cur_t *c, uint32_t *start) {
    uint32_t w[2];
    if (c->p > start && c->p[-1] == GDL_HDR(GDL_END, 0)) c->p--;
    gdl_w(c, GDL_HDR(GDL_RAW, rdp_frame_end(w)));
    gdl_w(c, w[0]); gdl_w(c, w[1]);
    gdl_end(c);
}

/* Drain the pipeline: every submitted frame complete, the newest on screen
 * and latched. Leaves RDP idle, so serial mode can take over. */
static void pipe_drain(void) {
    uint32_t n = 0;
    (void)geom_wait_done();
    while (((int32_t)(s_sub - s_done) > 0 || s_lat != s_shown) && ++n < 200000000u) {
        pipe_poll();
        if ((n & 0xFFu) == 0u) log_pump();
    }
}

/* the pipeline's bookkeeping */
void frame_dbg_state(void)
{
    printf("!! pipe overlap=%d sub=%lu done=%lu shown=%lu latched=%lu sync_base=%u sync_count=%u geom_busy=%d fbi=%d gslot=%d vcnt=%lu flip_vcnt=%lu\n",
           s_overlap, (unsigned long)s_sub, (unsigned long)s_done, (unsigned long)s_shown, (unsigned long)s_lat,
           (unsigned)s_sync_base, (unsigned)frame_sync_count(), s_geom_busy, s_fbi, s_gslot,
           (unsigned long)apf_video_video_frame_counter_read(), (unsigned long)s_flip_vcnt);
}

void frame_sync(void) { s_pipe_busy++; if (s_overlap) pipe_drain(); s_pipe_busy--; }

void frame_set_overlap(int on) {
    s_pipe_busy++;
    if (s_overlap) pipe_drain();
    s_overlap = on;
    if (on) {
        s_sync_base = (uint16_t)frame_sync_count();
        s_syncs = 0;
        s_sub = s_done = 0;
        s_shown = s_lat = 0;
    }
    s_pipe_busy--;
}

/* Average submit-to-submit period every FRAME_RATE_EVERY frames, through the
 * RAM log (never blocks): the frame rate of whatever is running, at no cost. */
#define FRAME_RATE_EVERY 100u
/* Pipelined, it also splits the frame: "geom" is how long the game CPU sat
 * waiting for the geom core to take the next list, "buf" for a free
 * framebuffer, "cpu" the rest -- the game's own work. The biggest of
 * cpu / geom-walk / RDP sets the frame rate, and a wait on geom that is
 * a large share of the frame says the geom core is the one. */
/* RDP command FIFO samples taken while waiting on the geom core (see
 * frame_submit_overlap): mostly near full = RDP is the bottleneck, the geom
 * core stalls on it; mostly empty = the geom core's own compute. */
#define RDP_CMD_FIFO_NEAR_FULL ((1u << (CSR_RDP_CMD_STATUS_LEVEL_SIZE - 1)) * 15u / 16u)
static uint32_t s_ws_n, s_ws_full, s_ws_tri;
static uint64_t s_ws_lvl;
/* Frames per second over the last half second, for the on-screen counter
 * (e.g. an on-screen FPS overlay). */
uint32_t frame_fps;
static void frame_rate_tick(void) {
    static uint64_t t0, wg0, wb0, we0;
    static uint32_t n, total;
    uint64_t t = uptime_cycles();
    {   static uint64_t f0; static uint32_t fn;
        if (!f0) f0 = t;
        fn++;
        if (t - f0 >= CONFIG_CLOCK_FREQUENCY / 2u) {
            frame_fps = (uint32_t)(((uint64_t)fn * CONFIG_CLOCK_FREQUENCY + (t - f0) / 2u) / (t - f0));
            fn = 0; f0 = t;
        }
    }
    if (n == 0u) { t0 = t; wg0 = frame_pipe_wait_geom_cyc; wb0 = frame_pipe_wait_buf_cyc; we0 = frame_emit_cyc; }
    total++;
    if (++n > FRAME_RATE_EVERY) {
        const uint32_t cpus = CONFIG_CLOCK_FREQUENCY / 1000000u;
        uint32_t us = (uint32_t)((t - t0) / cpus / FRAME_RATE_EVERY);
        uint32_t wg = (uint32_t)((frame_pipe_wait_geom_cyc - wg0) / cpus / FRAME_RATE_EVERY);
        uint32_t wb = (uint32_t)((frame_pipe_wait_buf_cyc - wb0) / cpus / FRAME_RATE_EVERY);
        uint32_t cpu = us > wg + wb ? us - wg - wb : 0u;
        uint32_t em = (uint32_t)((frame_emit_cyc - we0) / cpus / FRAME_RATE_EVERY);
        log_printf("FPS f=%lu %lu.%lu ms/frame %lu.%lu fps (%s) cpu %lu (emit %lu) geom %lu buf %lu ms\n", (unsigned long)total,
                   (unsigned long)(us / 1000u), (unsigned long)((us / 100u) % 10u),
                   (unsigned long)(10000000u / (us ? us : 1u) / 10u), (unsigned long)(10000000u / (us ? us : 1u) % 10u),
                   s_overlap ? "pipelined" : "serial",
                   (unsigned long)(cpu / 1000u), (unsigned long)(em / 1000u), (unsigned long)(wg / 1000u), (unsigned long)(wb / 1000u));
        if (s_ws_n)
            log_printf("RDPQ while waiting on geom: fifo avg %lu words, near-full %lu%%, rasterising %lu%% (%lu samples)\n",
                       (unsigned long)(s_ws_lvl / s_ws_n), (unsigned long)(s_ws_full * 100u / s_ws_n),
                       (unsigned long)(s_ws_tri * 100u / s_ws_n), (unsigned long)s_ws_n);
        s_ws_n = s_ws_full = s_ws_tri = 0; s_ws_lvl = 0;
        n = 1; t0 = t; wg0 = frame_pipe_wait_geom_cyc; wb0 = frame_pipe_wait_buf_cyc; we0 = frame_emit_cyc;
    }
}

static void frame_submit_overlap(gdl_cur_t *c) {
    gdl_raw_sync(c, s_gdl[s_gslot]);
    uint32_t gdl_words = gdl_used(c, s_gdl[s_gslot]);
    if (gdl_words > GDL_WORDS)
        printf("!! GDL OVERFLOW: %lu / %u words -- geometry truncated\n",
               (unsigned long)gdl_words, (unsigned)GDL_WORDS);
    /* the geom core walks one list at a time: wait for it to finish the last
     * one (RDP may still be drawing it -- that is the point). Poll for
     * completed frames meanwhile, so each one is flipped to as soon as it is
     * in DRAM rather than at the next submit: less display latency, and the
     * buffer the next frame needs is latched off the screen sooner. */
    uint64_t t_w0 = uptime_cycles();
    if (s_geom_busy) {
        uint32_t spins = 0;
        while (!mailbox_status_game_pending_read()) {
            if ((++spins & 0x3FFu) == 0u) {
                pipe_poll(); log_pump();
                /* who is the geom core waiting on? RDP's command FIFO
                 * level and whether it is rasterising, sampled while we wait */
                uint32_t lvl = rdp_cmd_status_level_read();
                s_ws_n++; s_ws_lvl += lvl;
                if (lvl >= RDP_CMD_FIFO_NEAR_FULL) s_ws_full++;
                if (!rdp_status_idle_read()) s_ws_tri++;
            }
            if (spins == 200000000u) break;
        }
        mailbox_game_ack_write(1);
        s_geom_busy = 0;
    }
    uint64_t t_w1 = uptime_cycles();
    frame_pipe_wait_geom_cyc += t_w1 - t_w0;
    /* this frame's clears overwrite the buffer frame s_sub-4 drew */
    uint32_t n = 0;
    /* frame s_sub - NUM_FB's buffer is free once a later frame is latched on
     * screen: while s_lat <= s_sub - (NUM_FB - 1) */
    while ((int32_t)(s_sub - s_lat) >= NUM_FB - 1 && ++n < 200000000u) {
        pipe_poll();
        if ((n & 0xFFu) == 0u) log_pump();
    }
    frame_pipe_wait_buf_cyc += uptime_cycles() - t_w1;
    flush_cpu_dcache();
    mailbox_game_msg_write((uint32_t)(uintptr_t)s_gdl[s_gslot]);
    mailbox_game_kick_write(1);
    s_geom_busy = 1;
    s_sub++;
    s_fbi   = (s_fbi + 1) % NUM_FB;
    s_gslot ^= 1;                      /* the next GDL is built in the OTHER slot, which geom is not reading */
    s_dbg_frames++;                    /* the DBG_FRAME trace covers the first frames only, as in serial mode */
    frame_rate_tick();
    pipe_poll();
    log_pump();
}

void frame_begin(gdl_cur_t *c) {
    MARK('B'); if (DBG_FRAME) DBG_LOG("  fb: begin, wait geom (busy=%d)\n", s_geom_busy);
    if (!s_overlap) geom_wait_done();       /* pipeline point: last frame's geom */
    MARK('b'); if (DBG_FRAME) DBG_LOG("  fb: geom done, gdl_begin slot %d\n", s_gslot);
    gdl_begin(c, s_gdl[s_gslot], GDL_WORDS);
    s_clear_rgb = CLEAR_RGB_DEFAULT;       /* per frame: a background colour is restated each frame */
    s_clear_word = 0;
    if (!s_overlap) gdl_raw_clears(c, s_fb[s_fbi]);   /* serial: the same clears, same place */
    if (s_overlap) {
        /* this frame's clears lead its own list -- see the pipelined block */
        s_kbuf[s_sub % NUM_FB] = s_fb[s_fbi];
        gdl_raw_clears(c, s_fb[s_fbi]);
    }
}

void frame_submit(gdl_cur_t *c) {
    if (s_overlap) { s_pipe_busy++; frame_submit_overlap(c); s_pipe_busy--; return; }
    uint32_t gdl_words = gdl_used(c, s_gdl[s_gslot]);
    MARK('E'); if (DBG_FRAME) DBG_LOG("  fs: enter, gdl_words=%lu\n", (unsigned long)gdl_words);
    if (gdl_words > GDL_WORDS)
        printf("!! GDL OVERFLOW: %lu / %u words -- geometry truncated\n",
               (unsigned long)gdl_words, (unsigned)GDL_WORDS);
    uint32_t back = s_fb[s_fbi];

    /* The clears lead the GDL (frame_begin) and the SYNC FULL ends it, so the
     * geom core queues clears, triangles and SYNC in one ordered stream. The
     * frame is in DRAM once sync_count moves past its value from before the
     * kick: nothing else can end a SYNC while this frame is in flight. */
    uint64_t t_ph = uptime_cycles(), t_ph0 = t_ph;
    gdl_raw_sync(c, s_gdl[s_gslot]);
    uint32_t sync0 = rdp_sync_count_read();
    MARK('C'); if (DBG_FRAME) DBG_LOG("  fs: flush + kick geom (gdl=%lx)\n", (unsigned long)(uintptr_t)s_gdl[s_gslot]);
    flush_cpu_dcache();
    mailbox_game_msg_write((uint32_t)(uintptr_t)s_gdl[s_gslot]);
    mailbox_game_kick_write(1);
    s_geom_busy = 1;
    { uint64_t t = uptime_cycles(); s_ph_acc[PH_CLEAR] += t - t_ph; t_ph = t; }
    MARK('w'); if (DBG_FRAME) DBG_LOG("  fs: wait geom done...\n");
    int gok = geom_wait_done();
    { uint64_t t = uptime_cycles(); s_ph_acc[PH_GEOM] += t - t_ph; t_ph = t; }
    MARK('G'); if (DBG_FRAME) DBG_LOG("  fs: geom g%s, wait SYNC\n", gok ? "ok" : "TO");
    uint32_t bspin = 0, wspin = 0, wdrops = 0;
    uint64_t t_wait0 = uptime_cycles();
    while (rdp_sync_count_read() == sync0) {
        if ((wspin & 0xFFu) == 0u) log_pump();
        if (++wspin == 40000000u) break;
    }
    MARK('Q');
    frame_last_wspin = wspin;
    frame_last_wait_us = (uint32_t)((uptime_cycles() - t_wait0)
                                    / (CONFIG_CLOCK_FREQUENCY / 1000000u));
    { uint64_t t = uptime_cycles(); s_ph_acc[PH_IDLE] += t - t_ph; t_ph = t; }

    /* Only now, with the frame provably in DRAM, hand the scan-out over to it.
     * This ordering is what was wrong before: fb70f84 and 95703a4 both put
     * their completion wait into the *logging* section, i.e. after
     * video_flip(), where it only delayed the following frame and never
     * guarded the flip it was written for. A buffer still filling up was
     * therefore free to reach the display -- whole faces missing, and in the
     * worst captured frame a single triangle sliver on an otherwise cleared
     * background. */
    MARK('u');
    int vok = wait_vblank_bounded();
    MARK('V');
    video_flip(back);
    { uint64_t t = uptime_cycles(); s_ph_acc[PH_VBL] += t - t_ph;
      (void)t_ph0; s_ph_frames++; }            /* clean DMA restart -- also un-sticks a
                                   wedged FIFO/VTG if vok == 0 */
    MARK('D');
    g_frames_done++;
    if (DBG_FRAME) DBG_LOG("  fs: done b=%lu v%s\n", (unsigned long)bspin, vok ? "ok" : "TO");
    s_dbg_frames++;
    frame_rate_tick();
    s_frame++;

    /* Frame timing, accumulated in RAM and reported rarely.
     *
     * Printing per frame is NOT an option here and the reason is worth
     * recording: stdout goes over the JTAG UART, and jtag_uart_relay.py moves
     * ~24 bytes per TCL-RPC round trip, so a ~150-byte log line blocks the CPU
     * for roughly 0.4 s. An earlier version of this flagged any frame over
     * 100 ms and printed it, which was self-sustaining -- the print made the
     * frame slow, which made it print. Every frame came back at 455-492 ms
     * (~2 fps) instead of the real ~29 ms.
     *
     * That same effect is the "occasional sub-second freeze" seen on screen:
     * at ~34 fps the 256-frame heartbeat fires every ~7.5 s and stalls the
     * game CPU for ~0.4 s while it drains. So: accumulate the worst frame
     * between reports and print once every LOG_EVERY frames, and measure how
     * long the report itself takes so the cost stays visible (`pms=`).
     *
     * `period` deliberately excludes the previous report's own printing --
     * s_t_last is taken after the logging block, not before it. */
    #define SLOW_CYCLES ((uint64_t)CONFIG_CLOCK_FREQUENCY / 10u)   /* ~100 ms */
    #define LOG_EVERY   1023u
    uint64_t t_now = uptime_cycles();
    uint32_t period_ms = 0;
    if (s_t_last) {
        uint64_t period = t_now - s_t_last;
        period_ms = (uint32_t)(period / (CONFIG_CLOCK_FREQUENCY / 1000u));
        if (period > SLOW_CYCLES) s_slow_n++;
        if (period_ms > s_max_ms) {
            s_max_ms    = period_ms;
            s_max_frame = s_frame;
            s_max_b     = bspin;
            s_max_w     = wspin;
            s_max_g     = s_geom_spins;
            s_max_v     = s_vbl_spins;
        }
    }

    if (!gok || !vok || (s_frame % LOG_EVERY) == 0) {
        /* Triangles the geom core streamed this frame. Read straight out of
         * geom RAM rather than from the done-mailbox: the geom core overwrites
         * the mailbox with its 0xB0000040 "waiting for doorbell" heartbeat
         * immediately after signalling done, so the mailbox always races and
         * reads back as the heartbeat. geom_tris_emitted is in the geom RAM
         * window (GEOM_RAM_BASE, 0x20008000; lang/c/geom/build/geom.map), a
         * normal SoC bus region the game CPU can read. */
        uint64_t t_log0 = uptime_cycles();
        uint32_t gtris = *(volatile uint32_t *)GEOM_TRIS_EMITTED_ADDR;
        const volatile uint16_t *fb = (const volatile uint16_t *)(uintptr_t)back;
        uint32_t npix = (uint32_t)s_hres * (uint32_t)VIDEO_FRAMEBUFFER_VRES;
        uint32_t drawn = 0;
        flush_cpu_dcache();
        for (uint32_t i = 0; i < npix; i += 64) if (fb[i] != CLEAR_RGB565) drawn++;
        log_printf("F%lu ms=%lu slow=%lu max=%lums@F%lu(b=%lu w=%lu g=%lu v=%lu) pms=%lu "
               "b=%lu w=%lu wd=%lu g%s v%s tris=%lu drawn=%lu\n",
               (unsigned long)s_frame, (unsigned long)period_ms,
               (unsigned long)s_slow_n,
               (unsigned long)s_max_ms, (unsigned long)s_max_frame,
               (unsigned long)s_max_b, (unsigned long)s_max_w,
               (unsigned long)s_max_g, (unsigned long)s_max_v,
               (unsigned long)s_last_print_ms,
               (unsigned long)bspin, (unsigned long)wspin, (unsigned long)wdrops,
               gok ? "ok" : "TO", vok ? "ok" : "TO",
               (unsigned long)gtris, (unsigned long)drawn);
        /* Per-phase breakdown: microseconds per frame, averaged since the
         * last report. This is the whole point of shipping five cores at
         * once -- a variant that does not help still says where its time
         * went, so one session settles it instead of five. */
        if (s_ph_frames) {
            uint32_t us[PH_N], tot = 0;
            for (int i = 0; i < PH_N; i++) {
                us[i] = (uint32_t)(s_ph_acc[i] / s_ph_frames
                                   / (CONFIG_CLOCK_FREQUENCY / 1000000u));
                tot += us[i];
            }
            log_printf("  PH v%d n=%lu clear=%luus geom=%luus sync=%luus "
                       "idle=%luus vbl=%luus sum=%luus\n",
                       0, (unsigned long)s_ph_frames,
                       (unsigned long)us[PH_CLEAR], (unsigned long)us[PH_GEOM],
                       (unsigned long)us[PH_SYNC],  (unsigned long)us[PH_IDLE],
                       (unsigned long)us[PH_VBL],   (unsigned long)tot);
            for (int i = 0; i < PH_N; i++) s_ph_acc[i] = 0;
            s_ph_frames = 0;
        }
        s_slow_n = 0;
        s_max_ms = 0;
        /* Cost of this report, including the framebuffer scan above -- charged
         * to the next one so the number is always visible. */
        s_last_print_ms = (uint32_t)((uptime_cycles() - t_log0)
                                     / (CONFIG_CLOCK_FREQUENCY / 1000u));
    }
    log_pump();   /* never blocks; see log.h */
    s_t_last = uptime_cycles();

    s_fbi   = (s_fbi + 1) % NUM_FB;
    s_gslot ^= 1;
}
