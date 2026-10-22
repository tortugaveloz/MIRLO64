/* n64rt: the N64's hardware behind libultra's functions -- VI, PI, SI, AI,
 * the RSP's tasks, the caches, libultra's 64-bit helpers.
 *
 * The RSP's tasks run on Mirlo's RSP with the game's own microcode, and the
 * RDP draws what they leave in RDRAM (n64rt_rsp.c). The AI is in hardware
 * (rtl/soc/rcp_ai.sv) when the SoC has it; else the game CPU feeds the
 * Pocket's audio FIFO itself (n64rt_rsp.c). */
#include <string.h>
#include <generated/csr.h>
#include <system.h>
#include <layout.h>
#include "n64rt.h"
#include "n64rt_savedb.h"

#define K0P(va) ((void *)(uintptr_t)N64RT_K0(va))      /* an RDRAM address the runtime can use */

static inline u32 ints_off(void) { u32 s; __asm__ volatile("mfc0 %0, $12" : "=r"(s)); __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u)); return s & 1u; }
static inline void ints_on(u32 m) { if (m) { u32 s; __asm__ volatile("mfc0 %0, $12" : "=r"(s)); __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s | 1u)); } }

/* ---- misc ------------------------------------------------------------------ */
void n64rt_osInitialize(void) { }
u32 n64rt_osGetMemSize(void) { return n64rt_game->mem_size; }
u32 n64rt_osVirtualToPhysical(u32 va)
{
    if ((va >> 30) == 2u) return va & 0x1FFFFFFFu;            /* KSEG0 / KSEG1 */
    u32 pa;
    if (n64rt_tlb_phys(va, &pa)) return pa;                   /* a page the game mapped (n64rt_os.c) */
    return 0xFFFFFFFFu;            /* libultra: the TLB probe finds nothing */
}
/* The D-cache writes through: memory is always current, and a line another
 * master overwrote is dropped by an invalidate. The I-cache: a fragment the
 * game has just loaded (and relocated) must be fetched anew. */
void n64rt_osInvalDCache(void *p, s32 n) { flush_cpu_dcache_range(p, (unsigned long)n); }
void n64rt_osWritebackDCache(void *p, s32 n) { (void)p; (void)n; }
void n64rt_osWritebackDCacheAll(void) { }
void n64rt_osInvalICache(void *p, s32 n) { (void)p; (void)n; n64rt_late_poll(); flush_cpu_icache(); }

/* ---- VI -------------------------------------------------------------------- */
/* The VI in hardware (MIRLO_VI, MONITOR.ID bit 3; rtl/soc/rcp_vi.sv; n64rt_vi_hw_event below): the game's accesses to the VI
 * are real bus accesses, the scan-out takes its base, gamma and black from the registers, and the runtime is woken (MONITOR.PEND
 * bit 1) only when the registers its window and size come from change. 0 on a core without it: everything is then emulated. */
static u32 s_vi_hw;
#ifdef N64RT_OWN
static void vi_hw_note_origin(void);
#endif
static struct { OSMesgQueue *mq; OSMesg msg; u32 retrace, count; } s_vi_ev;
static u32 s_fb_cur, s_fb_next, s_fb_prev;
/* The frame buffers the game has handed the VI lately (osViSwapBuffer, VI_ORIGIN): where the RDP draws.
 * A cartridge DMA into one of them waits for the RDP (pi_copy). */
static u32 s_fb_ring[4]; static u32 s_fb_ring_n;
static void fb_note(u32 fb)
{
    fb &= 0x007FFFFFu;
    for (u32 i = 0; i < 4u; i++) if (s_fb_ring[i] == fb) return;
    s_fb_ring[s_fb_ring_n++ & 3u] = fb;
}
u32 n64rt_swaps;                         /* (framebuffer swaps the scan-out took: a game's frames) */
static u32 s_tasks[3];
static u32 s_fields;
u32 n64rt_fields(void) { return s_fields; }             /* (fields so far: a debugging build's traces start at one) */
void n64rt_osCreateViManager(OSPri pri) { (void)pri; }
/* OSViMode: u8 type, then the common registers (ctrl, width, ...) */
static u32 s_vi_width = 320;
/* The scan-out's TV system (rtl/soc/n64_video.sv, rcp_vi.sv): WIDTH's bit 18 is the one it starts with -- the ROM's region
 * (n64rt_tv, 0 PAL) --, which the game's V_SYNC then overrides (625 lines: PAL, 525: NTSC); bit 19 of WIDTH, read, is the one
 * in use. A core without it reads 0 there and 59.94 Hz is what it has. */
#define TV_WIDTH_BIT (n64rt_tv == 0u ? 1u << 18 : 0u)
u32 n64rt_vi_pal(void) { return (video_framebuffer_width_read() >> 19) & 1u; }
/* osViBlack: taken at the next field, with the framebuffer the game swapped
 * to (n64rt_vi_field) -- as libultra applies both at the retrace. Taken at
 * once, the picture came back a frame before its framebuffer did: a frame of
 * whatever the last one pointed at (Pokemon Stadium, from its title to its
 * menu: a frame of noise). */
static u8 s_vi_black, s_vi_black_cur;
/* VI_CTRL's gamma correction: the mode's own (libultra's tables have it on)
 * until the game says (osViSetSpecialFeatures) -- the scan-out does it. */
static u32 s_vi_gamma;
/* What the game sets of the VI -- mode, framebuffer, black, gamma -- is the
 * next field's, as libultra's (its __osViNext, taken at the retrace). The
 * scan-out takes the framebuffer, its width (hi-res or not) and the picture's
 * on / off at its frame boundary, so those go to it together just before one
 * (n64rt_vi_field); the gamma correction it takes at once, so that goes just
 * after (n64rt_vi_after_boundary). Set when the game called, a new mode's
 * width met the framebuffer of the mode before for a frame (between Pokemon
 * Stadium's hi-res menus and its 3D screens: a frame of noise), and its gamma
 * a part of one (a band of the picture far too bright). */
static u32 s_vi_width_cur = 320, s_vi_gamma_cur;
/* What the scan-out shows (rtl/soc/n64_video.sv) is the VI's own: a 640-clock line, 240 lines, the picture's window in them
 * (H_START / H_END and V_START / V_END, from the VI's 108 and 37 -- PAL's 128 and 95), the framebuffer's pixel
 * (i X_SCALE) >> 10 at the window's clock i and its row (j Y_SCALE) >> 10 at its line j, the rows WIDTH pixels apart. The VI's
 * registers as the game wrote them (the software VI) or the hardware's (n64rt_vi_hw_event) are kept here, and given
 * to the scan-out with the field, in `vi_width_gamma`. A hi-res picture (WIDTH 512 or more) has X_SCALE 1.0 and 640 of its
 * pixels a line; an interlaced one (Y_SCALE 2.0, or WIDTH 1280 with 1.0: two rows' pitch) a field's rows, from the origin the
 * game wrote for the field (rtl/soc/n64_video.sv: 480i). */
#include "n64rt_vifit.h"
static u32 s_vi_l = 240;                /* the mode's window: the lines it shows (V_END - V_START) / 2 */
static u32 s_vi_ys = 0x400;             /* VI_Y_SCALE's step, 2.10 */
static u32 s_vi_xs = 0x200;             /* VI_X_SCALE's step, 2.10 */
static u32 s_vi_hs = 0x006C02ECu;       /* VI_H_START: start << 16 | end, in the VI's 640-pixel line (libultra's: 108 .. 748) */
static u32 s_vi_vs = 0x002501FFu;       /* VI_V_START: start << 16 | end, in half lines (37 .. 511) */
static u32 s_vi_tall_cur;               /* (the hi-res picture sent is 480 rows: no longer a thing) */
/* The TV system the game's registers ask for: V_SYNC's 525 lines (512..575) NTSC, 625 (576..703) PAL, any other value (0: none yet) the ROM's region -- what
 * rtl/soc/rcp_vi.sv's `vi_pal` is. NOT n64rt_vi_pal(), the system the scan-out is using (WIDTH bit 19): that follows at the scan-out's next frame boundary, so
 * the window of a mode's first registers (a PAL game's V_SYNC 0x271, V_START 0x5F0239, written once and never changed) was taken with NTSC's origin
 * (37: the picture 29 lines below its place, y0 29, and cut at the bottom, 211 of its 237 lines). */
static u32 s_vi_vsync;                  /* the last VI_V_SYNC that named a system (as rcp_vi.sv: any other value -- 0, noise -- changes nothing) */
static void vi_note_vsync(u32 v) { const u32 k = (v & 0x3FFu) >> 6; if (k == 8u || k == 9u || k == 10u) s_vi_vsync = v; }
static u32 vi_tv_pal(void)
{
    const u32 v = (s_vi_vsync & 0x3FFu) >> 6;
    return v == 8u ? 0u : (v == 9u || v == 10u) ? 1u : n64rt_tv == 0u;
}
/* the window the registers say (the TV system's own origins), and the scan-out's words for it */
static void vi_window(u32 *px0, u32 *pw, u32 *y0, u32 *rows)
{
    const u32 pal = vi_tv_pal();
    const u32 hb = pal ? 128u : 108u, vb = pal ? 95u : 37u;
    u32 hl = (s_vi_hs >> 16) & 0x3FFu, hr = s_vi_hs & 0x3FFu, vl = (s_vi_vs >> 16) & 0x3FFu, vr = s_vi_vs & 0x3FFu;
    *px0 = 0; *pw = 640; *y0 = 0; *rows = 0;
    if (hl < hb) hl = hb;
    if (hr > hb + 640u) hr = hb + 640u;
    if (hr > hl) { *px0 = hl - hb; *pw = hr - hl; }
    if (vr > vl) {
        u32 r = (vr - vl) / 2u, y = vl > vb ? (vl - vb) / 2u : 0u;
        if (y < 240u) { *y0 = y; *rows = r; }          /* (a window taller than the field from y0 is fitted to it: n64rt_vi_fit) */
    }
}
/* the bytes a picture of the VI's size takes from the origin: a row of the pitch, the rows its window steps through */
static u32 vi_pic_bytes(void)
{
    u32 w = s_vi_width_cur == ~0u ? 320u : s_vi_width_cur & ~1u, px0, pw, y0, rows;
    if (w < 2u) w = 320u;
    vi_window(&px0, &pw, &y0, &rows);
    if (!rows) rows = 240u;
    u32 ys = s_vi_ys > 0x800u ? 0x800u : s_vi_ys ? s_vi_ys : 0x400u;
    n64rt_vi_fit(y0, rows, ys, &rows, &ys);
    return w * 2u * (((rows * ys) >> 10) + 2u);
}
static void vi_width_gamma(void)
{
    u32 w = s_vi_width_cur & ~1u, px0, pw, y0, rows;
    if (w < 2u || w > 4094u) w = 320u;
    vi_window(&px0, &pw, &y0, &rows);
    const u32 xs = s_vi_xs & 0xFFFu ? (s_vi_xs > 0x400u ? 0x400u : s_vi_xs & 0xFFFu) : 0x200u;
    u32 ys = s_vi_ys > 0x800u ? 0x800u : s_vi_ys ? s_vi_ys : 0x400u;     /* (2.0 at most: a serrated picture's field, every other row) */
    n64rt_vi_fit(y0, rows, ys, &rows, &ys);
    /* the window's last pixel's source pixel, the priming's seven before it (rtl/soc/n64_video.sv NPE - NF), two to a word, and one more */
    const u32 nw = ((((pw - 1u) * xs) >> 10) + 8u) / 2u + 1u;
    s_vi_tall_cur = 0;
    video_framebuffer_scale_write(xs | ys << 16);
    video_framebuffer_rowwords_write(nw);
    video_framebuffer_hwindow_write(px0 | pw << 16);
    video_framebuffer_window_write(y0 << 8 | rows << 16);
    video_framebuffer_width_write(w | (s_vi_gamma_cur ? 1u << 16 : 0u) | TV_WIDTH_BIT);
}
void n64rt_osViSetMode(void *mode)
{
    u32 w = ((const u32 *)mode)[2];          /* comRegs.width */
    s_vi_width = w;
    {   /* the lines the mode's window shows (fldRegs[0].vStart: start << 16 | end, in half-lines) */
        const u32 *m = (const u32 *)mode;
        u32 l = ((m[12] & 0x3FFu) - ((m[12] >> 16) & 0x3FFu)) / 2u;
        u32 ys = m[11] & 0xFFFu;                                       /* fldRegs[0].yScale's step */
        u32 xs = m[8] & 0xFFFu;                                        /* comRegs.xScale's step */
        s_vi_l = l; if (ys) s_vi_ys = ys; if (xs) s_vi_xs = xs; s_vi_hs = m[7]; s_vi_vs = m[12]; s_vi_width_cur = ~0u;       /* (taken with the width, at the next field) */
    }
    s_vi_gamma = (((const u32 *)mode)[1] >> 3) & 1u;     /* comRegs.ctrl: VI_CTRL_GAMMA_ON */
    s_vi_black = 0;                          /* (libultra: a new mode's state is "mode updated" alone) */
    {   /* OSViMode: type (u8, padded), comRegs {ctrl, width, burst, vSync, hSync, leap, hStart, xScale,
         * vCurrent}, fldRegs[2] {origin, yScale, vStart, vBurst, vIntr} */
        const u32 *m = (const u32 *)mode;
        n64rt_log("n64rt: VI mode: type %lu ctrl %08lx width %lu xScale %08lx; field 0 origin %lu yScale %08lx, field 1 origin %lu yScale %08lx\n",
                  (unsigned long)(m[0] >> 24), (unsigned long)m[1], (unsigned long)w, (unsigned long)m[8],
                  (unsigned long)m[10], (unsigned long)m[11], (unsigned long)m[15], (unsigned long)m[16]);
    }
}
/* osViBlack: the picture off (the scan-out reads nothing, and what it does not
 * have shows black) -- games black the VI while its framebuffer is not one
 * yet (Pokemon Stadium points it at the top of its RDRAM between scenes) */
void n64rt_osViBlack(u8 on)
{
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: field %lu: osViBlack(%u)\n", (unsigned long)s_fields, (unsigned)on);
#endif
    s_vi_black = on != 0;
}
void n64rt_osViSetSpecialFeatures(u32 f)
{
    static u32 s_n;
    if (++s_n <= 4u) n64rt_log("n64rt: osViSetSpecialFeatures(%02lx)\n", (unsigned long)f);
    if (f & 0x01u) s_vi_gamma = 1;           /* OS_VI_GAMMA_ON */
    if (f & 0x02u) s_vi_gamma = 0;           /* OS_VI_GAMMA_OFF */
}
void n64rt_osViSetXScale(float x) { n64rt_log("n64rt: osViSetXScale(%d/1000)\n", (int)(x * 1000.0f)); }
void n64rt_osViSetYScale(float y) { n64rt_log("n64rt: osViSetYScale(%d/1000)\n", (int)(y * 1000.0f)); }
void n64rt_osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retrace)
{
    u32 m = ints_off();
    s_vi_ev.mq = mq; s_vi_ev.msg = msg; s_vi_ev.retrace = retrace ? retrace : 1; s_vi_ev.count = 0;
    ints_on(m);
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: field %lu: osViSetEvent(queue %08lx, message %08lx, every %lu)\n", (unsigned long)s_fields,
              (unsigned long)(uintptr_t)mq, (unsigned long)(uintptr_t)msg, (unsigned long)retrace);
#endif
}
void n64rt_osViSwapBuffer(void *fb)
{
    u32 m = ints_off();
#ifdef N64RT_DEBUG
    if ((u32)(uintptr_t)fb != s_fb_next) n64rt_log("n64rt: field %lu: osViSwapBuffer(%08lx)\n", (unsigned long)s_fields, (unsigned long)(uintptr_t)fb);
#endif
    s_fb_prev = s_fb_next;
    s_fb_next = (u32)(uintptr_t)fb;
    fb_note(s_fb_next);
    ints_on(m);
}
void *n64rt_osViGetCurrentFramebuffer(void) { return (void *)(uintptr_t)s_fb_cur; }
void *n64rt_osViGetNextFramebuffer(void) { return (void *)(uintptr_t)s_fb_next; }

/* A stretch of the game's code, compared every VI field with the copy taken
 * at boot (n64rt_watch_arm): Pokemon Stadium's func_800060E0 at 0x8000_6170
 * came back overwritten twice, with what looked like audio samples. Read
 * uncached (KSEG1): the audio core and MRDP write SDRAM behind the D-cache. */
#define WATCH_PA    0x00006000u
#define WATCH_WORDS 256u
static u32 s_watch[WATCH_WORDS];
static int s_watch_on;
void n64rt_watch_arm(void)
{
    const volatile u32 *m = (const volatile u32 *)(uintptr_t)(0xA0000000u | WATCH_PA);
    for (u32 i = 0; i < WATCH_WORDS; i++) s_watch[i] = m[i];
    s_watch_on = 1;
}
static void watch_check(void)
{
    const volatile u32 *m = (const volatile u32 *)(uintptr_t)(0xA0000000u | WATCH_PA);
    int n = 0;
    for (u32 i = 0; i < WATCH_WORDS; i++) {
        u32 v = m[i];
        if (v == s_watch[i]) continue;
        if (n++ < 8)
            n64rt_log("n64rt: WATCH %08lx: %08lx -> %08lx\n", (unsigned long)(0x80000000u | (WATCH_PA + 4u * i)),
                      (unsigned long)s_watch[i], (unsigned long)v);
        s_watch[i] = v;
    }
    if (n) {
        extern int n64rt_gfx_busy(void), n64rt_audio_busy(void);
        n64rt_log("n64rt: WATCH %d words changed at field %lu: tasks gfx %lu audio %lu other %lu, under way gfx %d audio %d, thread %ld\n",
                  n, (unsigned long)s_fields, (unsigned long)s_tasks[1], (unsigned long)s_tasks[2], (unsigned long)s_tasks[0],
                  n64rt_gfx_busy(), n64rt_audio_busy(), n64rt_cur ? (long)n64rt_cur->id : -1L);
    }
}

/* the frame's base to the scan-out. An interlaced hi-res picture is read as one frame of both fields, from the first
 * field's origin: libultra writes the two fields' origins one row of the buffer apart (1280 and 2560
 * bytes in, a field each -- the 240-row modes' too), so of the last two it is the lower one when they
 * are a row apart.
 * At the field (n64rt_vi_field: what the game said by osViSwapBuffer, taken at the retrace), and -- the game's own libultra
 * -- when the game writes VI_ORIGIN, which its retrace handler does (__osViSwapContext): the scan-out takes the base at
 * its frame boundary, and the handler runs before it; applied at the next field instead, a buffer the game has swapped in
 * (and so, as on the console, the one it no longer thinks is shown) would be on the screen a frame later: a frame
 * drawn into while it is scanned. */
static void vi_apply_base(void)
{
    u32 base = s_fb_next;
    if (s_vi_width_cur >= 512u && s_vi_width_cur != ~0u && s_fb_prev < s_fb_next) {
        /* (one row of the 640-pixel picture, 1280 bytes, apart: WIDTH 640's two rows' pitch is WIDTH * 2, WIDTH 1280's is WIDTH) */
        const u32 d = s_fb_next - s_fb_prev;
        if (d == s_vi_width_cur * 2u || (s_vi_width_cur >= 1024u && d == s_vi_width_cur)) base = s_fb_prev;
    }
    if (base != s_fb_cur) {
        s_fb_cur = base;
        /* the scan-out shows it from the next frame on (it latches the base
         * at its frame boundary) */
        video_framebuffer_dma_base_write(N64RT_RDRAM_SDRAM + (s_fb_cur & 0x7FFFFFu));
        static u32 s_swaps;
        if (++s_swaps <= 4u || (s_swaps & 255u) == 0u)
            n64rt_log("n64rt: VI field %lu: framebuffer %08lx (swap %lu)\n", (unsigned long)s_fields,
                      (unsigned long)s_fb_cur, (unsigned long)s_swaps);
    }
}

/* What a field is to the runtime that is not the scan-out's: its count, the debug log, the checks done once a second. With
 * the VI in hardware (s_vi_hw) no field wakes the monitor: the count is the uptime's, taken at the 4 ms ticks. */
static void vi_field_common(void)
{
    s_fields++;
    if (s_watch_on) watch_check();
#ifdef N64RT_STATS
    if (s_fields % 60u == 0u) { extern void n64rt_stats_log(u32); n64rt_stats_log(s_fields); }
#endif
#ifdef N64RT_DEBUG
    if (s_fields % 120u == 0u) {
        extern void n64rt_dump_threads(void);
        extern u32 n64rt_field_lock(void);
        extern const u32 *n64rt_field_stats(void);
        const u32 *fs = n64rt_field_stats();
        n64rt_log("n64rt: field %lu, tasks gfx %lu audio %lu; the scan-out's frame %lu cycles; fields on its boundary %lu, late %lu, none in reach %lu\n",
                  (unsigned long)s_fields, (unsigned long)s_tasks[1], (unsigned long)s_tasks[2], (unsigned long)n64rt_field_lock(),
                  (unsigned long)fs[0], (unsigned long)fs[1], (unsigned long)fs[2]);
        { extern void n64rt_field_late_log(void); n64rt_field_late_log(); }
        n64rt_dump_threads();
    }
#endif
}

/* (n64rt_tick, interrupts off) */
void n64rt_vi_field(void)
{
    vi_field_common();
    if (s_vi_width != s_vi_width_cur) { s_vi_width_cur = s_vi_width; vi_width_gamma(); }
    vi_apply_base();
    {
        /* Black: the game's (osViBlack), or a framebuffer that does not lie in
         * the game's memory -- between two screens Pokemon Stadium points the
         * VI at the last bytes of its 4 MiB with the picture left on: on the
         * console what is past the RDRAM reads as nothing, here it is the
         * rest of the SDRAM (never written: noise, for half a second). */
        u32 fb = s_fb_cur & 0x7FFFFFu, bytes = vi_pic_bytes();
        u8 black = s_vi_black || fb + bytes > n64rt_game->mem_size + 0x1000u;
        if (black != s_vi_black_cur) {               /* (with the base: a picture comes back on its own framebuffer) */
            s_vi_black_cur = black;
            video_framebuffer_dma_enable_write(black ? 0u : 1u);
        }
    }
    if (s_vi_ev.mq && ++s_vi_ev.count >= s_vi_ev.retrace) {
        s_vi_ev.count = 0;
        n64rt_send_isr(s_vi_ev.mq, s_vi_ev.msg);
    }
    n64rt_post_event(OS_EVENT_VI);
}

/* (n64rt_tick, the scan-out past its frame boundary) the gamma correction */
void n64rt_vi_after_boundary(void)
{
    if (s_vi_gamma != s_vi_gamma_cur) { s_vi_gamma_cur = s_vi_gamma; vi_width_gamma(); }
}

/* ---- PI: the ROM is in SDRAM; domain 2 is the cartridge's save ------------------
 *
 * Domain 2 (PI 0x0800_0000) is the cartridge's save memory: a battery-backed SRAM (32 KiB: Ocarina of Time) reached by
 * DMA at an offset -- PI_WR_LEN reads it into the RDRAM, PI_RD_LEN writes the RDRAM into it -- or a FlashRAM (128 KiB:
 * Pokemon Stadium...), which speaks a command protocol through its command register (0x0801_0000): read-array, status
 * and ID modes, sector and chip erase, page program from a 128-byte buffer the game DMAs in (as mupen64plus has it).
 * Array reads address 16-bit units: the ID is an old MX chip's, for which libultra's devAddr is page * 64. Domain 2 is
 * an SRAM until the game writes the command register (then a FlashRAM for good). Either is the Pocket's save slot
 * (data.json slot 1, LAYOUT_SAVE: 128 KiB kept for it), whose file size is set when the kind is known (d2_use), so the
 * Pocket writes 32 KiB or 128 KiB back; what the file did not have reads 0xFF (an erased FlashRAM). The page buffer and
 * the status words are at LAYOUT_SAVE_AUX. With the PI's domain 2 in hardware (MIRLO_PI2, MONITOR.ID bit 6;
 * rtl/soc/rcp_mi.sv) the engine copies to and from that memory, and the game's word reads and writes of domain 2 are the
 * hardware's -- the status word, once MONITOR.PI2 says FlashRAM -- so only a command (the core leaves 0x0801_xxxx to the
 * runtime) and a byte or half access trap. */
static u32 s_pi2_hw;                    /* MONITOR.ID bit 6: domain 2 and the cartridge's reads in hardware */
static void d2_use(int kind);
#define N64RT_SAVE_LOG n64rt_log
#include "n64rt_save.h"
static u32 s_save_fsz;                  /* the save slot's file size as the Pocket loaded it (n64rt_eeprom_init) */
static u32 s_save_ff;                   /* what is 0xFF from the file's end up to */
static void bridge_settle(void);
static void save_size(u32 bytes)
{
    u32 prev = apf_bridge_slot_id_read();
    apf_bridge_slot_id_write(1);
    bridge_settle();
    if (apf_bridge_file_size_read() < bytes) { apf_bridge_file_size_write(bytes); bridge_settle(); }
    apf_bridge_slot_id_write(prev);
}
/* domain 2 is used as an SRAM, or a FlashRAM: the slot's size, the rest of it erased, the hardware told */
static void d2_use(int kind)
{
    if ((int)s_d2 == kind || s_d2 == D2_FLASH) return;
    s_d2 = kind;
    const u32 bytes = kind == D2_FLASH ? FLASH_BYTES : SRAM_BYTES, from = s_save_fsz > s_save_ff ? s_save_fsz : s_save_ff;
    if (from < bytes) { memset(SAVE_MEM + from, 0xFF, bytes - from); s_save_ff = bytes; }
    save_size(bytes);
    if (kind == D2_FLASH) {
        fl_status(0u, 0u);
        if (s_pi2_hw) monitor_pi2_write(1u);
    }
    n64rt_log("n64rt: the cartridge's domain 2 is %s (the save slot: %lu bytes, %lu of them from the file)%s\n",
              kind == D2_FLASH ? "a FlashRAM" : "an SRAM", (unsigned long)bytes, (unsigned long)(s_save_fsz < bytes ? s_save_fsz : bytes),
              s_pi2_hw ? ", in hardware" : "");
}

/* the cartridge's handle: game code writes into it (Star Fox 64's
 * AudioLoad_Dma), so it lives in RDRAM (n64rt_rdram_alloc), not in the
 * runtime's own memory, which game code cannot reach */
static OSPiHandle *s_cart;
OSPiHandle *n64rt_osCartRomInit(void)
{
    if (!s_cart) s_cart = n64rt_rdram_alloc(sizeof *s_cart);
    s_cart->type = 0; s_cart->baseAddress = 0xB0000000u; s_cart->domain = 0;
    return s_cart;
}
void n64rt_osCreatePiManager(OSPri pri, OSMesgQueue *mq, OSMesg *msgs, s32 n) { (void)pri; (void)mq; (void)msgs; (void)n; }

static void copy_words(void *dst, const void *src, u32 size)
{
    if ((((uintptr_t)src | (uintptr_t)dst | size) & 3u) == 0) {
        const u32 *s = src; u32 *d = dst;
        for (u32 i = 0; i < size / 4u; i++) d[i] = s[i];
    } else {
        memcpy(dst, src, size);
    }
}
void n64rt_gfx_quiesce(void) __attribute__((weak));
/* does [pa, pa + size) touch a frame buffer (at its widest: 640 x 480 x 16 bits, 320 x 240 x 32)? */
static int fb_overlaps(u32 pa, u32 size)
{
#ifdef N64RT_OWN
    vi_hw_note_origin();                              /* (the VI in hardware: the runtime did not see the game's writes of ORIGIN) */
#endif
    const u32 bytes = s_vi_width_cur >= 512u && s_vi_width_cur != ~0u ? 640u * 480u * 2u : 320u * 240u * 4u;
    for (u32 i = 0; i < 4u; i++)
        if (s_fb_ring[i] && pa < s_fb_ring[i] + bytes && s_fb_ring[i] < pa + size) return 1;
    return 0;
}
/* a DMA between the RDRAM and domain 2, by the CPU (what the engine does not: n64rt_save.h's d2_engine_addr) */
static void d2_copy(u32 dev, u8 *d, u32 size, s32 dir)
{
    u32 off = ((dev & 0x1FFFFFFFu) - 0x08000000u) & (D2_WINDOW - 1u);
    if (s_d2 == D2_NONE) d2_use(D2_SRAM);
    if (s_d2 == D2_FLASH) {
        if (dir == OS_WRITE) { if (s_fl_mode == FL_WRITE) copy_words(FL_PAGE, d, size > 128u ? 128u : size); }
        else if (s_fl_mode == FL_STATUS || s_fl_mode == FL_ERASE) { u32 w[2] = { FL_STATW[0], FL_STATW[1] }; copy_words(d, w, size > 8u ? 8u : size); }
        else if (off * 2u + size <= FLASH_BYTES) { flush_cpu_dcache_range(SAVE_MEM + off * 2u, size); copy_words(d, SAVE_MEM + off * 2u, size); }
    } else {
        if (size > D2_WINDOW - off) size = D2_WINDOW - off;
        if (dir == OS_WRITE) memcpy(SAVE_MEM + off, d, size);
        else { flush_cpu_dcache_range(SAVE_MEM + off, size); memcpy(d, SAVE_MEM + off, size); }
    }
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 24u) n64rt_log("n64rt: domain 2 %s %lu bytes at %05lx (copied)\n", dir == OS_WRITE ? "write" : "read", (unsigned long)size, (unsigned long)off); }
#endif
}
static s32 pi_copy(u32 dev, void *dram, u32 size, s32 dir)
{
    u8 *d = K0P(dram);
    /* (the RDP's frame lands before the DMA's data, as it does on the N64 -- when the DMA is into what the RDP draws to. Not
     * for the rest: Star Fox 64's audio thread DMAs samples on demand, and an audio frame held up for a graphics task misses
     * its AI buffer.) */
    if (dir == OS_READ && n64rt_gfx_quiesce && fb_overlaps((u32)(uintptr_t)dram & 0x007FFFFFu, size)) n64rt_gfx_quiesce();
    if (is_flash(dev)) { d2_copy(dev, d, size, dir); return 0; }
    u32 off = dev & 0x0FFFFFFFu;                     /* (a cartridge address or an offset) */
#ifdef N64RT_DEBUG
    { static u32 s_n; extern u32 n64rt_fields(void); if (++s_n <= 24u || (s_n & (s_n - 1u)) == 0u)
        n64rt_log("n64rt: PI %s %lu (field %lu): dev %08lx -> %08lx, %lu bytes\n", dir == OS_READ ? "read" : "write", (unsigned long)s_n,
                  (unsigned long)n64rt_fields(), (unsigned long)dev, (unsigned long)(uintptr_t)dram, (unsigned long)size); }
#endif
    /* (a size that wraps the sum is no part of the ROM either) */
    if (dir != OS_READ || size > n64rt_game->rom_size || off > n64rt_game->rom_size - size) {
        n64rt_log("n64rt: PI %s dev %08lx size %lu: not the ROM -- ignored\n", dir == OS_READ ? "read" : "write",
                  (unsigned long)dev, (unsigned long)size);
        return -1;
    }
    n64rt_rom_copy(d, off, size);                    /* (through the window: the PSRAM's part and the SDRAM's) */
    n64rt_late_poll();
    return 0;
}
s32 n64rt_osEPiRawStartDma(OSPiHandle *h, s32 dir, u32 dev, void *dram, u32 size)
{
    return pi_copy((h ? h->baseAddress : 0) | dev, dram, size, dir);
}
s32 n64rt_osPiRawStartDma(s32 dir, u32 dev, void *dram, u32 size) { return pi_copy(dev, dram, size, dir); }
s32 n64rt_osEPiStartDma(OSPiHandle *h, OSIoMesg *mb, s32 dir)
{
    pi_copy((h ? h->baseAddress : 0) | mb->devAddr, mb->dramAddr, mb->size, dir);
    if (mb->hdr.retQueue) {
        extern s32 n64rt_osSendMesg(OSMesgQueue *, OSMesg, s32);
        n64rt_osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}
s32 n64rt_osPiStartDma(OSIoMesg *mb, s32 pri, s32 dir, u32 dev, void *dram, u32 size, OSMesgQueue *mq)
{
    (void)pri;
    pi_copy(dev, dram, size, dir);
    if (mq) {
        extern s32 n64rt_osSendMesg(OSMesgQueue *, OSMesg, s32);
        n64rt_osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}
static s32 io_read(u32 dev, u32 *data)
{
    if (is_flash(dev)) {
        if (s_d2 == D2_FLASH) *data = FL_STATW[0];
        else {
            u32 *w = (u32 *)(SAVE_MEM + (((dev & 0x1FFFFFFFu) - 0x08000000u) & (D2_WINDOW - 4u)));
            flush_cpu_dcache_range(w, 4u);
            *data = *w;
        }
        return 0;
    }
    u32 off = dev & 0x0FFFFFFFu;
    *data = off + 4 <= n64rt_game->rom_size ? n64rt_rom_word(off & ~3u) : 0;
    return 0;
}
static s32 io_write(u32 dev, u32 data)
{
    if (is_flash(dev)) {
        const u32 off = ((dev & 0x1FFFFFFFu) - 0x08000000u) & (D2_WINDOW - 1u);
        if (off == 0x10000u) flash_cmd(data);                        /* the FlashRAM's command register */
        else if (s_d2 != D2_FLASH) { d2_use(D2_SRAM); *(volatile u32 *)(SAVE_MEM + (off & ~3u)) = data; }
        return 0;
    }
    n64rt_log("n64rt: PI write %08lx to %08lx -- ignored\n", (unsigned long)data, (unsigned long)dev);
    return 0;
}
s32 n64rt_osEPiReadIo(OSPiHandle *h, u32 dev, u32 *data)     { return io_read((h ? h->baseAddress : 0) | dev, data); }
s32 n64rt_osEPiRawReadIo(OSPiHandle *h, u32 dev, u32 *data)  { return io_read((h ? h->baseAddress : 0) | dev, data); }
s32 n64rt_osEPiWriteIo(OSPiHandle *h, u32 dev, u32 data)     { return io_write((h ? h->baseAddress : 0) | dev, data); }
s32 n64rt_osEPiRawWriteIo(OSPiHandle *h, u32 dev, u32 data)  { return io_write((h ? h->baseAddress : 0) | dev, data); }
u32 n64rt_osPiGetStatus(void) { return 0; }
s32 n64rt_osPiRawReadIo(u32 dev, u32 *data) { return io_read(0xB0000000u | dev, data); }   /* (osRomBase | dev) */

/* ---- SI: controller 1 is the Pocket's (or the dock's first pad) --------------- */
#include "n64rt_pad.h"          /* the keys and the settings to the N64 controller (a pure function: host-tested) */
static void read_pad(OSContPad *p, int index)
{
    u32 k, j;
    switch (index) {
    case 0:  k = apf_input_cont1_key_read(); j = apf_input_cont1_joy_read(); break;
    case 1:  k = apf_input_cont2_key_read(); j = apf_input_cont2_joy_read(); break;
    case 2:  k = apf_input_cont3_key_read(); j = apf_input_cont3_joy_read(); break;
    default: k = apf_input_cont4_key_read(); j = apf_input_cont4_joy_read(); break;
    }
    n64rt_pad_map(k, j, apf_interact_interact0_read(), apf_interact_interact1_read(), &p->button, &p->stick_x, &p->stick_y);
    p->errnum = 0;
#ifdef N64RT_PADLOG
    /* (make PADLOG=1) what the dock sends and what the game gets, on the JTAG UART whenever a pad's words change, at most every 20 reads
     * of it: KEY_TYPE in the key word's top nibble, the stick bytes LX LY RX RY from the low byte up */
    {
        static u32 s_lk[4], s_lj[4], s_wait[4];
        if (s_wait[index]) s_wait[index]--;
        if ((k != s_lk[index] || j != s_lj[index]) && !s_wait[index]) {
            s_lk[index] = k; s_lj[index] = j; s_wait[index] = 20;
            n64rt_log("n64rt: pad %d key %08lx joy %08lx -> buttons %04x stick %d %d\n", index, (unsigned long)k, (unsigned long)j, p->button,
                      p->stick_x, p->stick_y);
        }
    }
#endif
}
/* a controller is there: the first always (the Pocket's own), the others
 * when the dock reports a pad */
static int pad_there(int i)
{
    if (i == 0) return 1;
    u32 k = i == 1 ? apf_input_cont2_key_read() : i == 2 ? apf_input_cont3_key_read() : apf_input_cont4_key_read();
    return KEY_TYPE(k) != 0u;
}
static u8 s_si_irq;                     /* the SI's interrupt, for a game that set it no message (below) */
#ifdef N64RT_OWN
static void mi_si_sync(int on);         /* (the MI in hardware keeps a copy of it) */
#else
#define mi_si_sync(on) ((void)0)
#endif
#if !defined(N64RT_SI_GAME) || N64RT_SI_GAME == 2
/* (a replaced function's: the SI's interrupt, and the thread it woke) */
static void si_done(void)
{
    u32 m = ints_off();
    if (!n64rt_post_event(OS_EVENT_SI)) { s_si_irq = 1; mi_si_sync(1); }
    ints_on(m);
    n64rt_resched();
}
#endif
#ifndef N64RT_SI_GAME                    /* (built with it, the SI's functions are the game's own: below) */
static void cont_status(OSContStatus *st)
{
    for (int i = 0; i < 4; i++) {
        int on = pad_there(i);
        st[i].type = on ? 0x0005u : 0;               /* CONT_TYPE_NORMAL */
        st[i].status = on && n64rt_pak_kind(i) != N64RT_PAK_NONE ? 1 : 0;   /* CONT_CARD_ON */
        st[i].errnum = on ? 0 : 8;                   /* CONT_NO_RESPONSE_ERROR */
    }
}
s32 n64rt_osContInit(OSMesgQueue *mq, u8 *bits, OSContStatus *st)
{
    (void)mq;
    *bits = (u8)(1u | (pad_there(1) ? 2u : 0u) | (pad_there(2) ? 4u : 0u) | (pad_there(3) ? 8u : 0u));
    cont_status(st);
    return 0;
}
s32 n64rt_osContStartQuery(OSMesgQueue *mq) { (void)mq; si_done(); return 0; }
void n64rt_osContGetQuery(OSContStatus *st) { cont_status(st); }
s32 n64rt_osContStartReadData(OSMesgQueue *mq) { (void)mq; si_done(); return 0; }
void n64rt_osContGetReadData(OSContPad *p)
{
    for (int i = 0; i < 4; i++) {
        if (pad_there(i)) read_pad(&p[i], i);
        else { p[i].button = 0; p[i].stick_x = p[i].stick_y = 0; p[i].errnum = 8; }
    }
}
/* the accessory's blocks (n64rt_pak.c), for the game's own osPfs* and
 * osGbpak*; 1 is PFS_ERR_NOPACK. A write below block 7 that is not forced is
 * left out, as libultra's (the Controller Pak's ID area). */
s32 n64rt___osContRamRead(OSMesgQueue *mq, int ch, u16 addr, u8 *buf)
{
    (void)mq;
    s32 r = n64rt_pak_rw(ch, (u32)addr << 5, buf, 0);
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 40u) n64rt_log("n64rt: pak read  %04lx: %02x %02x %02x %02x ... (%ld)\n", (unsigned long)addr << 5, buf[0], buf[1], buf[2], buf[3], (long)r); }
#endif
    return r;
}
s32 n64rt___osContRamWrite(OSMesgQueue *mq, int ch, u16 addr, u8 *buf, int force)
{
    (void)mq;
    if (force != 1 && addr < 7u && addr != 0u) return 0;
    s32 r = n64rt_pak_rw(ch, (u32)addr << 5, buf, 1);
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 40u) n64rt_log("n64rt: pak write %04lx: %02x %02x %02x %02x ... (%ld)\n", (unsigned long)addr << 5, buf[0], buf[1], buf[2], buf[3], (long)r); }
#endif
    return r;
}
s32 n64rt___osPfsGetStatus(OSMesgQueue *mq, int ch) { (void)mq; return n64rt_pak_kind(ch) != N64RT_PAK_NONE ? 0 : 1; }
/* (the game's own goes by its libultra's count of controllers, which the
 * game's osContInit would have set: this one is the runtime's) */
s32 n64rt_osPfsIsPlug(OSMesgQueue *mq, u8 *pattern)
{
    (void)mq;
    u8 bits = 0;
    for (int i = 0; i < 4; i++) if (pad_there(i) && n64rt_pak_kind(i) != N64RT_PAK_NONE) bits |= (u8)(1u << i);
    *pattern = bits;
    return 0;
}
#endif
#define EEPROM_SDRAM ((u32)LAYOUT_SAVE)   /* (the Pocket's save slot: below) */
#define EEPROM_BYTES 0x200u
#define EEPROM_16K   0x800u
/* The cartridge's EEPROM as the game finds it, for any game: the first command of the game's on the PIF's channel 4 says it
 * has one, and that is when it is given its part of the save slot (the file's size, what the file did not have 0xFF). Its SIZE
 * is 4 Kbit (512 bytes, blocks 0-63) or 16 Kbit (2,048 bytes, blocks 0-255), and nothing the PIF is asked says which a game
 * wants -- the status command, the first blocks, are the same requests --, while the games of libultra's older versions compare
 * the type their status command gets with the 4 Kbit one exactly (Pilotwings 64 shows "EEPROM CHECK FAILED", Super Mario 64,
 * Star Fox 64 and Mario Kart 64 never get past their first screen when it says 16 Kbit). So it is told from outside: by the
 * SAVE-TYPE DATABASE, a text file on the SD card (n64savedb.txt, data.json's slot 4, n64rt_savedb.h) with a line per
 * cartridge ID, looked up once at boot with the ID in the ROM's header (eep_db_init) -- no game is named here, and a game that
 * is not in the file, or no file, is a 4 Kbit chip. There is no setting to override it (the core had an "EEPROM size" one, Auto /
 * 4 Kbit / 16 Kbit, interact field 8: removed -- the database decides; fix a game's entry in the file).
 * A block of 64 or more, which only a 16 Kbit game is asked for, makes the slot 2,048 bytes whatever the database says.
 * For tests and for trying a game, at build time only: N64RT_EEP16K forces 16 Kbit (make EEP16K=1); N64RT_EEP_FORCE (an
 * expression: 1 the 4 Kbit chip, 2 the 16 Kbit one, else the database) stands for a database in a simulation. */
static int s_eep_db_16k;                /* the database says 16 Kbit */
static inline int eep_16k(void)
{
#ifdef N64RT_EEP16K
    return 1;
#else
#ifdef N64RT_EEP_FORCE
    const u32 f = (N64RT_EEP_FORCE);
    return f == 2u ? 1 : f == 1u ? 0 : s_eep_db_16k;
#else
    return s_eep_db_16k;
#endif
#endif
}
/* The database (data.json's slot 4, LAYOUT_SAVEDB: the Pocket put the file there before the program ran; no file, size 0)
 * against the cartridge ID and version of the ROM's header. */
static void eep_db_init(void)
{
    u32 prev = apf_bridge_slot_id_read();
    apf_bridge_slot_id_write(LAYOUT_SAVEDB_SLOT);
    bridge_settle();
    u32 len = apf_bridge_file_size_read();
    apf_bridge_slot_id_write(prev);
    s_eep_db_16k = 0;
    if (len == 0) { n64rt_log("n64rt: no save-type database: the EEPROM is 4 Kbit\n"); return; }
    if (len > LAYOUT_SAVEDB_MAX) len = LAYOUT_SAVEDB_MAX;
    const u32 w = n64rt_rom_word(0x38u), v = n64rt_rom_word(0x3Cu);       /* 0x3B..0x3E: the ID, 0x3F: the version */
    const unsigned char id[4] = { (unsigned char)w, (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8) };
    const int kind = n64rt_savedb_lookup((const char *)(uintptr_t)LAYOUT_SAVEDB, len, id, v & 0xFFu);
    s_eep_db_16k = kind == N64RT_SAVEDB_EEP16K;
    static const char *const names[] = { "not in the file", "eeprom4k", "eeprom16k", "sram", "flash", "none" };
    n64rt_log("n64rt: save-type database of %lu bytes: %c%c%c%c version %lu is %s\n", (unsigned long)len,
           id[0] >= 32 && id[0] < 127 ? id[0] : '?', id[1] >= 32 && id[1] < 127 ? id[1] : '?', id[2] >= 32 && id[2] < 127 ? id[2] : '?',
           id[3] >= 32 && id[3] < 127 ? id[3] : '?', (unsigned long)(v & 0xFFu), names[kind]);
}
static u32 s_eep_bytes;                 /* the EEPROM's part of the slot in use (0: the game has not spoken to one) */
static void eep_use(u32 bytes)
{
    if (s_eep_bytes >= bytes) return;
    const u32 from = s_save_fsz > s_save_ff ? s_save_fsz : s_save_ff;
    if (from < bytes) { memset(SAVE_MEM + from, 0xFF, bytes - from); s_save_ff = bytes; }
    save_size(bytes);
    n64rt_log("n64rt: the cartridge's EEPROM is used (the save slot: %lu bytes, %lu of them from the file)\n", (unsigned long)bytes,
              (unsigned long)(s_save_fsz < bytes ? s_save_fsz : bytes));
    s_eep_bytes = bytes;
}
/* The PIF, for a game whose own osCont* / osEeprom* run (a libultra not
 * recognised function by function, or a runtime built with N64RT_SI_GAME):
 * its joybus commands done here. The 64-byte block holds them -- per channel
 * {tx bytes, rx bytes, command, ..., reply}; 0x00 skips a channel, 0xFD
 * resets one, 0xFE ends, 0xFF pads. A write of the block with bit 0 of its
 * last byte says it holds commands; they run at every read of it from then
 * on, as the PIF's (libultra writes its controller request once and then
 * only reads, a frame after the other), and the read gives the block back
 * with the replies.
 *   controllers (channels 0-3): 0x00 / 0xFF status, 0x01 buttons and stick,
 *       0x02 / 0x03 a block of the accessory read / written (n64rt_pak.c)
 *   the cartridge (channel 4): 0x00 status (a 4 Kbit EEPROM), 0x04 / 0x05 a
 *       block of 8 bytes read / written */
static u32 s_pif_w[16];                 /* (as words: the SI moves it so, and a store is a write to the SDRAM) */
static u8 *const s_pif = (u8 *)s_pif_w;
static u8 s_pif_joy;                    /* the block holds commands */
/* How long the console's PIF takes over the block, from the SI's read of it to its interrupt, in RCP cycles (62.5 MHz;
 * pif_run's value, which the SI in hardware waits out): measured on the console, 15,030 for a block that is only its end
 * mark, 37,987 for one controller read, 97,890 for four with one controller there -- 13,600, then 1,420 for each byte that
 * is no command (0x00, 0xFD, 0xFF, the end), 22,000 for a command a device answers, 18,000 for one nothing answers (the
 * timeout), 20,000 for the cartridge's; a command longer than a controller read's 5 bytes (a pak's block, 36) adds the
 * joybus's 32 us (2,000) for each byte more. */
#define PIF_T0    13600u
#define PIF_TSKIP 1420u
#define PIF_TCMD(ok, ch, n) (((ch) == 4 ? 20000u : (ok) ? 22000u : 18000u) + ((n) > 5u ? ((n) - 5u) * 2000u : 0u))
static u32 pif_run(void)
{
    int ch = 0;
    u32 t = PIF_T0;
    for (int i = 0; i < 63 && ch < 6; ) {
        u8 tx = s_pif[i];
        if (tx == 0xFEu) { t += PIF_TSKIP; break; }
        if (tx == 0xFFu) { t += PIF_TSKIP; i++; continue; }
        if (tx == 0x00u || tx == 0xFDu) { t += PIF_TSKIP; ch++; i++; continue; }
        tx &= 0x3Fu;
        s_pif[i + 1] &= 0x3Fu;                       /* (the run before: nothing had answered) */
        u8 rx = s_pif[i + 1];
        u8 *cmd = &s_pif[i + 2], *rep = cmd + tx;
        if (i + 2 + tx + rx > 63) break;
        int ok = 0;
        if (ch < 4 && pad_there(ch)) {
            const int pak = n64rt_pak_kind(ch) != N64RT_PAK_NONE;
            if ((cmd[0] == 0x00u || cmd[0] == 0xFFu) && rx >= 3) { rep[0] = 0x05; rep[1] = 0x00; rep[2] = pak ? 0x01 : 0x02; ok = 1; }   /* a pad, its pak */
            else if (cmd[0] == 0x02u && tx >= 3 && rx >= 33 && pak) {  /* (no pak: not answered, as it never was) */
                n64rt_pak_rw(ch, (u32)cmd[1] << 8 | cmd[2], rep, 0);
                rep[32] = n64rt_pak_crc(rep); ok = 1;
#ifdef N64RT_DEBUG
                { static u32 s_n; if (s_n++ < 40u) n64rt_log("n64rt: PIF: pak read  %04x: %02x %02x %02x %02x ...\n", (cmd[1] << 8 | cmd[2]) & 0xFFE0, rep[0], rep[1], rep[2], rep[3]); }
#endif
            } else if (cmd[0] == 0x03u && tx >= 35 && rx >= 1 && pak) {
                n64rt_pak_rw(ch, (u32)cmd[1] << 8 | cmd[2], &cmd[3], 1);
                rep[0] = n64rt_pak_crc(&cmd[3]); ok = 1;
#ifdef N64RT_DEBUG
                { static u32 s_n; if (s_n++ < 40u) n64rt_log("n64rt: PIF: pak write %04x: %02x %02x %02x %02x ...\n", (cmd[1] << 8 | cmd[2]) & 0xFFE0, cmd[3], cmd[4], cmd[5], cmd[6]); }
#endif
            }
            else if (cmd[0] == 0x01u && rx >= 4) {
                OSContPad p; read_pad(&p, ch);
                rep[0] = (u8)(p.button >> 8); rep[1] = (u8)p.button; rep[2] = (u8)p.stick_x; rep[3] = (u8)p.stick_y; ok = 1;
            }
        } else if (ch == 4) {
            volatile u8 *e = (volatile u8 *)(uintptr_t)EEPROM_SDRAM;
            const int k16 = eep_16k();
            eep_use(k16 ? EEPROM_16K : EEPROM_BYTES);
            if (cmd[0] == 0x00u && rx >= 3) { rep[0] = 0x00; rep[1] = k16 ? 0xC0 : 0x80; rep[2] = 0x00; ok = 1; }   /* (status: the type) */
            else if (cmd[0] == 0x04u && tx >= 2 && rx >= 8) {
                if (cmd[1] >= 64u) eep_use(EEPROM_16K);
                for (int k = 0; k < 8; k++) rep[k] = e[cmd[1] * 8u + (u32)k];
                ok = 1;
            }
            else if (cmd[0] == 0x05u && tx >= 10 && rx >= 1) {
                if (cmd[1] >= 64u) eep_use(EEPROM_16K);
                for (int k = 0; k < 8; k++) e[cmd[1] * 8u + (u32)k] = cmd[2 + k];
                rep[0] = 0x00; ok = 1;
            }
#ifdef N64RT_DEBUG
            { static int s_blk; if (cmd[0] == 0x00u && s_blk++ < 2) {
                n64rt_log("n64rt: PIF block at the EEPROM's status (channel 4: tx %u rx %u):", tx, rx);
                for (int w = 0; w < 16; w++) n64rt_log(" %08lx", (unsigned long)s_pif_w[w]);
                n64rt_log("\n"); } }
            { static u32 s_n[6]; u32 c = cmd[0] < 6u ? cmd[0] : 1u; if (++s_n[c] <= 4u || (s_n[c] & (s_n[c] - 1u)) == 0u)
                n64rt_log("n64rt: PIF: EEPROM command %u, block %u (%lu of them)\n", cmd[0], cmd[0] >= 4u ? cmd[1] : 0u, (unsigned long)s_n[c]); }
#endif
        }
        if (!ok) s_pif[i + 1] |= 0x80u;              /* nothing answered */
        t += PIF_TCMD(ok, ch, (u32)tx + rx);
        i += 2 + tx + rx;
        ch++;
    }
    return t;
}
/* The challenge of a CIC-6105 game (bit 1 of the status byte): the PIF sends the 30 nibbles of bytes 48-62 to the cartridge's
 * CIC and puts the CIC's answer in their place, a nibble for each, the first nibble the high one; bytes 46 and 47 and the
 * status byte are left 0. The CIC answers each nibble c with r = key + 5 c (mod 16); the next key is r's entry in one of two
 * tables, and which table follows depends on r. The game sends the challenge when it chooses and checks the answer itself
 * (one that gets its own challenge back, unanswered, goes on to fail later). Another CIC answers nothing: the block is left
 * as it is. */
static void pif_challenge(void)
{
    static const u8 lut[2][16] = {
        { 0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9 },
        { 0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9 } };
    s_pif[63] = 0;
    if (!n64rt_game || n64rt_game->cic != 6105u) return;
    u32 key = 0xBu, t = 0;
    for (u32 i = 0; i < 30u; i++) {
        u8 *b = &s_pif[48u + i / 2u];
        const u32 sh = i & 1u ? 0u : 4u, r = (key + 5u * ((*b >> sh) & 0xFu)) & 0xFu;
        *b = (u8)((*b & ~(0xFu << sh)) | r << sh);
        key = lut[t][r];
        const u32 neg = r >> 3, mag = (neg ? ~r : r) & 7u;
        u32 nt = mag % 3u == 1u ? neg : 1u - neg;
        if (t && (r == 0x1u || r == 0x9u)) nt = 1;
        if (t && (r == 0xBu || r == 0xEu)) nt = 0;
        t = nt;
    }
    s_pif[46] = s_pif[47] = 0;
}
/* the SI's two transfers of the block: into the PIF, and out of it */
static void pif_put(const void *b)
{
    copy_words(s_pif_w, b, 64);
    if (s_pif[63] & 1u) s_pif_joy = 1;
    if (s_pif[63] & 2u) pif_challenge();
    s_pif[63] = 0;                                   /* (taken: a game that looks finds nothing pending) */
}
static void pif_get(void *b)
{
    if (s_pif_joy) (void)pif_run();
    copy_words(b, s_pif_w, 64);
}

/* ---- the SI itself: a game whose own libultra drives it ----------------------
 * Everything on the joybus -- controllers, their accessories, the cartridge's
 * EEPROM -- goes through four registers and the PIF's 64 bytes, a handful of
 * accesses a frame. They trap (n64rt_os.c emulate_hw) and are done here, so
 * libultra's osContInit, osContStartReadData, osEeprom*, osPfs*, osGbpak*,
 * osMotor* and what they stand on (__osSiRawStartDma, __osSiDeviceBusy, its
 * access queue) can be left as the game has them: nothing of theirs to find
 * in a ROM, none of their variables to keep on the game's behalf. A runtime
 * built with N64RT_SI_GAME (make SI_GAME=1) replaces none of them; SI_GAME=2
 * replaces __osSiRawStartDma alone, where a game's list has it (above), and
 * a game in which it was not found still runs, on the registers.
 *   SI_DRAM_ADDR         the block's place in RDRAM
 *   SI_PIF_ADDR_RD64B    written: the PIF's block to RDRAM (its commands run)
 *   SI_PIF_ADDR_WR64B    written: RDRAM's block to the PIF
 *   SI_STATUS            never busy; bit 12 the interrupt, any write clears it
 * A transfer is done at once and ends in the SI's interrupt: the message of
 * the game's osSetEventMesg(OS_EVENT_SI), which libultra's own handler would
 * send -- the interrupts and the threads stay the runtime's. A game that set
 * none (it polls SI_STATUS) finds the interrupt's bit there, and in MI_INTR.
 * PIF RAM can be read and written a word at a time too (libultra's
 * __osSiRawReadIo / WriteIo). */
#define SI_DRAM_ADDR      0x04800000u
#define SI_PIF_ADDR_RD64B 0x04800004u
#define SI_PIF_ADDR_WR64B 0x04800010u
#define SI_STATUS         0x04800018u
#define PIF_RAM           0x1FC007C0u
static u32 s_si_dram;
/* The SI and the PIF RAM in hardware (MIRLO_SI, MONITOR.ID bit 5; rtl/soc/rcp_si.sv; n64rt_si_hw_event below): the game's word
 * accesses to the SI's registers and to PIF RAM are real bus accesses, the 64-byte DMA and the interrupt are the hardware's, and
 * the runtime is woken (MONITOR.PEND bit 4) only to run the PIF's commands. What traps in that case (a load of a byte or half: the
 * pass-through takes words only) is read from the hardware. 0 on a core without it: the emulation above and below. */
#ifdef N64RT_OWN
static u32 s_si_hw;
static inline u32 hw_rd(u32 pa);
static inline void hw_wr(u32 pa, u32 v);
#define SI_HW_PA(pa) (((pa) >= 0x04800000u && (pa) < 0x04900000u) || ((pa) >= PIF_RAM && (pa) < PIF_RAM + 64u))
#else
#define s_si_hw 0u
#define hw_rd(pa) 0u
#define hw_wr(pa, v) ((void)0)
#define SI_HW_PA(pa) 0
#endif
#if !defined(N64RT_SI_GAME) || N64RT_SI_GAME == 2
s32 n64rt___osSiRawStartDma(s32 dir, void *buf)
{
    if (dir) pif_put(buf);
    else pif_get(buf);
    si_done();
    return 0;
}
#endif
/* (the exception's handler: interrupts off, and it picks the thread to run on its way out) */
static void si_dma(int write)
{
    if (s_si_dram <= n64rt_game->mem_size - 64u) {
        if (write) pif_put(K0P(s_si_dram));
        else pif_get(K0P(s_si_dram));
    }
    if (!n64rt_post_event(OS_EVENT_SI)) { s_si_irq = 1; mi_si_sync(1); }
}
static int si_read(u32 pa, u32 *v)
{
    if (s_si_hw && SI_HW_PA(pa)) { *v = hw_rd(pa); return 1; }
    if (pa >= PIF_RAM && pa < PIF_RAM + 64u) { *v = s_pif_w[(pa - PIF_RAM) >> 2]; return 1; }
    switch (pa) {
    case SI_STATUS:    *v = s_si_irq ? 0x1000u : 0u; return 1;
    case SI_DRAM_ADDR: *v = s_si_dram; return 1;
    default: return 0;
    }
}
static int si_write(u32 pa, u32 v)
{
    if (s_si_hw && SI_HW_PA(pa)) { hw_wr(pa, v); return 1; }
    if (pa >= PIF_RAM && pa < PIF_RAM + 64u) {
        s_pif_w[(pa - PIF_RAM) >> 2] = v;
        if (pa == PIF_RAM + 60u) { if (v & 1u) s_pif_joy = 1; if (v & 2u) pif_challenge(); s_pif[63] = 0; }
        return 1;
    }
    switch (pa) {
    case SI_DRAM_ADDR:      s_si_dram = v & 0x00FFFFFFu; return 1;
    case SI_PIF_ADDR_RD64B: si_dma(0); return 1;
    case SI_PIF_ADDR_WR64B: si_dma(1); return 1;
    case SI_STATUS:         s_si_irq = 0; mi_si_sync(0); return 1;
    default: return 0;
    }
}
#define PFS_ERR_NOPACK 1
/* ---- EEPROM: the Pocket's save slot ------------------------------------------
 * A 4-Kbit EEPROM (64 blocks of 8 bytes) on data.json's slot 1 ("Save":
 * nonvolatile, 0x200 bytes at bridge 0x07320000 = SDRAM 0x43320000, in the
 * saves' part of the SDRAM -- lang/mips/include/layout.h; 0xFF when there is
 * no file yet, an erased EEPROM). The Pocket writes the .sav there before the
 * core starts and reads it back into the file when the core quits or the
 * Pocket sleeps, so the game just uses that memory (as Super Mirlo 64's
 * hal/os_hal.c). It writes back as many bytes as the slot's file size in the
 * core's table, 0 when there was no file: n64rt_eeprom_init() sets it to 512 (at least: an SRAM's or a
 * FlashRAM's file is bigger, d2_use). */
#define EEPROM_TYPE_4K 1
static void bridge_settle(void) { for (volatile int i = 0; i < 256; i++) { } }
static void n64rt_eeprom_init(void)
{
    u32 prev = apf_bridge_slot_id_read();
    apf_bridge_slot_id_write(1);
    bridge_settle();
    s_save_fsz = apf_bridge_file_size_read();
#ifndef N64RT_SI_GAME
    /* (the replaced osEeprom* functions are a 4 Kbit EEPROM from the start; the game's own go through eep_use, when the game first uses one) */
    if (s_save_fsz < EEPROM_BYTES) {          /* (a bigger file is an SRAM's or a FlashRAM's: kept whole) */
        apf_bridge_file_size_write(EEPROM_BYTES);
        bridge_settle();
    }
#endif
    apf_bridge_slot_id_write(prev);
}
#ifndef N64RT_SI_GAME
s32 n64rt_osEepromProbe(OSMesgQueue *mq) { (void)mq; return EEPROM_TYPE_4K; }
s32 n64rt_osEepromLongRead(OSMesgQueue *mq, u8 a, u8 *b, int n)
{
    (void)mq;
    const volatile u8 *e = (const volatile u8 *)(uintptr_t)EEPROM_SDRAM;
    for (int i = 0; i < n; i++) { u32 x = (u32)a * 8u + (u32)i; b[i] = x < EEPROM_BYTES ? e[x] : 0xFFu; }
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 12u && n >= 4) n64rt_log("n64rt: EEPROM read  block %u, %d bytes: %02x %02x %02x %02x ...\n", a, n, b[0], b[1], b[2], b[3]); }
#endif
    return (u32)a * 8u + (u32)n <= EEPROM_BYTES ? 0 : -1;
}
s32 n64rt_osEepromLongWrite(OSMesgQueue *mq, u8 a, u8 *b, int n)
{
    (void)mq;
    volatile u8 *e = (volatile u8 *)(uintptr_t)EEPROM_SDRAM;     /* (the D-cache writes through) */
    for (int i = 0; i < n; i++) { u32 x = (u32)a * 8u + (u32)i; if (x < EEPROM_BYTES) e[x] = b[i]; }
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 12u && n >= 4) n64rt_log("n64rt: EEPROM write block %u, %d bytes: %02x %02x %02x %02x ...\n", a, n, b[0], b[1], b[2], b[3]); }
#endif
    return (u32)a * 8u + (u32)n <= EEPROM_BYTES ? 0 : -1;
}
s32 n64rt_osEepromRead(OSMesgQueue *mq, u8 a, u8 *b) { return n64rt_osEepromLongRead(mq, a, b, 8); }
s32 n64rt_osEepromWrite(OSMesgQueue *mq, u8 a, u8 *b) { return n64rt_osEepromLongWrite(mq, a, b, 8); }
/* the Rumble Pak: none (the Pocket and its dock give a core nothing to vibrate; osMotorInit fails, so the game never starts it) */
s32 n64rt_osMotorInit(OSMesgQueue *mq, void *pfs, int ch) { (void)mq; (void)pfs; (void)ch; return PFS_ERR_NOPACK; }
s32 n64rt_osMotorStart(void *pfs) { (void)pfs; return PFS_ERR_NOPACK; }
s32 n64rt_osMotorStop(void *pfs) { (void)pfs; return PFS_ERR_NOPACK; }
#endif
void n64rt_osViRepeatLine(u8 on) { (void)on; }

int n64rt_other_task(OSTask *t) __attribute__((weak));
int n64rt_other_task(OSTask *t) { (void)t; return 0; }

/* ---- the RSP's tasks: reported, finished at once ----------------------------------- */
/* (the task is taken at osSpTaskStartGo -- or, from a game that starts the RSP
 * itself, at its write of SP_STATUS that clears halt: n64rt_hw_write) */
static OSTask *s_task_loaded;
void n64rt_osSpTaskLoad(OSTask *t) { s_task_loaded = t; }
/* An RSP task runs on the runtime's own stack, interrupts masked (nothing
 * else may enter that stack); the VI's tick waits, its interrupt pending. */
void n64rt_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);
static u8 s_task_stack[32768] __attribute__((aligned(16)));
static void task_body(void *p);
static int s_async;                          /* task_body: the task finishes later (polled) */
int n64rt_tasks_busy(void);
void n64rt_osSpTaskStartGo(OSTask *t)
{
    u32 m = ints_off();
    s_task_loaded = 0;
    s_async = 0;
    n64rt_call_on_stack(task_body, t, s_task_stack + sizeof s_task_stack);
    if (!s_async) n64rt_post_event(OS_EVENT_SP);
    extern void n64rt_rearm(void);
    if (n64rt_tasks_busy()) n64rt_rearm();   /* poll what is under way from now on */
    ints_on(m);
    n64rt_resched();
}
/* (n64rt_tick, interrupts off) the RSP's and RDP's events of tasks under way */
u32 n64rt_gfx_poll(void);
u32 n64rt_audio_poll(void);
int n64rt_gfx_busy(void);
int n64rt_audio_busy(void);
int n64rt_tasks_busy(void) { return n64rt_gfx_busy() || n64rt_audio_busy(); }
static void tasks_post(u32 ev)
{
    if (ev & (1u << OS_EVENT_SP)) n64rt_post_event(OS_EVENT_SP);
    if (ev & (1u << OS_EVENT_DP)) n64rt_post_event(OS_EVENT_DP);
}
void n64rt_tasks_poll(void) { tasks_post(n64rt_gfx_poll() | n64rt_audio_poll()); }
/* (n64rt_tick: the two halves apart -- the audio's is the long one) */
void n64rt_tasks_events(void) { tasks_post(n64rt_gfx_poll()); }
void n64rt_tasks_feed(void) { tasks_post(n64rt_audio_poll()); }
static void task_body(void *p)
{
    OSTask *t = p;
    u32 type = t->t.type;
#ifdef N64RT_RSPDUMP
    /* (sim/mips_soc RSPDUMP=<dir>: the RDRAM and this task, as the RSP would
     * start it -- the reference model's input, sim/rsp) */
    ctrl_scratch_write(0xD0000000u | N64RT_PHYS(t));
#endif
    u32 k = type == M_GFXTASK ? 1u : type == M_AUDTASK ? 2u : 0u;
    if (++s_tasks[k] <= 3u || (s_tasks[k] & 255u) == 0u)
        n64rt_log("n64rt: %s task %lu: data %08lx size %lu ucode %08lx\n",
                  k == 1 ? "gfx" : k == 2 ? "audio" : "other", (unsigned long)s_tasks[k],
                  (unsigned long)(uintptr_t)t->t.data_ptr, (unsigned long)t->t.data_size,
                  (unsigned long)(uintptr_t)t->t.ucode);
#ifdef N64RT_RSP
    /* Mirlo's RSP (n64rt_rsp.c): every task is its own microcode's, whatever its kind */
    {
        extern void n64rt_gfx_task(OSTask *);
        n64rt_gfx_task(t);
        s_async = 1;
        return;
    }
#endif
    if (k == 1) {
        extern void n64rt_gfx_task(OSTask *);
        n64rt_gfx_task(t);
        extern const int n64rt_gfx_sp_early;
        s_async = !n64rt_gfx_sp_early;       /* its SP now (the list is translated), or once the geom core has walked it */
    } else if (k == 2) {
        extern int n64rt_audio_task(OSTask *);
        s_async = n64rt_audio_task(t);
    } else if (k == 0) {
        /* another microcode's task (a JPEG decoder...): the game's port may
         * know it (n64rt_other_task); else it just ends */
        if (!n64rt_other_task(t) && s_tasks[0] == 1)
            n64rt_log("n64rt: a task this runtime does not know (ucode %08lx)\n", (unsigned long)(uintptr_t)t->t.ucode);
    }
}
#ifndef N64RT_RSP                        /* (n64rt_rsp.c has the real ones) */
void n64rt_osSpTaskYield(void) { }
u32 n64rt_osSpTaskYielded(OSTask *t) { (void)t; return 0; }
#endif
void n64rt___osSpSetPc(u32 pc) { (void)pc; }
void n64rt_osDpSetStatus(u32 s) { (void)s; }
void n64rt_osDpGetCounters(u32 *c) { c[0] = c[1] = c[2] = c[3] = 0; }
void n64rt_osAfterPreNMI(void) { }
/* the 64DD (libleo): not there */
s32 n64rt_LeoDriveExist(void) { return 0; }

/* ---- libultra's 64-bit integer helpers (long long in register pairs, as in
 * IDO's o32; libultra's quirks kept: __ll_rem is an unsigned remainder,
 * __ull_divremi's divisor is a halfword) ------------------------------------------------ */
s64 n64rt___ll_div(s64 a, s64 b) { return a / b; }
s64 n64rt___ll_mul(s64 a, s64 b) { return (s64)((u64)a * (u64)b); }
u64 n64rt___ull_div(u64 a, u64 b) { return a / b; }
u64 n64rt___ull_rem(u64 a, u64 b) { return a % b; }
s64 n64rt___ll_rem(u64 a, s64 b) { return (s64)(a % (u64)b); }
s64 n64rt___ll_mod(s64 a, s64 b)
{
    s64 r = a % b;
    if ((r < 0 && b > 0) || (r > 0 && b < 0)) r += b;
    return r;
}
s64 n64rt___ll_lshift(s64 a, s64 b) { return (s64)((u64)a << (b & 63)); }
s64 n64rt___ll_rshift(s64 a, s64 b) { return a >> (b & 63); }
u64 n64rt___ull_rshift(u64 a, u64 b) { return a >> (b & 63); }
void n64rt___ull_divremi(u64 *q, u64 *r, u64 a, s16 b)
{
    u64 d = (u64)(s64)b;
    *q = a / d; *r = a % d;
}

/* ---- the registers a game reads or writes itself (n64rt_os.c emulate_hw) ---- */
static void hw_note(const char *what, u32 addr)
{
    static u32 s_seen[16]; static int s_n;
    for (int i = 0; i < s_n; i++) if (s_seen[i] == addr) return;
    if (s_n >= 16) return;                       /* (told of sixteen: enough) */
    s_seen[s_n++] = addr;
    n64rt_log("n64rt: the game %s %08lx itself (emulated)\n", what, (unsigned long)addr);
}
static void si_note(void)
{
    static u8 said;
    if (said) return;
    said = 1;
    n64rt_log("n64rt: the game drives the SI itself (its registers and the PIF: emulated)\n");
}
u32 n64rt_osAiGetLength(void);
u32 n64rt_osAiGetStatus(void);
#ifdef N64RT_OWN
static int rcp_read(u32 pa, u32 *v);
static int rcp_write(u32 pa, u32 v);
#endif
int n64rt_hw_read(u32 addr, u32 *v)
{
    u32 pa = addr & 0x1FFFFFFFu;
#ifdef N64RT_OWN
    if (rcp_read(pa, v)) return 1;
#endif
    if (pa >= 0x10000000u && pa < 0x1FC00000u) {                /* the cartridge: the ROM */
        u32 off = pa - 0x10000000u;
        *v = off < n64rt_game->rom_size ? n64rt_rom_word(off & ~3u) : 0;
        hw_note("reads the cartridge at", addr & ~0xFFFFFu);
        return 1;
    }
#ifdef N64RT_RSP
    if (pa >= 0x04000000u && pa < 0x04040000u) {                /* the RSP's DMEM and IMEM (repeated every 8 KiB to 0x0404_0000) */
        extern int n64rt_sp_mem(u32 off, u32 *v, int write);
        if (!n64rt_sp_mem(pa & 0x1FFCu, v, 0)) *v = 0;
        hw_note("reads the RSP's memory at", addr & ~0xFFFu);
        return 1;
    }
#endif
    if (pa >= 0x05000000u && pa < 0x08000000u) {                /* the 64DD: none (a game looks for its ROM) */
        *v = 0;
        hw_note("looks for the 64DD at", addr & ~0xFFFFFFu);
        return 1;
    }
    if (si_read(pa, v)) { si_note(); return 1; }               /* the SI, PIF RAM */
    switch (pa) {
    case 0x04300008u: *v = s_si_irq ? 0x02u : 0u; break;          /* MI_INTR: the SI's, where the game polls it */
    case 0x04600010u: *v = 0; break;                              /* PI_STATUS: idle */
    case 0x04500004u: *v = n64rt_osAiGetLength(); break;          /* AI_LEN */
    case 0x0450000Cu: *v = n64rt_osAiGetStatus(); break;          /* AI_STATUS */
#ifdef N64RT_RSP
    case 0x04040010u: *v = *(volatile u32 *)(uintptr_t)0x20000010u; break;   /* SP_STATUS */
    case 0x04040018u: *v = *(volatile u32 *)(uintptr_t)0x20000018u; break;   /* SP_DMA_BUSY */
    case 0x04100004u: *v = *(volatile u32 *)(uintptr_t)0x20000024u; break;   /* DPC_END */
    case 0x04100008u: *v = *(volatile u32 *)(uintptr_t)0x20000028u; break;   /* DPC_CURRENT */
    case 0x0410000Cu: *v = *(volatile u32 *)(uintptr_t)0x2000002Cu; break;   /* DPC_STATUS */
#endif
    case 0x04400010u: *v = (s_fields & 1u) | 0x100u; break;       /* VI_CURRENT: a line of this field */
    default:
        /* the rest of the RCP's registers: nothing there, and a game that
         * looks is told so (once a register) rather than stopped */
        if (pa < 0x04000000u || pa >= 0x04900000u) return 0;
        *v = 0;
        break;
    }
    hw_note("reads", addr);
    return 1;
}
int n64rt_hw_write(u32 addr, u32 v)
{
    (void)v;
    u32 pa = addr & 0x1FFFFFFFu;
#ifdef N64RT_OWN
    if (rcp_write(pa, v)) return 1;
#endif
    if (si_write(pa, v)) { si_note(); return 1; }
#ifdef N64RT_RSP
    if (pa >= 0x04000000u && pa < 0x04040000u) {
        extern int n64rt_sp_mem(u32 off, u32 *v, int write);
        (void)n64rt_sp_mem(pa & 0x1FFCu, &v, 1);
        hw_note("writes the RSP's memory at", addr & ~0xFFFu);
        return 1;
    }
#endif
#ifdef N64RT_RSP
    if (pa == 0x04040010u) {                     /* SP_STATUS */
        /* halt cleared with a task loaded and not started: the game starts the
         * RSP itself (and then reads SP_STATUS for its end: a CIC-6105 game's
         * check). Interrupts are off here (the exception's handler). */
        if ((v & 1u) && s_task_loaded) {
            OSTask *t = s_task_loaded;
            s_task_loaded = 0;
            s_async = 0;
            task_body(t);
        }
        /* (anything else the game writes there is left out: the RSP's run and
         * halt are the runtime's, by the tasks it is given) */
#ifdef N64RT_DEBUG
        { static u32 s_n; if (s_n++ < 12u) n64rt_log("n64rt: the game writes SP_STATUS %08lx itself\n", (unsigned long)v); }
#endif
        hw_note("writes SP_STATUS", addr);
        return 1;
    }
#endif
    if (pa < 0x04000000u || pa >= 0x04900000u) return 0;
    hw_note("writes (ignored)", addr);           /* the RCP's registers: the runtime's, not the game's */
    return 1;
}

void n64rt_io_init(void)
{
    n64rt_eeprom_init();
    eep_db_init();
    n64rt_pak_init();
    extern void n64rt_audio_init(void), n64rt_gfx_init(void);
    n64rt_audio_init();
    n64rt_gfx_init();
    video_framebuffer_vtg_mode_write(1);                 /* 320 x 240 */
    video_framebuffer_width_write(TV_WIDTH_BIT);
    video_framebuffer_dma_base_write(N64RT_RDRAM_SDRAM);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
}

#ifdef N64RT_OWN
/* ===== the RCP, for a game that runs its own libultra (N64RT_OWN) =============
 * Nothing of libultra is replaced: the game's own handler (0x8000_0180), its
 * threads, its timers run on the CPU as they are. What it reaches for is the
 * RCP's registers, each access a trap into the monitor level (mips_core's MON,
 * n64rt_os.c n64rt_mon_exception), and what they say is kept here.
 *   MI   the interrupt bits and their mask. SP's bit is the RSP's own flag
 *        (the hardware's, read live), SI's the PIF emulation's; the others the
 *        runtime sets -- VI at each field, AI when a buffer has been played, PI
 *        when a DMA is done, DP when the RDP's SYNC FULL is in memory -- and
 *        clears at the game's writes. What is left of (INTR & MASK) is the line the
 *        game's Cause.IP2 shows (MONITOR.CTL), and when it is up its handler runs.
 *   SP, DPC   the hardware's own registers, at 0x2000_0000 -- the RSP is real
 *        here, a DMA, its status bits, its PC as the N64's.
 *   VI   the raw registers kept; origin, width, the window, black, gamma go
 *        to the scan-out at each field as osViSwapBuffer / osViSetMode's did.
 *   AI   a buffer handed over by AI_LEN is the audio feed's (n64rt_rsp.c), the
 *        rate AI_DACRATE's.
 *   PI   a DMA is done at once, from the ROM (or the FlashRAM), and ends in the
 *        PI interrupt.
 *   SI   the PIF emulation above.
 * What wakes the monitor: MONITOR.PEND, the hardware's -- the RSP's interrupt,
 * a field (four lines before the scan-out's frame boundary), a tick of 4 ms
 * (the audio feed), the RDP's SYNC FULL. */
#define MI_SP 0x01u
#define MI_SI 0x02u
#define MI_AI 0x04u
#define MI_VI 0x08u
#define MI_PI 0x10u
#define MI_DP 0x20u
static u32 s_mi_mask, s_mi_sw, s_mi_mode;
static u32 s_vi[16];
static u32 s_ai_dram, s_ai_ctl, s_ai_dac;
u32 n64rt_pi_regs[16];                   /* the PI's and the RI's registers as the game wrote them (n64rt_exc.S's fast path reads
                                         * and writes them too) */
u32 n64rt_ri_regs[8];
static u32 s_mon_line;
static u64 s_field_t0;
/* The MI and PI_STATUS in hardware (MIRLO_MI, MIRLO_PI; rtl/soc/rcp_mi.sv): the game's word accesses to them are no
 * trap, VI and DP are set by the hardware's own pulses, and the line is the hardware's. What the runtime raises or
 * clears itself it says through MIRAISE / MICLR; a copy of the cartridge's it does not do itself, the engine does.
 * Without them (a core before this, or this one's MI unset) every access is emulated as it was. */
static u32 s_mi_hw, s_pi_hw, s_pi_hw_n, s_pi_sw_n, s_pi_half_n, s_pi_ps_n, s_rom64_hw;
/* The AI in hardware (MIRLO_AI, MONITOR.ID bit 4; rtl/soc/rcp_ai.sv; n64rt_ai_hw_event below): the game's accesses to the AI are
 * real bus accesses -- the queue, the reader and the resampler to the Pocket's FIFO are the hardware's, and AI_STATUS clears the
 * MI's AI bit itself -- and the runtime is woken (MONITOR.PEND bit 3) only when AI_DACRATE is written, to give the resampler
 * its step. 0 on a core without it: the software AI (n64rt_rsp.c) feeds the FIFO from the tick. */
static u32 s_ai_hw;
static void mi_set(u32 b) { if (s_mi_hw) monitor_miraise_write(b); else s_mi_sw |= b; }
static void mi_clr(u32 b) { if (s_mi_hw) monitor_miclr_write(b); else s_mi_sw &= ~b; }
static void mi_si_sync(int on) { if (s_mi_hw) { if (on) monitor_miraise_write(MI_SI); else monitor_miclr_write(MI_SI); } }
/* (the monitor's own access to a register the hardware has: a plain KSEG1 access, which is no trap there) */
static inline u32 hw_rd(u32 pa) { return *(volatile u32 *)(uintptr_t)(0xA0000000u | pa); }
static inline void hw_wr(u32 pa, u32 v) { *(volatile u32 *)(uintptr_t)(0xA0000000u | pa) = v; }
/* start-up (n64rt_os_start): is the MI there, and is the core's pass-through on (MCTL bit 8, written with bit 9) */
void n64rt_mi_init(void)
{
    u32 id = monitor_id_read(), m;
    if (!(id & 2u)) return;
    __asm__ volatile("mfc0 %0, $7" : "=r"(m));
    /* (bit 10, written with bit 11: the game's RDRAM is 4 MiB -- the console without its Expansion Pak, the core's setting:
     * the game's accesses to the upper half come here, and are answered as RDRAM that is not there, n64rt_os.c emulate_hw) */
    m = (m & 3u) | 0x300u | 0x800u | (n64rt_game->mem_size <= 0x400000u ? 0x400u : 0u);
    __asm__ volatile("mtc0 %0, $7" :: "r"(m));
    __asm__ volatile("mfc0 %0, $7" : "=r"(m));
    if (!(m & 0x100u)) return;
    s_mi_hw = 1;
    { extern void n64rt_mon_fast_enable(void); n64rt_mon_fast_enable(); }
    s_pi_hw = (id & 4u) != 0u;
    s_vi_hw = (id & 8u) != 0u;
    s_ai_hw = (id & 16u) != 0u;
    s_si_hw = (id & 32u) != 0u;
    s_pi2_hw = s_pi_hw && (id & 64u) != 0u;
    s_rom64_hw = s_pi2_hw && (id & 128u) != 0u;
    if (s_pi2_hw && n64rt_game && n64rt_game->rom_sdram != 0x40100000u)
        n64rt_log("n64rt: the ROM is not at 1 MiB: the game's own reads of it (the PI's in hardware) would be wrong\n");
}
u32 n64rt_mi_hw(void) { return s_mi_hw; }
u32 n64rt_vi_hw(void) { return s_vi_hw; }
u32 n64rt_ai_hw(void) { return s_ai_hw; }
u32 n64rt_si_hw(void) { return s_si_hw; }
/* The PIF has commands to run (MONITOR.PEND bit 4: an RD64B of a block that holds them, or the end of a WR64B that carried the run
 * flag or the challenge's bit, or a word write of the status byte with that bit): the block is in the PIF RAM -- read it, run the
 * joybus commands on the copy (pif_run, as the software SI's) or answer the challenge, write back the words that changed, and say
 * the PIF is done: IO busy clears, and an RD64B that waited goes on once the console's time for those commands is over (SIDONE's
 * value: a game whose controller thread polls in a loop must leave its other threads that time; a challenge, 0). */
void n64rt_si_hw_event(void)
{
    u32 orig[16];
    for (int i = 0; i < 16; i++) orig[i] = s_pif_w[i] = hw_rd(PIF_RAM + 4u * (u32)i);
    u32 t = 0;
    if (s_pif[63] & 2u) pif_challenge();             /* (the status byte keeps that bit: a challenge, not commands) */
    else t = pif_run();
    for (int i = 0; i < 16; i++) if (s_pif_w[i] != orig[i]) hw_wr(PIF_RAM + 4u * (u32)i, s_pif_w[i]);
    monitor_sidone_write(t < 0x3FFFFu ? t : 0x3FFFFu);
}
static void vi_hw_note_origin(void) { if (s_vi_hw) fb_note(hw_rd(0x04400004u)); }
/* The VI's registers the window and the size come from changed (MONITOR.PEND bit 1, which is no longer a field): read
 * them back, as the software VI took them from the game's writes, and give the scan-out the width, the window and the
 * size (they go with its next frame boundary) and the last origin a picture of that size fits at in the game's RDRAM (the
 * hardware blanks the one that does not: Pokemon Stadium between two screens). The base, the gamma and the black of
 * H_START 0 are the hardware's own. Also at start, from the registers' reset values. */
void n64rt_vi_hw_event(void)
{
    const u32 w = hw_rd(0x04400008u) & 0xFFFu, vs = hw_rd(0x04400028u), ys = hw_rd(0x04400034u) & 0xFFFu, xs = hw_rd(0x04400030u) & 0xFFFu,
              hs = hw_rd(0x04400024u);
    vi_note_vsync(hw_rd(0x04400018u));
    s_vi_l = ((vs & 0x3FFu) - ((vs >> 16) & 0x3FFu)) / 2u; s_vi_vs = vs;
    if (ys) s_vi_ys = ys;                              /* (0: a fade's or a repeated line's, not a mode's) */
    if (xs) s_vi_xs = xs;
    if (hs) s_vi_hs = hs;                              /* (0: osViBlack) */
    s_vi_width = s_vi_width_cur = w;
    vi_width_gamma();
    {
        const u32 bytes = vi_pic_bytes(), lim = n64rt_game->mem_size + 0x1000u;
        monitor_vithr_write(lim > bytes ? (lim - bytes) >> 6 : 0u);
#ifdef N64RT_DEBUG
        static u32 s_n;
        if (++s_n <= 32u)
            n64rt_log("n64rt: field %lu: VI WIDTH %lu V_START %08lx Y_SCALE %03lx V_SYNC %03lx H_SYNC %08lx V_INTR %03lx: the picture %lu wide, %lu rows (tall %lu), threshold %05lx, %s\n",
                      (unsigned long)s_fields, (unsigned long)w, (unsigned long)vs, (unsigned long)ys, (unsigned long)hw_rd(0x04400018u),
                      (unsigned long)hw_rd(0x0440001Cu), (unsigned long)hw_rd(0x0440000Cu), (unsigned long)(w >= 512u ? 640u : w),
                      (unsigned long)s_vi_l, (unsigned long)s_vi_tall_cur, (unsigned long)(lim > bytes ? (lim - bytes) >> 6 : 0u),
                      n64rt_vi_pal() ? "PAL" : "NTSC");
#endif
    }
}
#ifdef N64RT_DEBUG
static void rcp_hist_log(void);
#endif
u32 n64rt_audio_poll(void);
s32 n64rt_osAiSetNextBuffer(void *buf, u32 size);
s32 n64rt_osAiSetFrequency(u32 f);
u32 n64rt_ai_buffers_done(void);                 /* (n64rt_rsp.c) AI buffers played since the last call */

static u32 mi_intr(void)
{
    if (s_mi_hw) return monitor_miintr_read() & 0x3Fu;
    u32 v = s_mi_sw;
    if (monitor_level_read() & 1u) v |= MI_SP;
    if (s_si_irq) v |= MI_SI;
    return v;
}
/* Cause.IP2: (INTR & MASK) as it is now */
void n64rt_rcp_line(void)
{
    if (s_mi_hw) return;
    u32 want = (mi_intr() & s_mi_mask) != 0u;
    if (want != s_mon_line) { s_mon_line = want; monitor_ctl_write(1u | want << 1); }
}
static inline u32 rsp_rd(u32 off) { return *(volatile u32 *)(uintptr_t)(0x20000000u + off); }
/* DPC_CLOCK (0x0410_0010): the RDP's 24-bit clock counter, one tick per RCP clock (62.5 MHz: the SoC's), cleared by a write of
 * DPC_STATUS with bit 9 (CLR_CLOCK_CTR). It ticks on every console but the CIC-6105 ones (some games, like NBA Jam, just
 * need the clock counter ticking up and others, like Ocarina of Time, depend on it being either zero or very accurate): Ocarina of Time and
 * the other 6105 games read 0, as before. The hardware has no such counter (the RSP's own MFC0 of it reads 0); this is the CPU's
 * view of it, from the SoC's uptime counter. */
static u32 s_dpc_clk0;
static u32 s_pi_wr_len;                 /* PI_WR_LEN as a read gives it (pi_advance, below) */
static u32 dpc_clock(void)
{
    if (n64rt_game && n64rt_game->cic == 6105u) return 0u;
    timer0_uptime_latch_write(1);
    return ((u32)timer0_uptime_cycles_read() - s_dpc_clk0) & 0x00FFFFFFu;
}
#ifdef N64RT_RSPDUMP
/* (sim/mips_soc RSPDUMP=<dir>: the RDRAM, and the OSTask the game DMAs into DMEM 0xFC0, at the
 * write that starts the RSP -- sim/rsp's input) */
static u32 s_dm_mem, s_dm_dram, s_dm_task;
static inline void rsp_wr(u32 off, u32 v)
{
    if (off == 0x00u) s_dm_mem = v;
    else if (off == 0x04u) s_dm_dram = v;
    else if (off == 0x08u && (s_dm_mem & 0x1FFFu) == 0x0FC0u) s_dm_task = s_dm_dram & 0x7FFFFFu;
    else if (off == 0x10u && (v & 1u) && s_dm_task) { ctrl_scratch_write(0xD0000000u | s_dm_task); s_dm_task = 0; }
    *(volatile u32 *)(uintptr_t)(0x20000000u + off) = v;
}
#else
static inline void rsp_wr(u32 off, u32 v) { *(volatile u32 *)(uintptr_t)(0x20000000u + off) = v; }
#endif

#ifdef N64RT_DEBUG
static void mon_field_log(void)
{
    if (s_fields % 60u != 0u) return;
    extern u32 n64rt_mon_codes[32], n64rt_mon_epc, n64rt_mon_fast_count(void);
    n64rt_log("n64rt: MI %s%s, PI DMAs: engine %lu (%lu from a half word, %lu from the PSRAM), copied %lu; monitor entries on the fast path %lu\n", s_mi_hw ? (s_pi_hw ? "and PI in hardware" : "in hardware") : "emulated",
              s_vi_hw ? ", VI in hardware" : "", (unsigned long)s_pi_hw_n, (unsigned long)s_pi_half_n, (unsigned long)s_pi_ps_n, (unsigned long)s_pi_sw_n, (unsigned long)n64rt_mon_fast_count());
    n64rt_log("n64rt: monitor, field %lu: swaps %lu | int %lu sys %lu fpu-ish %lu adel %lu ades %lu tlb %lu ri %lu | MI intr %02lx mask %02lx line %lu sp %lu | game at %08lx\n",
              (unsigned long)s_fields, (unsigned long)n64rt_swaps, (unsigned long)n64rt_mon_codes[31], (unsigned long)n64rt_mon_codes[8], (unsigned long)n64rt_mon_codes[11],
              (unsigned long)n64rt_mon_codes[4], (unsigned long)n64rt_mon_codes[5], (unsigned long)(n64rt_mon_codes[2] + n64rt_mon_codes[3]),
              (unsigned long)n64rt_mon_codes[10], (unsigned long)mi_intr(), (unsigned long)s_mi_mask, (unsigned long)s_mon_line,
              (unsigned long)(monitor_level_read() & 1u), (unsigned long)n64rt_mon_epc);
    rcp_hist_log();
    { extern void n64rt_ri_hist_log(void); n64rt_ri_hist_log(); }
}
#endif

/* the monitor's wake-up: what the hardware has seen */
void n64rt_vi_hw_event(void);
void n64rt_ai_hw_event(void);
void n64rt_si_hw_event(void);
void n64rt_rcp_events(void)
{
    u32 pend = monitor_pend_read();
    monitor_pend_write(pend);
    if (pend & 16u) n64rt_si_hw_event();             /* (first: an RD64B waits for it) */
    if (pend & 2u) {                                  /* a field (the VI in hardware: a change of its registers) */
        if (s_vi_hw) {
            n64rt_vi_hw_event();
        } else {
            timer0_uptime_latch_write(1);
            s_field_t0 = timer0_uptime_cycles_read();
            n64rt_vi_field();
            if (!s_mi_hw) s_mi_sw |= MI_VI;
#ifdef N64RT_DEBUG
            mon_field_log();
#endif
        }
    }
    if (pend & 4u) {                                  /* a tick */
        if (s_vi_hw) {
            /* (no field wakes the monitor: its count follows the ticks, 262,144 SoC cycles each, a field 1,042,710 -- the console's
             * 59.940 Hz: 4096 * 262,144 / 1,042,710 = 1029.8; it was 1,045,440 and 1027 -- and the frame buffers the game has
             * handed the VI are the origins seen at the ticks, four a field) */
            static u32 s_tick_acc;
            s_tick_acc += n64rt_vi_pal() ? 859u : 1030u;         /* (PAL: 4096 * 262,144 / 1,250,000 = 859.0) */
            while (s_tick_acc >= 4096u) {
                s_tick_acc -= 4096u;
                vi_field_common();
#ifdef N64RT_DEBUG
                mon_field_log();
#endif
            }
            vi_hw_note_origin();
        } else {
            n64rt_vi_after_boundary();
        }
        if (!s_ai_hw) {
            (void)n64rt_audio_poll();
            if (n64rt_ai_buffers_done()) mi_set(MI_AI);
        }
    }
    if (pend & 8u) {
        if (s_ai_hw) n64rt_ai_hw_event();             /* (the AI in hardware: bit 3 is a write of AI_DACRATE, not a SYNC FULL) */
        else if (!s_mi_hw) s_mi_sw |= MI_DP;          /* SYNC FULL */
    }
}
static u32 vi_current(void)
{
    const u32 pal = n64rt_vi_pal();
    const u32 period = pal ? N64RT_FIELD_PAL : N64RT_FIELD_NTSC;      /* the scan-out's field, in SoC cycles (59.940 Hz; PAL 50 Hz) */
    const u32 nlines = pal ? 312u : 262u;
    timer0_uptime_latch_write(1);
    u32 d = (u32)(timer0_uptime_cycles_read() - s_field_t0) % period;
    u32 line = (d * nlines / period + 236u) % nlines;                  /* (the field starts 4 lines before the boundary) */
    return line << 1 | (s_fields & 1u);
}
#ifdef N64RT_DEBUG
/* which registers are read and written, and how many times (the log, every 60 fields) */
static struct { u32 pa, n, last; u8 w; } s_rcp_hist[48];
static void rcp_count(u32 pa, int w, u32 v)
{
    for (int i = 0; i < 48; i++)
        if (s_rcp_hist[i].pa == pa && s_rcp_hist[i].w == w) { s_rcp_hist[i].n++; s_rcp_hist[i].last = v; return; }
    for (int i = 0; i < 48; i++)
        if (!s_rcp_hist[i].pa) { s_rcp_hist[i].pa = pa; s_rcp_hist[i].w = (u8)w; s_rcp_hist[i].n = 1; s_rcp_hist[i].last = v; return; }
}
static void rcp_hist_log(void)
{
    for (int i = 0; i < 48; i++)
        if (s_rcp_hist[i].pa) {
            n64rt_log("n64rt:   %s %08lx x%lu, last %08lx\n", s_rcp_hist[i].w ? "write" : "read ", (unsigned long)s_rcp_hist[i].pa,
                      (unsigned long)s_rcp_hist[i].n, (unsigned long)s_rcp_hist[i].last);
            s_rcp_hist[i].n = 0;
        }
}
#endif
static int rcp_read_(u32 pa, u32 *v);
static int rcp_read(u32 pa, u32 *v)
{
    int r = rcp_read_(pa, v);
#ifdef N64RT_DEBUG
    if (r) rcp_count(pa, 0, *v);
#endif
    return r;
}
static int rcp_read_(u32 pa, u32 *v)
{
    if (pa >= 0x08000000u && pa < 0x10000000u) { (void)io_read(pa, v); return 1; }       /* the cartridge's domain 2: FlashRAM, as the PI's IO */
    /* The 64DD's registers (0x0500_0000) and its ROM (0x0600_0000): no drive. Nothing answers on the cartridge's bus there, and a
     * read gets what the bus last carried, the address's low half, twice (0x0500_0508, the drive's status, reads 0x0508_0508).
     * A 0 read here is a drive that is there to a game that looks for one: it starts the drive's manager and waits for it for
     * ever (LeoCJCreateLeoManager's `while (status == 8)`), after reading its font from the drive's ROM -- a black screen. */
    if (pa >= 0x05000000u && pa < 0x08000000u) { *v = (pa & 0xFFFFu) << 16 | (pa & 0xFFFFu); return 1; }
    /* what is not there (the bus's other addresses, the PIF's ROM): a read is told 0 */
    if ((pa >= 0x04900000u && pa < 0x10000000u) || (pa >= 0x1FC00000u && !(pa >= 0x1FC007C0u && pa < 0x1FC00800u))) { *v = 0; return 1; }
    if (pa < 0x04000000u || pa >= 0x04900000u) return 0;
    u32 blk = pa >> 20, o = pa & 0xFFFFFu;
    switch (blk) {
    case 0x040:
        if (pa < 0x04040000u) return 0;                             /* DMEM, IMEM (and their repeats): n64rt_sp_mem */
        if (pa == 0x04080000u) { *v = rsp_rd(0x20u); return 1; }     /* SP_PC */
        if (pa >= 0x04040000u && pa < 0x04040020u) { *v = rsp_rd(pa - 0x04040000u); return 1; }   /* SP_MEM_ADDR .. SP_SEMAPHORE */
        *v = 0; return 1;
    case 0x041:                                                       /* DPC */
        if (o < 0x10u) *v = rsp_rd(0x40u + o);
        else if (o == 0x10u) *v = dpc_clock();                         /* DPC_CLOCK */
        else *v = 0u;                                                  /* (BUFBUSY, PIPEBUSY, TMEM: 0) */
        return 1;
    case 0x042: *v = 0; return 1;                                     /* DPS */
    case 0x043:
        if (s_mi_hw) { *v = hw_rd(0x04300000u | (pa & 0xCu)); return 1; }
        switch (o) {
        case 0x00u: *v = s_mi_mode & 0x7Fu; return 1;
        case 0x04u: *v = 0x02020102u; return 1;
        case 0x08u: *v = mi_intr(); return 1;
        case 0x0Cu: *v = s_mi_mask; return 1;
        }
        *v = 0; return 1;
    case 0x044:
        if (o == 0x10u) *v = vi_current();
        else if (o < 0x38u) *v = s_vi[o >> 2];
        else *v = 0;
        return 1;
    case 0x045:
        if (s_ai_hw) { *v = hw_rd(pa); return 1; }                    /* (a byte or halfword access: the hardware's word) */
        switch (o) {
        case 0x00u: *v = s_ai_dram; return 1;
        case 0x04u: *v = n64rt_osAiGetLength(); return 1;
        case 0x08u: *v = s_ai_ctl; return 1;
        case 0x0Cu: *v = n64rt_osAiGetStatus() | ((n64rt_osAiGetStatus() & 0x80000000u) ? 1u : 0u); return 1;
        case 0x10u: *v = s_ai_dac; return 1;
        }
        *v = 0; return 1;
    case 0x046:
        if (o == 0x10u && s_pi_hw) *v = hw_rd(pa);                    /* PI_STATUS: the engine's */
        else if (o == 0x10u) *v = (s_mi_sw & MI_PI) ? 0u : 0u;        /* PI_STATUS: never busy */
        else if (o == 0x00u) *v = n64rt_pi_regs[0] & 0x00FFFFFEu;     /* PI_DRAM_ADDR: 24 bits, even */
        else if (o == 0x04u) *v = n64rt_pi_regs[1] & 0xFFFFFFFEu;     /* PI_CART_ADDR: even */
        else if (o == 0x08u) *v = 0x7Fu;                              /* PI_RD_LEN reads 0x7F */
        else if (o == 0x0Cu) *v = s_pi_wr_len;                        /* PI_WR_LEN: the last block's residual */
        else if (o < 0x34u) *v = n64rt_pi_regs[o >> 2];
        else *v = 0;
        return 1;
    case 0x047:
        *v = o < 0x20u ? n64rt_ri_regs[o >> 2] : 0u;
        return 1;
    default: return 0;                                                /* SI: below */
    }
}
static void vi_write(u32 o, u32 v)
{
    if (o >= 0x38u) return;
    s_vi[o >> 2] = v;
    switch (o) {
    case 0x00: s_vi_gamma = (v >> 3) & 1u; break;                    /* VI_STATUS: VI_CTRL_GAMMA_ON */
    case 0x04:
#ifdef N64RT_DEBUG
        {   /* (when in the field, in SoC cycles from the field event, the game's VI_ORIGIN write lands: the scan-out latches 19,799 cycles after it) */
            static u32 s_n; u32 d;
            timer0_uptime_latch_write(1);
            d = (u32)(timer0_uptime_cycles_read() - s_field_t0);
            if ((v & 0x00FFFFFFu) != s_fb_next && s_n++ < 40u)
                n64rt_log("n64rt: VI_ORIGIN %08lx written %lu cycles after the field event (field %lu)\n", (unsigned long)v, (unsigned long)d, (unsigned long)s_fields);
        }
#endif
        s_fb_prev = s_fb_next; s_fb_next = v & 0x00FFFFFFu; fb_note(s_fb_next);     /* VI_ORIGIN */
#ifdef N64RT_OWN
        vi_apply_base();
#endif
        break;
    case 0x08: s_vi_width = v & 0xFFFu; break;                        /* VI_WIDTH */
    case 0x10: mi_clr(MI_VI); break;                                  /* VI_CURRENT: clears the interrupt */
    case 0x18: { const u32 o = s_vi_vsync; vi_note_vsync(v); if (s_vi_vsync != o) s_vi_width_cur = ~0u; break; }   /* VI_V_SYNC: the TV system's (the window's origins) */
    case 0x24:                                                        /* VI_H_START 0: osViBlack's */
        s_vi_black = v == 0u;
        if (v != 0u && v != s_vi_hs) { s_vi_hs = v; s_vi_width_cur = ~0u; }
        break;
    case 0x28: {                                                      /* VI_V_START: the lines it shows */
        u32 l = ((v & 0x3FFu) - ((v >> 16) & 0x3FFu)) / 2u;
        if (v != s_vi_vs) { s_vi_vs = v; s_vi_l = l; s_vi_width_cur = ~0u; }
        break;
    }
    case 0x30:                                                        /* VI_X_SCALE: the step */
        if ((v & 0xFFFu) != 0u && (v & 0xFFFu) != s_vi_xs) { s_vi_xs = v & 0xFFFu; s_vi_width_cur = ~0u; }
        break;
    case 0x34:                                                        /* VI_Y_SCALE: the step (an interlaced hi-res mode's is 2.0) */
        if ((v & 0xFFFu) != 0u && (v & 0xFFFu) != s_vi_ys) { s_vi_ys = v & 0xFFFu; s_vi_width_cur = ~0u; }   /* (0: a fade's or a repeated line's, not a mode's) */
        break;
    }
}
/* The bytes a PI_WR_LEN of `len` (the register's value: len + 1 bytes) writes into the RDRAM at `pa` (8 bytes aligned, as
 * libultra has it), as the console's PI does: it moves the cartridge's 16-bit words in blocks of up to 128 bytes that end
 * at a 2 KiB row of the RDRAM; only a first block of less than 127 bytes is written byte by byte, every other one as
 * whole 16-bit words -- so an odd length is exact if the whole copy is one block of at most 126 bytes, else one byte
 * more (ares' pi/dma.cpp, after the n64-systemtest measurements). A PI_RD_LEN moves (len | 1) + 1 bytes. */
static u32 pi_wr_bytes(u32 pa, u32 len)
{
    const u32 n = (len & 0x00FFFFFFu) + 1u;
    return n <= 126u && n <= 0x800u - (pa & 0x7FFu) ? n : (n + 1u) & ~1u;
}
/* A cartridge DMA by the engine, if it can do it (1): words of the SDRAM to words of the SDRAM. Into the RDRAM from the ROM
 * (MONITOR.ID bit 7: its first 32 MiB from the PSRAM, PISRC bit 25) and, with PI2 (MONITOR.ID bit 6), from a cartridge address 2 mod 4,
 * of any length, and from domain 2 (the save's memory; a FlashRAM's array, or its status words); from the RDRAM into
 * domain 2 (an SRAM, a FlashRAM's page buffer). Without PI2 only whole words, word-aligned at both ends, from the ROM. It
 * ends in the PI interrupt, the engine's. The game's D-cache lines over an RDRAM destination go first: the engine writes
 * the SDRAM, not the cache (a game does its osInvalDCache itself too). */
static int pi_hw_dma(int rd, u32 dev, u32 pa, u32 size)
{
    const u32 cpa = dev & 0x1FFFFFFFu, rdram = N64RT_RDRAM_SDRAM + pa;
    u32 src = 0, dst = 0, ps = 0;
    if (size == 0u || (pa & 3u) || pa + size > 0x800000u || (size >> 2) >= (1u << 21) - 1u) goto soft;
    if (cpa >= 0x08000000u && cpa < 0x10000000u) {                   /* domain 2: the save */
        if (!s_pi2_hw) goto soft;
        if (s_d2 == D2_NONE) d2_use(D2_SRAM);
        const u32 a = d2_engine_addr(rd, (cpa - 0x08000000u) & (D2_WINDOW - 1u), size);
        if (!a) goto soft;
        if (rd) dst = a; else src = a;
    } else if (!rd && cpa >= 0x10000000u && cpa < 0x1FC00000u) {    /* the ROM */
        u32 off = cpa - 0x10000000u;
        if (size > n64rt_game->rom_size || off > n64rt_game->rom_size - size) goto soft;
        /* (the core's own placement, the ROM at 1 MiB: its first 32 MiB in the PSRAM -- the word the SDRAM's would be, its 23
         * low bits --, the rest in the SDRAM from 1 MiB; a copy across 32 MiB stays the runtime's) */
        if (!s_rom64_hw || n64rt_game->rom_sdram != 0x40100000u) goto soft;
        if (off + size <= N64RT_ROM_PSRAM_MAX) { ps = 1u; src = 0x40100000u + off; }
        else if (off >= N64RT_ROM_PSRAM_MAX) src = 0x40100000u + (off - N64RT_ROM_PSRAM_MAX);
        else goto soft;
    } else goto soft;
    if (rd) src = rdram; else dst = rdram;
    if ((src & 1u) || (!s_pi2_hw && ((src | size) & 3u))) goto soft;
    while (monitor_pilen_read() >> 31) { }                            /* (one at a time: the game's PI queue does that too) */
    if (!rd) {
        if (n64rt_gfx_quiesce && fb_overlaps(pa, size)) n64rt_gfx_quiesce();
        flush_cpu_dcache_range(K0P(pa), size);
    }
    monitor_pidst_write((dst - 0x40000000u) >> 2);                    /* (an RDRAM word: 0xE0_0000 + its word, which a core without
                                                                       * PI2 takes as its 21 bits) */
    monitor_pisrc_write((((src - 0x40000000u) >> 2) & 0x00FFFFFFu) | ((src >> 1) & 1u) << 24 | ps << 25);
    monitor_pilen_write(s_pi2_hw ? ((size + 3u) >> 2) | (size & 3u) << 22 : size >> 2);
    s_pi_hw_n++;
    if (src & 2u) s_pi_half_n++;
    s_pi_ps_n += ps;
    return 1;
soft:
    s_pi_sw_n++;
    return 0;
}
/* The PI's bookkeeping, as the console's: PI_DRAM_ADDR holds 24 bits and PI_CART_ADDR 32, both even (a read gives them masked: above);
 * a DMA leaves them past what it moved -- the RDRAM side rounded up to 8 bytes, the cartridge side to an even count -- and a read
 * of PI_WR_LEN gives the residual of the last block written to RDRAM, 0x7F for a whole one (0 until the first), PI_RD_LEN 0x7F
 * always. libultra writes both addresses before every DMA and never reads them; a program that chains DMAs on the registers'
 * advance, or reads them back (libdragon's, test ROMs), finds what the hardware gives. (PI_STATUS's IO busy and DMA error bits
 * are the PI hardware's own register, which a word read does not trap: not here.) */
static void pi_advance(u32 dev, u32 pa, u32 size, int rd)
{
    n64rt_pi_regs[0] = (pa + size + 7u) & ~7u & 0x00FFFFFEu;
    n64rt_pi_regs[1] = (dev & 0xFFFFFFFEu) + ((size + 1u) & ~1u);
    if (!rd) s_pi_wr_len = 0x7Fu;
}
/* a write of PI_WR_LEN (rd 0) or PI_RD_LEN (1): the DMA, by the engine or here */
static void pi_start(int rd, u32 len)
{
    const u32 dev = n64rt_pi_regs[1] & 0xFFFFFFFEu, pa = n64rt_pi_regs[0] & 0x00FFFFFEu;
    const u32 size = rd ? ((len & 0x00FFFFFFu) | 1u) + 1u : pi_wr_bytes(pa, len);
    pi_advance(dev, pa, size, rd);
#ifdef N64RT_OWN
    if (!rd) n64rt_tlb_frames_loaded(pa, size);
#endif
    if (s_pi_hw && pi_hw_dma(rd, dev, pa, size)) return;
    if (!rd) (void)pi_copy(dev, K0P(pa), size, OS_READ);
    else if (is_flash(dev)) d2_copy(dev, K0P(pa), size, OS_WRITE);
    else n64rt_log("n64rt: PI_RD_LEN %08lx -> cartridge %08lx: not done\n", (unsigned long)len, (unsigned long)dev);
    mi_set(MI_PI);
}
static int rcp_write_(u32 pa, u32 v);
static int rcp_write(u32 pa, u32 v)
{
    int r = rcp_write_(pa, v);
#ifdef N64RT_DEBUG
    if (r) rcp_count(pa, 1, v);
#endif
    return r;
}
static int rcp_write_(u32 pa, u32 v)
{
    if (pa >= 0x08000000u && pa < 0x10000000u) { (void)io_write(pa, v); return 1; }
#ifdef N64RT_TLBPROF
    if (pa == 0x04900004u) { ctrl_scratch_write(v); return 1; }       /* (tlb_bench's marks: the simulation's CPUSTAT marks, 0xA5 / 0xA6) */
#endif
    if ((pa >= 0x04900000u && pa < 0x10000000u) || (pa >= 0x1FC00000u && !(pa >= 0x1FC007C0u && pa < 0x1FC00800u))) return 1;
    if (pa < 0x04000000u || pa >= 0x04900000u) return 0;
    u32 blk = pa >> 20, o = pa & 0xFFFFFu;
    switch (blk) {
    case 0x040:
        if (pa < 0x04040000u) return 0;
        if (pa == 0x04080000u) { rsp_wr(0x20u, v); return 1; }
        if (pa >= 0x04040000u && pa < 0x04040020u) {
#ifdef N64RT_RSPDUMP
            /* (sim/mips_soc RSPDUMP=<dir>: the task the game loads at DMEM 0xFC0 -- osSpTaskLoad's DMA --
             * and starts by clearing HALT: its RDRAM address is the dump's mark) */
            static u32 s_dm, s_dd, s_task;
            if (pa == 0x04040000u) s_dm = v;
            else if (pa == 0x04040004u) s_dd = v;
            else if (pa == 0x04040008u && (s_dm & 0xFFFu) == 0xFC0u && (v & 0xFFFu) >= 0x3Fu) s_task = s_dd & 0x7FFFFFu;
            else if (pa == 0x04040010u && (v & 1u) && s_task) ctrl_scratch_write(0xD0000000u | s_task);
#endif
            rsp_wr(pa - 0x04040000u, v);
            return 1;
        }
        return 1;
    case 0x041:                                                       /* DPC: START, END, STATUS (CURRENT and the CLOCK are read-only) */
        if (o == 0x00u || o == 0x04u || o == 0x0Cu) {
            if (o == 0x0Cu && (v & 0x200u)) { timer0_uptime_latch_write(1); s_dpc_clk0 = (u32)timer0_uptime_cycles_read(); }   /* CLR_CLOCK_CTR */
            /* The RDP's registers are the hardware's own, at 0x2000_0040 + the offset (a read comes from there). A CPU's write is
             * forwarded to them: START (ignored while a start is pending), END and STATUS are the RDP's, the status bits as the
             * console shows them (rcp_dpc_view.sv). A core without the CPU's DPC write path drops the write (the host port gates
             * 0x2000_0040 and up): START / END / STATUS written by a CPU then stay what they were. */
            rsp_wr(0x40u + o, v);
        }
        return 1;
    case 0x042: return 1;
    case 0x043:
        if (s_mi_hw) { hw_wr(pa, v); return 1; }
        if (o == 0x00u) {
            if (v & 0x800u) s_mi_sw &= ~MI_DP;                        /* MI_CLR_DP_INTR */
            s_mi_mode = v;
        } else if (o == 0x0Cu) {
            static const u32 bit[6] = { MI_SP, MI_SI, MI_AI, MI_VI, MI_PI, MI_DP };
            for (int i = 0; i < 6; i++) {
                if (v & (1u << (2 * i)))     s_mi_mask &= ~bit[i];    /* clear */
                if (v & (2u << (2 * i)))     s_mi_mask |= bit[i];     /* set */
            }
        }
        return 1;
    case 0x044: vi_write(o, v); return 1;
    case 0x045:
        if (s_ai_hw) { hw_wr(pa, v); return 1; }
        switch (o) {
        case 0x00u: s_ai_dram = v & 0x00FFFFF8u; break;
        case 0x04u:
            if ((v & 0x3FFF8u) && n64rt_osAiSetNextBuffer((void *)(uintptr_t)N64RT_K0(s_ai_dram), v & 0x3FFF8u) == 0) { }
            break;
        case 0x08u: s_ai_ctl = v & 1u; break;
        case 0x0Cu: mi_clr(MI_AI); break;                             /* AI_STATUS: any write clears the interrupt */
        case 0x10u: s_ai_dac = v & 0x3FFFu; (void)n64rt_osAiSetFrequency(n64rt_vi_clock() / ((v & 0x3FFFu) + 1u)); break;
        }
        return 1;
    case 0x046:
        if (o == 0x10u && s_pi_hw) { hw_wr(pa, v); return 1; }       /* PI_STATUS: the engine's */
        if (o < 0x34u) n64rt_pi_regs[o >> 2] = v;
        if (o == 0x0Cu) pi_start(0, v);                              /* PI_WR_LEN: the cartridge into RDRAM */
        else if (o == 0x08u) pi_start(1, v);                         /* PI_RD_LEN: RDRAM to the cartridge: the save */
        else if (o == 0x10u) {
            if (v & 2u) mi_clr(MI_PI);                                /* clear the interrupt */
        }
        return 1;
    case 0x047: if (o < 0x20u) n64rt_ri_regs[o >> 2] = v; return 1;
    default: return 0;
    }
}
#endif
