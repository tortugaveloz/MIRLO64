/* GameShark codes: the setting "Expansion Pak/Gameshark" with GameShark chosen. The codes the player Started in the picker (n64rt_gspick.c, before the game starts) are the engine's: the
 * picker leaves the list it has built from the choice in SDRAM (n64rt_gsact.h) and the engine takes it when the game's start sets it up (n64rt_cheat_init). Nothing is read from the card
 * for them -- a game runs the codes of the picker that was Started, and none when there was no picker or B was pressed --, and every code of the list is run in every video field
 * (n64rt_gs.h has the format and the code types), as a GameShark does from its hook on the vertical retrace. The memory the codes write is the game's RDRAM (LAYOUT_RDRAM; with
 * 4 MiB the upper half is not written), and what they change is made visible to the game's caches.
 *
 * A debugging build (make GSLOG=1) writes what the engine does to a status file, <the game's .gs name>log (/Saves/n64/common/<ROM name>.gslog), after the first field, after 10 s and after a minute:
 * the number of codes and of fields, the writes that changed a value, and the RDRAM's value at each of the first codes' addresses. It is not in the normal build: each of those writes is a target command
 * that the CPU waits for (the Pocket takes tens of milliseconds, up to the timeout), in the video field's hook, with the game stopped -- an audible gap in a game whose audio thread has little to spare.
 * GSTRACE=1 logs the engine's time in a field. */
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"
#include "n64rt_gs.h"
#include "n64rt_gsact.h"
#include "n64rt_gsfile.h"

#define GS_MAX GSACT_MAX
static n64rt_gs_code_t s_code[GS_MAX];
static unsigned s_n;

static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static void settle(void) { for (volatile int i = 0; i < 256; i++) { } }

/* a target command: 0 done, the result code (1 to 7) if the Pocket failed it, -1 if it did not answer */
static int command(void (*issue)(u32))
{
    (void)apf_bridge_status_read();                       /* (a read clears it) */
    issue(1u);
    const u64 end = now() + (u64)CONFIG_CLOCK_FREQUENCY / 2u;
    while (!apf_bridge_status_read()) if (now() > end) return -1;
    return (int)(apf_bridge_command_result_code_read() & 7u);
}

static unsigned rdram_ok(unsigned a, unsigned n) { return a + n <= n64rt_game->mem_size; }
static volatile u8 *rdram(unsigned a) { return (volatile u8 *)(uintptr_t)(LAYOUT_RDRAM + a); }
/* A changed word may be an instruction: the one line of the game's instruction cache that can hold it is dropped (a hit invalidate: the core drops the line `p` is in), not the whole 16 KiB cache -- a
 * code that the game writes back every field (a timer, an ammunition count) would empty the cache of a game that is short of time, a field after a field. */
static inline void icache_drop(const volatile u8 *p) { MIPS_CACHE_OP(0x10, (unsigned long)p); }
static unsigned g_rd8(unsigned a)  { if (!rdram_ok(a, 1)) return 0x100u; flush_cpu_dcache_range((const void *)rdram(a), 1); return rdram(a)[0]; }
static unsigned g_rd16(unsigned a) { if (!rdram_ok(a, 2)) return 0x10000u; flush_cpu_dcache_range((const void *)rdram(a), 2); return (unsigned)(rdram(a)[0] << 8 | rdram(a)[1]); }
static int g_wr8(unsigned a, unsigned v)
{
    if (!rdram_ok(a, 1)) return 0;
    volatile u8 *p = rdram(a);
    flush_cpu_dcache_range((const void *)p, 1);
    if (p[0] == (u8)v) return 0;
    p[0] = (u8)v;
    flush_cpu_dcache_range((const void *)p, 1);
    icache_drop(p);
    return 1;
}
static int g_wr16(unsigned a, unsigned v)
{
    if (!rdram_ok(a, 2)) return 0;
    volatile u8 *p = rdram(a);
    flush_cpu_dcache_range((const void *)p, 2);
    if (p[0] == (u8)(v >> 8) && p[1] == (u8)v) return 0;
    p[0] = (u8)(v >> 8); p[1] = (u8)v;
    flush_cpu_dcache_range((const void *)p, 2);
    icache_drop(p);
    return 1;
}
static const n64rt_gs_mem_t s_mem = { g_rd8, g_rd16, g_wr8, g_wr16 };

/* the game ROM's path, as the Pocket has it (the slot 0's file: GETFILE writes it, NUL-ended, to the parameter struct); 0 if it did not answer */
static int rom_path(char *path)
{
    volatile u8 *ps = (volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM;
    int r = -1;
    for (int tries = 0; tries < 1 && r != 0; tries++) {         /* (the Pocket answers in 20 ms: one try) */
        for (u32 i = 0; i < 0x110u; i++) ps[i] = 0;
        flush_cpu_dcache_range((const void *)ps, 0x110u);
        apf_bridge_slot_id_write(0);
        apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
        settle();
        r = command(apf_bridge_request_getfile_write);
    }
    if (r != 0) return 0;
    settle();
    flush_cpu_dcache_range((const void *)ps, 0x110u);
    u32 len = 0;
    for (; len < 255u && ps[len]; len++) path[len] = (char)ps[len];
    path[len] = 0;
    return len != 0;
}

/* for the picker (n64rt_gspick.c): the same path */
int n64rt_cheat_rom_path(char *path) { return rom_path(path); }

/* ---- the status file ---- */
static u32 s_fields, s_changes, s_first_changes;
static char s_gspath[256];
static char *put_s(char *d, const char *s) { while (*s) *d++ = *s++; return d; }
static char *put_u(char *d, u32 v) { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v); while (n) *d++ = t[--n]; return d; }
static char *put_x(char *d, u32 v, int digits) { for (int i = digits - 1; i >= 0; i--) *d++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15u]; return d; }
#ifdef N64RT_GSLOG
static void status_file(const char *what)
{
    if (!s_gspath[0]) return;
    char *const buf = (char *)(uintptr_t)LAYOUT_GSIO;
    char *d = buf;
    d = put_s(d, "GameShark codes, "); d = put_s(d, what); d = put_s(d, "\ncodes from the picker: "); d = put_u(d, s_n);
    d = put_s(d, "\nvideo fields run: "); d = put_u(d, s_fields); d = put_s(d, "\nwrites that changed a value: "); d = put_u(d, s_changes);
    d = put_s(d, " (in the first field: "); d = put_u(d, s_first_changes);
    d = put_s(d, ")\ngame memory: "); d = put_u(d, n64rt_game ? n64rt_game->mem_size : 0u); d = put_s(d, " bytes\nthe first codes, and the value in the RDRAM now:\n");
    for (unsigned i = 0; i < s_n && i < 24u; i++) {
        const unsigned t = s_code[i].type;
        d = put_x(d, t, 2); d = put_x(d, s_code[i].addr, 6); d = put_s(d, " "); d = put_x(d, s_code[i].val, 4); d = put_s(d, "  now ");
        if (t == 0x80 || t == 0xA0 || t == 0xF0 || t == 0xD0 || t == 0xD2) d = put_x(d, g_rd8(s_code[i].addr) & 0xFFu, 2);
        else if (t == 0x81 || t == 0xA1 || t == 0xF1 || t == 0xD1 || t == 0xD3) d = put_x(d, g_rd16(s_code[i].addr) & 0xFFFFu, 4);
        else d = put_s(d, "(a type that is not run)");
        d = put_s(d, "\n");
    }
    const u32 n = (u32)(d - buf);
    volatile u8 *ps = (volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM;
    char lp[256];
    u32 l = 0;
    while (s_gspath[l] && l < 250u) { lp[l] = s_gspath[l]; l++; }              /* the path: the game's .gs with "log" after it */
    lp[l++] = 'l'; lp[l++] = 'o'; lp[l++] = 'g'; lp[l] = 0;
    n64rt_open_param(ps, lp, 3u, n);                                         /* (create, set the size) */
    flush_cpu_dcache_range((const void *)ps, 0x110u);
    flush_cpu_dcache_range((const void *)buf, n);
    apf_bridge_slot_id_write(LAYOUT_GSIO_SLOT);
    apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
    settle();
    int r = command(apf_bridge_request_openfile_write);
    if (r == 0 || r == 1) {
        settle();
        apf_bridge_data_offset_write(0);
        apf_bridge_transfer_length_write(n);
        apf_bridge_ram_data_address_write(LAYOUT_GSIO);
        settle();
        r = command(apf_bridge_request_write_write);
    }
    n64rt_log("n64rt: GameShark: status file (%s): %d\n", what, r);
    apf_bridge_slot_id_write(0);
}
#else
static void status_file(const char *what) { (void)what; }
#endif

/* ---- the engine ---- */
/* The game's start sets the engine up: the list the picker left is taken (once: the picker's own mark is cleared), or there is none */
void n64rt_cheat_init(void)
{
    s_n = 0; s_fields = s_changes = s_first_changes = 0; s_gspath[0] = 0;
    if (!((apf_interact_interact1_read() >> 25) & 1u)) return;                                  /* (GameShark is not chosen: nothing is looked at, nothing is said) */
    flush_cpu_dcache_range((const void *)GSACT, sizeof(n64rt_gsact_t));
    if (GSACT->magic == GSACT_MAGIC) {
        unsigned n = GSACT->n;
        if (n > GS_MAX) n = GS_MAX;
        for (unsigned i = 0; i < n; i++) s_code[i] = ((const n64rt_gs_code_t *)GSACT->code)[i];
        s_n = n;
        n64rt_gsact_get_path(s_gspath, GSACT->path);
        GSACT->magic = 0;
        flush_cpu_dcache_range((const void *)GSACT, 8u);
    }
    n64rt_log("n64rt: GameShark: %lu codes from the picker\n", (unsigned long)s_n);
}

void n64rt_cheat_field(void)
{
    if (!s_n) return;
    if (!((apf_interact_interact1_read() >> 25) & 1u)) { s_n = 0; return; }     /* (GameShark chosen away while the game runs: the codes stop) */
#ifdef N64RT_GSTRACE
    const u64 t0 = now();
#endif
    const u32 ch = n64rt_gs_apply(s_code, s_n, &s_mem);
#ifdef N64RT_GSTRACE
    {   static u64 sum, mx; const u64 d = now() - t0; sum += d; if (d > mx) mx = d;
        if (s_fields == 29u || s_fields == 119u) n64rt_log("n64rt: GameShark: engine, %lu codes: %lu cycles in a field on average, at most %lu (the SoC clock %lu Hz; a field is 1,042,710 cycles)\n", (unsigned long)s_n, (unsigned long)(sum / (s_fields + 1u)), (unsigned long)mx, (unsigned long)CONFIG_CLOCK_FREQUENCY); }
#endif
    s_changes += ch;
    s_fields++;
    if (s_fields == 1u) { s_first_changes = ch; n64rt_log("n64rt: GameShark: the first field: %lu codes run, %lu writes changed a value\n", (unsigned long)s_n, (unsigned long)ch); status_file("after the first field"); }
    else if (s_fields == 600u) status_file("after 10 s");
    else if (s_fields == 3600u) status_file("after a minute");
}
