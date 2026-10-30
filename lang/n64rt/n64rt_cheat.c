/* GameShark codes: the setting "Enable Gameshark codes" (interact.json: INTERACT1[25]). With it on the
 * runtime looks, once, for the game's codes in a text file on the SD card -- Assets/n64/common/gameshark/<ID>.<version>.txt, else
 * <ID>.txt, where <ID> is the cartridge's four characters (ROM 0x3B..0x3E: "NSME": the game's code and its region) and <version> the
 * header's version byte, in decimal -- and runs every code in it in every video field (n64rt_gs.h has the format and the code types), as a
 * GameShark does from its hook on the vertical retrace. No code is left out: the file holds the ones to run.
 *
 * The file is read by the Pocket at the runtime's request, with the bridge's target commands (docs/control.md, APF_BRIDGE): the open of
 * data.json's slot LAYOUT_GS_SLOT to a path (the parameter struct at LAYOUT_GS_PARAM, which the Pocket reads from the SDRAM through the
 * bridge), then a read of the slot into LAYOUT_GS. A file that is not there is a command that fails: no codes. The memory the codes write
 * is the game's RDRAM (LAYOUT_RDRAM; with 4 MiB the upper half is not written), and what they change is made visible to the game's
 * caches. */
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"
#include "n64rt_gs.h"

#define GS_MAX 512u
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
static u32 s_changed_code;
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
    s_changed_code = 1;
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
    s_changed_code = 1;
    return 1;
}
static const n64rt_gs_mem_t s_mem = { g_rd8, g_rd16, g_wr8, g_wr16 };

static int load(const char *path)
{
    volatile u8 *ps = (volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM;
    for (u32 i = 0; i < 0x110u; i++) ps[i] = 0;
    for (u32 i = 0; i < 255u && path[i]; i++) ps[i] = (u8)path[i];       /* (the flags and the size after the 256 bytes: 0) */
    apf_bridge_slot_id_write(LAYOUT_GS_SLOT);
    apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
    settle();
    int r = command(apf_bridge_request_openfile_write);
    if (r != 0) { n64rt_log("n64rt: GameShark: %s: %s (%d)\n", path, r < 0 ? "no answer" : "no file", r); return 0; }
    settle();
    u32 size = apf_bridge_file_size_read();
    if (size == 0 || size > LAYOUT_GS_MAX) size = size ? LAYOUT_GS_MAX : 0;
    if (size == 0) { n64rt_log("n64rt: GameShark: %s is empty\n", path); return 0; }
    apf_bridge_data_offset_write(0);
    apf_bridge_transfer_length_write(size);
    apf_bridge_ram_data_address_write(LAYOUT_GS);
    settle();
    r = command(apf_bridge_request_read_write);
    if (r != 0) { n64rt_log("n64rt: GameShark: %s: the read failed (%d)\n", path, r); return 0; }
    flush_cpu_dcache_range((const void *)(uintptr_t)LAYOUT_GS, size);
    s_n = n64rt_gs_parse((const char *)(uintptr_t)LAYOUT_GS, size, s_code, GS_MAX);
    n64rt_log("n64rt: GameShark: %s: %lu codes\n", path, (unsigned long)s_n);
    return 1;
}

static void load_codes(void)
{
    const u32 w = n64rt_rom_word(0x38u), v = n64rt_rom_word(0x3Cu);      /* 0x3B..0x3E: the ID, 0x3F: the version */
    char id[5] = { (char)w, (char)(v >> 24), (char)(v >> 16), (char)(v >> 8), 0 };
    for (int i = 0; i < 4; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) id[i] = '_';
    }
    char plain[48] = "/Assets/n64/common/gameshark/", with[56];
    memcpy(plain + 29, id, 4);
    memcpy(plain + 33, ".txt", 5);
    memcpy(with, plain, 33);
    char *p = with + 33;
    *p++ = '.';
    char dig[4];
    int nd = 0;
    u32 r = v & 0xFFu;
    do { dig[nd++] = (char)('0' + r % 10u); r /= 10u; } while (r);
    while (nd) *p++ = dig[--nd];
    memcpy(p, ".txt", 5);
    if (!load(with)) load(plain);
    apf_bridge_slot_id_write(0);
}

/* The setting is looked at in every field: the Pocket sends the core's settings as it starts, some time after the runtime does, and the box
 * can be changed while the game runs. With it on and no codes yet the file is read; with it off the codes are dropped. */
static u8 s_loaded;

void n64rt_cheat_init(void) { s_n = 0; s_loaded = 0; }

void n64rt_cheat_field(void)
{
    const int on = (apf_interact_interact1_read() >> 25 & 1u) != 0;
    if (on && !s_loaded) { s_loaded = 1; load_codes(); }
    else if (!on && s_loaded) { s_loaded = 0; s_n = 0; }
    if (!s_n) return;
    s_changed_code = 0;
    n64rt_gs_apply(s_code, s_n, &s_mem);
    if (s_changed_code) flush_cpu_icache();                  /* (a code may have changed an instruction) */
}
