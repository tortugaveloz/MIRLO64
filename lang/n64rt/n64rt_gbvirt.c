/* The Transfer Pak (SD)'s cartridges: a Game Boy cartridge made of the ROM the Pocket loads (data.json's slots
 * LAYOUT_GBROM_SLOT, for the "Controller 1 pak" setting's Transfer Pak (SD) (n64rt_pak.c), and LAYOUT_GBROM2_SLOT for the "Controller 2 pak"
 * setting's) and a battery RAM that is the Game Boy game's own save file, read and written by its path. The two are instances (n = 0, 1)
 * of the same code, each with its own ROM, mapper, RAM and clock; one with no ROM in its slot is no cartridge
 * (the same for both). What a cartridge's bus does is done here: the mapper's registers at 0x0000-0x7FFF,
 * the ROM at 0x0000-0x7FFF, the RAM (or the clock) at 0xA000. The game's own osGbpak* code runs on top of it exactly as on
 * a real one.
 *
 * The mappers: none, MBC1, MBC2, MBC3 (with its clock) and MBC5, by the header's cartridge type (0x147); any other type is
 * not a cartridge here (it is not there for the game). The ROM's banks are its size rounded up to a power of two; its bytes
 * beyond the file read as 0xFF. The RAM's size is the header's (0x149; MBC2's 512 half-bytes are its own).
 *
 * The save is the file <ROM's name>.sav which a Game Boy core also keeps: the Pocket tells the runtime the ROM's path, and the save is in the
 * Saves folder of the same platform (/Assets/gbc/common/x.gb: /Saves/gbc/common/x.sav; where the ROM's path says neither, a .gbc: gbc, a .gb: gb,
 * then gbc if the file is not in gb): the cartridge's RAM as the emulators have it; with MBC3's clock, 48 bytes follow it: the clock as
 * 10 little-endian words (seconds, minutes, hours, days, days' control, then the same latched) and a 64-bit time that is
 * left 0. The clock runs while the core does and is not kept for the time it is off (the core has no calendar).
 * The RAM lives in memory (LAYOUT_GBSAVE, LAYOUT_GBSAVE2) and is read from the file when a ROM is new to the pak (n64rt_gbv_field, a video field after
 * the Pocket has loaded it: the cartridge is not there for the game before): with no file it is 0xFF and the file is made by the first write back. It is
 * written back (flush) only if the game has written to it, when the game powers the cartridge off, when the ROM is replaced or gone, and a couple
 * of seconds after its last write (a state carries the RAM, so a state loaded writes it). Every command to the Pocket is made from n64rt_gbv_field
 * (the video field's context, where the GameShark's file is read too), never from the game's accesses to the pak.
 *
 * Powering the cartridge on (the Transfer Pak's 0xB000) resets the mapper's registers, as a real cartridge's reset does:
 * the RAM disabled, the first ROM banks, the RAM's bank 0. */
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"
#include "n64rt_gbpath.h"


#define ROM_MEM    ((const volatile u8 *)(uintptr_t)g.rom_base)
#define RAM_MEM    ((volatile u8 *)(uintptr_t)(g.n ? LAYOUT_GBSAVE2 : LAYOUT_GBSAVE))
#define FILE_SLOT  (g.n ? LAYOUT_GBSAVE2_SLOT : LAYOUT_GBSAVE_SLOT)   /* the data slot the save file is opened into: no file of its own, writable */
#define RTC_FOOT 48u
#define LOGO_CRC 0x46195417u                    /* CRC-32 of the header's 48 logo bytes */

enum { K_NONE, K_ROM, K_MBC1, K_MBC2, K_MBC3, K_MBC5 };

typedef struct {
    u8  n;                                      /* which cartridge: 0 (controller 1's), 1 (controller 2's) */
    u32 rom_base;                               /* where its ROM is: its own slot's memory */
    u8  in, kind, timer;                        /* a cartridge is there; its mapper; it has a clock */
    u8  ram_en, mode, hi2, ram_sel, latch_arm;  /* the mapper's registers */
    u16 rom_bank;                               /* the one at 0x4000, as written (MBC1: its low 5 bits) */
    u32 rom_bytes, rom_banks;                   /* the file's size; the banks, a power of two */
    u32 ram_bytes, ram_banks;                   /* the RAM (MBC3's clock not in it) */
    u32 id;                                     /* the header's CRC */
    char sav[256];                              /* the save file's path ("" if it is not known) */
    u8  have, req;                              /* the RAM (and sav, wr, dirty) are those of the ROM with `id`; a write back is asked for (the cartridge was powered off) */
    u8  wr, dirty;                              /* the file may be written (it was read, or it is not there); the RAM changed since the file was written */
    u8  fails, noans;                           /* write backs that failed (the Pocket's answer was an error; there was no answer) since the last one that did not */
    u32 wcount, wseen;                          /* the writes to the RAM (and the clock), and how many of them the poll has seen */
    u64 t_dirty;                                /* when the poll last saw the RAM change */
    /* the clock: its value at `t0` (uptime cycles) */
    u64 t0;
    u32 s, m, h, d;                             /* d: 9 bits */
    u8  halt, carry;
    u32 l[5];                                   /* latched: seconds, minutes, hours, days low, control */
} gbv_t;
static gbv_t s_gbv[2] = { { .n = 0 }, { .n = 1 } };
static gbv_t *s_cur = &s_gbv[0];
#define g (*s_cur)                              /* (the cartridge the call is for: every function below begins with its n) */
static void sel(int n) { s_cur = &s_gbv[n ? 1 : 0]; }

static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static u32 pow2_up(u32 n) { u32 p = 1; while (p < n) p <<= 1; return p; }

/* ---- the clock --------------------------------------------------------------- */
static void rtc_tick(void)
{
    u64 t = now(), per = CONFIG_CLOCK_FREQUENCY;
    if (g.halt) { g.t0 = t; return; }
    u64 e = (t - g.t0) / per;
    if (!e) return;
    g.t0 += e * per;
    u64 total = (((u64)g.d * 24u + g.h) * 60u + g.m) * 60u + g.s + e;
    g.s = (u32)(total % 60u); total /= 60u;
    g.m = (u32)(total % 60u); total /= 60u;
    g.h = (u32)(total % 24u); total /= 24u;
    if (total >= 512u) { g.carry = 1; total &= 511u; }
    g.d = (u32)total;
}
static u32 rtc_ctl(void) { return (g.d >> 8 & 1u) | (u32)g.halt << 6 | (u32)g.carry << 7; }
static void put32(u32 o, u32 v) { for (int i = 0; i < 4; i++) RAM_MEM[o + (u32)i] = (u8)(v >> (8 * i)); }
static u32 get32(u32 o) { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)RAM_MEM[o + (u32)i] << (8 * i); return v; }
/* the clock into the save's memory (the footer after the RAM) */
static void rtc_store(void)
{
    if (!g.timer) return;
    u32 o = g.ram_bytes;
    put32(o, g.s); put32(o + 4u, g.m); put32(o + 8u, g.h); put32(o + 12u, g.d & 0xFFu); put32(o + 16u, rtc_ctl());
    for (u32 i = 0; i < 5u; i++) put32(o + 20u + 4u * i, g.l[i]);
    put32(o + 40u, 0); put32(o + 44u, 0);
}
static void rtc_load(void)
{
    u32 o = g.ram_bytes, c = get32(o + 16u);
    g.s = get32(o) % 60u; g.m = get32(o + 4u) % 60u; g.h = get32(o + 8u) % 24u;
    g.d = (get32(o + 12u) & 0xFFu) | (c & 1u) << 8;
    g.halt = (u8)(c >> 6 & 1u); g.carry = (u8)(c >> 7 & 1u);
    for (u32 i = 0; i < 5u; i++) g.l[i] = get32(o + 20u + 4u * i);
    g.t0 = now();
}
static void rtc_latch(void)
{
    rtc_tick();
    g.l[0] = g.s; g.l[1] = g.m; g.l[2] = g.h; g.l[3] = g.d & 0xFFu; g.l[4] = rtc_ctl();
    rtc_store();
}
static void rtc_write(u32 reg, u8 v)
{
    rtc_tick();
    switch (reg) {
    case 8:  g.s = v & 63u; break;
    case 9:  g.m = v & 63u; break;
    case 10: g.h = v & 31u; break;
    case 11: g.d = (g.d & 0x100u) | v; break;
    case 12: g.d = (g.d & 0xFFu) | (u32)(v & 1u) << 8; g.carry = (u8)(v >> 7 & 1u);
             if (g.halt && !(v & 0x40u)) g.t0 = now();
             g.halt = (u8)(v >> 6 & 1u);
             break;
    }
    rtc_store();
}

/* ---- the save file, by its path ---------------------------------------------------------------------------- */
/* The Pocket names a nonvolatile slot's file after the N64 game, so a Game Boy game's own save (the file a Game Boy core keeps) cannot be a
 * slot's file: the runtime reads and writes it by its path, with the bridge's target commands. It asks the Pocket for the path of the file in
 * the ROM's slot (the one picked for it: APF_BRIDGE.REQUEST_GETFILE writes a 256-byte NUL-ended absolute path, with its extension, to the
 * parameter struct), makes the .sav's path from its name, and has the Pocket open that path into FILE_SLOT (a slot of data.json with no file, not
 * read-only: REQUEST_OPENFILE; the struct has the path, then 32-bit flags -- bit 0 creates a file that is not there -- and a size), then read it
 * (REQUEST_READ) or write it (REQUEST_WRITE) at the offset 0. The folders are the ones the ROM's path decides (a .gbc: gbc; a
 * .gb: gb, then gbc if the file is not in gb; any other extension: Assets/gbc or Assets/gb in its path, as the .gbc or the .gb), and the Pocket opens files only in the platform folders core.json lists.
 * The parameter struct is LAYOUT_GS_PARAM, which the GameShark's codes use as well: the two are run by the same monitor, one after the other. */
#define IMP_PARAM ((volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM)
#ifdef N64RT_GBTRACE
void n64rt_trace_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define GBLOG(...) do { n64rt_log(__VA_ARGS__); n64rt_trace_log(__VA_ARGS__); } while (0)
#else
#define GBLOG(...) n64rt_log(__VA_ARGS__)
#endif
static void imp_settle(void) { for (volatile int i = 0; i < 256; i++) { } }
static int imp_command(void (*issue)(u32), u32 ms)      /* a target command: 0 done, the result code (1 to 7) if the Pocket failed it, -1 no answer in ms */
{
    (void)apf_bridge_status_read();             /* (a read clears it) */
    issue(1u);
    const u64 end = now() + (u64)CONFIG_CLOCK_FREQUENCY / 1000u * ms;
    while (!apf_bridge_status_read()) if (now() > end) return -1;
    return (int)(apf_bridge_command_result_code_read() & 7u);
}
/* open `path` into FILE_SLOT (flags: bit 0 creates it if it is not there, bit 1 sets its size to `size`): the Pocket's result (0 opened, 1 created,
 * 3 not found, 4 refused, 5 failed), -1 no answer */
static int imp_open(const char *path, u32 flags, u32 size)
{
    for (u32 i = 0; i < 0x110u; i++) IMP_PARAM[i] = 0;
    for (u32 i = 0; i < 255u && path[i]; i++) IMP_PARAM[i] = (u8)path[i];
    *(volatile u32 *)(uintptr_t)(LAYOUT_GS_PARAM + 0x100u) = flags;      /* (the struct's words are the CPU's: big-endian) */
    *(volatile u32 *)(uintptr_t)(LAYOUT_GS_PARAM + 0x104u) = size;
    flush_cpu_dcache_range((const void *)IMP_PARAM, 0x110u);
    apf_bridge_slot_id_write(FILE_SLOT);
    apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
    imp_settle();
    int r = imp_command(apf_bridge_request_openfile_write, 1000u);
    imp_settle();
    return r;
}
/* The ROM slot's file: the path of its save in g.sav ("" if the Pocket does not give the ROM's path or it says neither gb nor gbc); the result is
 * 1 if the save is in the gb folder (and may be in gbc), 2 if in gbc, 0 if none */
static int sav_path(void)
{
    char *out = g.sav, path[256];
    out[0] = 0;
    int r = -1;
    for (int tries = 0; tries < 3 && r != 0; tries++) {         /* (the Pocket answers in 20 ms; a missing answer is tried again) */
        for (u32 i = 0; i < 0x110u; i++) IMP_PARAM[i] = 0;
        flush_cpu_dcache_range((const void *)IMP_PARAM, 0x110u);
        apf_bridge_slot_id_write(g.n ? LAYOUT_GBROM2_SLOT : LAYOUT_GBROM_SLOT);
        apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
        imp_settle();
        r = imp_command(apf_bridge_request_getfile_write, 1000u);
    }
    if (r != 0) { GBLOG("n64rt: Game Boy save: the ROM's file name: %s (%d)\n", r < 0 ? "no answer" : "refused", r); return 0; }
    imp_settle();
    flush_cpu_dcache_range((const void *)IMP_PARAM, 0x110u);
    u32 len = 0;
    for (; len < 255u && IMP_PARAM[len]; len++) path[len] = (char)IMP_PARAM[len];
    path[len] = 0;
    GBLOG("n64rt: Game Boy save: the ROM is %s\n", path);
    const int kind = gbpath_sav(path, out, 256u);
    if (!kind) GBLOG("n64rt: Game Boy save: neither in a gb or gbc folder nor a .gb or .gbc, or no name\n");
    return kind;
}
/* the gb folder's path in g.sav made the gbc folder's, and the other way round */
static void sav_to_gbc(void)
{
    u32 n = 0;
    while (g.sav[n]) n++;
    for (u32 i = n + 1u; i > 17u; i--) g.sav[i] = g.sav[i - 1u];       /* ("/Saves/gb/common/" is 17 bytes, "/Saves/gbc/common/" 18) */
    const char *d = "/Saves/gbc/common/";
    for (u32 i = 0; i < 18u; i++) g.sav[i] = d[i];
}
static void sav_to_gb(void)
{
    u32 n = 0;
    while (g.sav[n]) n++;
    for (u32 i = 18u; i <= n; i++) g.sav[i - 1u] = g.sav[i];
    const char *d = "/Saves/gb/common/";
    for (u32 i = 0; i < 17u; i++) g.sav[i] = d[i];
}

/* The RAM (and the clock's footer) from the file: `total` bytes at RAM_MEM, g.wr set if the file may be written. The file there and as long as
 * the RAM or longer: read (of a longer one, only the first `total` bytes), and written back later; not there: the RAM stays 0xFF and the first write back
 * makes it; shorter than the RAM, or any other answer (the folder refused, no answer): left alone, the RAM is not kept. Each outcome is logged. */
static void sav_read(u32 total)
{
    const u32 prev_slot = apf_bridge_slot_id_read(), prev_len = apf_bridge_transfer_length_read();
    g.wr = 0; g.dirty = 0; g.fails = g.noans = 0; g.wcount = g.wseen = 0;
    const int kind = sav_path();
    if (g.sav[0]) {
        int r = imp_open(g.sav, 0, 0);
        if (r == 3 && kind == 1) {                  /* (a .gb's save may be in gbc too: where a Game Boy core that has both kinds keeps it) */
            GBLOG("n64rt: Game Boy save: %s is not there\n", g.sav);
            sav_to_gbc();
            r = imp_open(g.sav, 0, 0);
            if (r == 3) sav_to_gb();                /* (in neither: it is made where the ROM's platform says) */
        }
        if (r == 3) { g.wr = 1; GBLOG("n64rt: Game Boy save: %s is not there: it is made at the first write\n", g.sav); }
        else if (r != 0) GBLOG("n64rt: Game Boy save: %s: %s (%d): not used\n", g.sav, r < 0 ? "no answer" : r == 4 ? "refused (is its platform in core.json?)" : "not opened", r);
        else {
            u32 size = apf_bridge_file_size_read();
            if (size < g.ram_bytes) GBLOG("n64rt: Game Boy save: %s: %lu bytes, the cartridge's RAM is %lu: not used\n", g.sav, (unsigned long)size, (unsigned long)g.ram_bytes);
            else {
                u32 n = size < total ? size : total;
                apf_bridge_data_offset_write(0);
                apf_bridge_transfer_length_write(n);
                apf_bridge_ram_data_address_write((u32)(uintptr_t)RAM_MEM);
                imp_settle();
                r = imp_command(apf_bridge_request_read_write, 2000u);
                if (r != 0) GBLOG("n64rt: Game Boy save: %s: the read failed (%d): not used\n", g.sav, r);
                else { g.wr = 1; GBLOG("n64rt: Game Boy save: %s: %lu bytes\n", g.sav, (unsigned long)n); }
            }
        }
    }
    apf_bridge_transfer_length_write(prev_len);
    apf_bridge_slot_id_write(prev_slot);
}
/* The RAM to the file, if the game has written to it and the file may be written: `total` bytes from the offset 0 (a longer file keeps its end), the
 * file made, at that size, if it is not there (a shorter one is made as long). A failure leaves the RAM dirty for another try, 5 s later (n64rt_gbv_field):
 * the Pocket does not answer the target commands while it makes a save state, for some 15 s; three answers that are errors in a row end the write backs,
 * so do 20 times with no answer (each costs the second of its wait). */
static void flush(const char *why)
{
    if (!g.have || !g.dirty || !g.wr || !g.sav[0]) return;
    const u32 total = g.ram_bytes + (g.timer ? RTC_FOOT : 0u);
    const u32 prev_slot = apf_bridge_slot_id_read(), prev_len = apf_bridge_transfer_length_read();
    if (g.timer) { rtc_tick(); rtc_store(); }
    flush_cpu_dcache_range((const void *)RAM_MEM, total);
    int r = imp_open(g.sav, 0, 0);
    if (r == 3) r = imp_open(g.sav, 3, total);
    else if (r == 0 && apf_bridge_file_size_read() < total) r = imp_open(g.sav, 3, total);
    if (r == 0 || r == 1) {
        apf_bridge_data_offset_write(0);
        apf_bridge_transfer_length_write(total);
        apf_bridge_ram_data_address_write((u32)(uintptr_t)RAM_MEM);
        imp_settle();
        r = imp_command(apf_bridge_request_write_write, 2000u);
        if (r == 0) { g.dirty = 0; g.fails = g.noans = 0; GBLOG("n64rt: Game Boy save: %s written (%s)\n", g.sav, why); }
        else GBLOG("n64rt: Game Boy save: %s: the write failed (%d) (%s)\n", g.sav, r, why);
    } else GBLOG("n64rt: Game Boy save: %s: %s (%d) (%s)\n", g.sav, r < 0 ? "no answer" : "not opened", r, why);
    if (g.dirty) {
        g.t_dirty = now() + 3u * (u64)CONFIG_CLOCK_FREQUENCY;       /* (the quiet moment the poll waits for is 2 s: this is 5 s from now) */
        g.wseen = g.wcount;
        if (r < 0 ? ++g.noans >= 20u : ++g.fails >= 3u) { g.wr = 0; GBLOG("n64rt: Game Boy save: %s: no more write backs\n", g.sav); }
    }
    apf_bridge_transfer_length_write(prev_len);
    apf_bridge_slot_id_write(prev_slot);
}

/* ---- the save's RAM ----------------------------------------------------------------------------------------- */
/* the RAM's memory from the file: what it has, the rest 0xFF (the clock's footer 0) */
static void ram_init(void)
{
    u32 total = g.ram_bytes + (g.timer ? RTC_FOOT : 0u);
    for (u32 i = 0; i < total; i++) RAM_MEM[i] = i < g.ram_bytes ? 0xFFu : 0u;
    g.wr = 0; g.sav[0] = 0;
    if (g.ram_bytes) sav_read(total);
    flush_cpu_dcache_range((const void *)RAM_MEM, total);
    g.l[0] = g.l[1] = g.l[2] = g.l[3] = g.l[4] = 0;
    g.s = g.m = g.h = g.d = 0; g.halt = g.carry = 0; g.t0 = now();
    if (g.timer) rtc_load();
}

/* ---- the header ------------------------------------------------------------------ */
/* Is a ROM in the slot, a Game Boy one of a kind that is a cartridge here? Its header (0x100..0x14F) in h, the file's size, its header's CRC. */
static int rom_probe(u8 *h, u32 *size, u32 *id)
{
    u32 sz = n64rt_slot_size(g.n ? LAYOUT_GBROM2_SLOT : LAYOUT_GBROM_SLOT, 0);
    if (sz > LAYOUT_GBROM_MAX) sz = LAYOUT_GBROM_MAX;
    if (sz < 0x150u) return 0;
    flush_cpu_dcache_range((const void *)(ROM_MEM + 0x100), 0x50);
    for (u32 i = 0; i < 0x50u; i++) h[i] = ROM_MEM[0x100u + i];
    u8 x = 0;
    for (u32 i = 0x34; i <= 0x4C; i++) x = (u8)(x - h[i] - 1u);
    if (x != h[0x4D] || crc32(h + 4, 48) != LOGO_CRC) return 0;
    switch (h[0x47]) {
    case 0x00: case 0x08: case 0x09: case 0x01: case 0x02: case 0x03: case 0x05: case 0x06: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: break;
    default: return 0;                          /* another kind of cartridge: not one here */
    }
    *size = sz;
    *id = crc32(h, 0x50);
    return 1;
}
/* is the cartridge there for the game: the ROM in the slot is the one the cartridge was made of (n64rt_gbv_field makes it, a video field after the
 * ROM is there, and reads its save) */
int n64rt_gbv_look(int n)
{
    u8 h[0x50];
    u32 size, id;
    sel(n);
    g.rom_base = n ? LAYOUT_GBROM2 : LAYOUT_GBROM;
    if (!g.have || !rom_probe(h, &size, &id) || id != g.id || size != g.rom_bytes) { g.in = 0; return 0; }
    g.in = 1;
    return 1;
}
#ifdef N64RT_GBTRACE
/* (a GBTRACE build) the CRC-32 of the ROM and of the RAM as the CPU reads them (through its D-cache) and as the memory has them: a difference is a line
 * of the cache that the Pocket's bridge wrote under. `rom` is how much of the ROM from its start. */
static u32 s_crct[256];
static u32 crc_of(const volatile u8 *p, u32 n)
{
    if (!s_crct[1]) for (u32 i = 0; i < 256u; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); s_crct[i] = c; }
    u32 c = 0xFFFFFFFFu;
    while (n--) c = s_crct[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}
static void trace_crcs(const char *why, u32 rom)
{
    const u32 ram = g.ram_bytes + (g.timer ? RTC_FOOT : 0u);
    if (rom > g.rom_bytes) rom = g.rom_bytes;
    const u32 rc = crc_of(ROM_MEM, rom), ac = crc_of(RAM_MEM, ram);
    flush_cpu_dcache();
    const u32 rm = crc_of(ROM_MEM, rom), am = crc_of(RAM_MEM, ram);
    GBLOG("n64rt: SD cartridge %d (%s): ROM %lu bytes crc %08lx (memory %08lx), RAM %lu bytes crc %08lx (memory %08lx)%s\n", g.n, why, (unsigned long)rom,
          (unsigned long)rc, (unsigned long)rm, (unsigned long)ram, (unsigned long)ac, (unsigned long)am, rc != rm || ac != am ? " DIFFERENT" : "");
}
#else
#define trace_crcs(why, rom) ((void)0)
#endif

/* A ROM that is new to the slot (called from n64rt_gbv_field): the cartridge it was is written back to its own file, and the new one made, with its
 * save read from the Game Boy game's own file. */
static void rom_new(const u8 *h, u32 size, u32 id)
{
    flush("another ROM");
    g.have = 0; g.in = 0;
    u8 t = h[0x47], timer = 0, k;
    switch (t) {
    case 0x00: case 0x08: case 0x09: k = K_ROM; break;
    case 0x01: case 0x02: case 0x03: k = K_MBC1; break;
    case 0x05: case 0x06: k = K_MBC2; break;
    case 0x0F: case 0x10: timer = 1; k = K_MBC3; break;
    case 0x11: case 0x12: case 0x13: k = K_MBC3; break;
    default: k = K_MBC5; break;
    }
    static const u32 ram_size[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
    flush_cpu_dcache();                         /* (the bridge wrote the ROM: no stale line of it) */
    g.kind = k; g.timer = timer;
    g.rom_bytes = size;
    g.rom_banks = pow2_up((size + 0x3FFFu) >> 14);
    if (g.rom_banks < 2u) g.rom_banks = 2u;
    g.ram_bytes = k == K_MBC2 ? 0x200u : h[0x49] < 6u ? ram_size[h[0x49]] : 0u;
    g.ram_banks = g.ram_bytes >= 0x2000u ? g.ram_bytes >> 13 : g.ram_bytes ? 1u : 0u;
    g.id = id;
    ram_init();
    g.have = 1;
    g.in = 1;
    GBLOG("n64rt: SD cartridge %d: type %02x, %lu ROM banks, %lu bytes of RAM%s\n", g.n, t, (unsigned long)g.rom_banks,
          (unsigned long)g.ram_bytes, timer ? ", a clock" : "");
    trace_crcs("made", g.rom_bytes);
}
u32 n64rt_gbv_id(int n) { sel(n); return g.id; }

/* a save state: the RAM and the clock's footer, as many bytes as the cartridge has, rounded to the words the state's tokens are. The file is not
 * written now: the Pocket answers no target command while it makes the state (the RAM, dirty or not, is in the state), and it is written at the next quiet moment */
u32 n64rt_gbv_state_bytes(int n)
{
    sel(n);
    if (!g.in) return 0;
    rtc_tick(); rtc_store();
    return (g.ram_bytes + (g.timer ? RTC_FOOT : 0u) + 15u) & ~15u;
}
/* after a state is put back (the cartridge's registers and RAM are in it): the clock, which counts the SoC's cycles, goes on from where it was; and the
 * ROM, which the Pocket loads as for any launch, must be the same file: with none, or another, the cartridge is not in */
void n64rt_gbv_resume(int n, u64 delta)
{
    sel(n);
    if (!g.in) return;
    g.dirty = 1; g.wcount++;                    /* (the state's RAM is not the file's: it is written at the next quiet moment) */
    g.t0 += delta;
    u32 size = n64rt_slot_size(n ? LAYOUT_GBROM2_SLOT : LAYOUT_GBROM_SLOT, 0);
    if (size > LAYOUT_GBROM_MAX) size = LAYOUT_GBROM_MAX;
    if (size < 0x150u) { g.in = 0; return; }
    u8 h[0x50];
    flush_cpu_dcache_range((const void *)(ROM_MEM + 0x100), sizeof h);
    for (u32 i = 0; i < sizeof h; i++) h[i] = ROM_MEM[0x100u + i];
    if (crc32(h, sizeof h) != g.id || size != g.rom_bytes) g.in = 0;
}

/* the cartridge's reset */
void n64rt_gbv_power(int n, int on)
{
    sel(n);
    if (on) {
        g.ram_en = 0; g.mode = 0; g.hi2 = 0; g.ram_sel = 0; g.latch_arm = 0;
        g.rom_bank = 1;
        flush_cpu_dcache();                     /* (what the Pocket's bridge wrote to the ROM's and the RAM's memory since the cartridge was made is read, not what the cache has) */
#ifdef N64RT_GBTRACE
        { static u8 s_first[2]; if (g.in && !s_first[g.n]) { s_first[g.n] = 1; trace_crcs("the game powers it on", 0x10000u); } }
#endif
    } else {
        g.req = 1;                              /* (written back by n64rt_gbv_field: no command to the Pocket from the game's own access) */
        if (g.in && g.timer) { rtc_tick(); rtc_store(); }
    }
}

/* every video field (n64rt_io.c; the commands to the Pocket are made here only): the ROM in each slot is looked at, some times a second -- a ROM new to the
 * slot makes its cartridge and reads its save, one that is gone writes it back --; and a cartridge whose RAM the game wrote and has not written for
 * FLUSH_QUIET is written back, as a core that is quit leaves no word for the runtime (the Pocket tells the core nothing then) */
#define FLUSH_QUIET (2u * CONFIG_CLOCK_FREQUENCY)
static void poll(int n, int look)
{
    sel(n);
    if (n64rt_pak_kind(n) != N64RT_PAK_VIRTUAL) {      /* (a ROM is loaded for a pak that is not the SD one: nothing is read or written for it) */
        if (g.have) { flush("the pak is not the SD one"); g.have = 0; g.in = 0; }
        return;
    }
    if (look || !g.have) {
        u8 h[0x50];
        u32 size, id;
        g.rom_base = n ? LAYOUT_GBROM2 : LAYOUT_GBROM;
        if (!rom_probe(h, &size, &id)) {
            if (g.have) { flush("the ROM is gone"); g.have = 0; g.in = 0; }
        } else if (!g.have || id != g.id || size != g.rom_bytes) rom_new(h, size, id);
    }
    if (!g.have) return;
    if (g.req) { g.req = 0; flush("the cartridge's power off"); }
    if (!g.dirty || !g.wr) return;
    if (g.wcount != g.wseen) { g.wseen = g.wcount; g.t_dirty = now(); return; }
    if (now() >= g.t_dirty && now() - g.t_dirty >= FLUSH_QUIET) flush("quiet");
}
void n64rt_gbv_field(void)
{
    static u32 s_f;
    const int look = (s_f++ & 7u) == 0;
    for (int n = 0; n < 2; n++) poll(n, look);
}

/* The pak's setting is the SD one and a ROM is in the slot: its cartridge is made now, not at the next video field (n64rt_pak_init, as the game starts: a game
 * that looks at its pak once, soon after it boots, finds a ROM that was loaded before it started; the pak's first look at the slot is this one) */
void n64rt_gbv_make(int n) { poll(n, 1); }

/* The game is about to start over (n64rt_restart.c, the "Restart Game" action), as if the console were switched off and on with the
 * cartridges left in the Transfer Paks. The ROM loaded last is looked at now (a cartridge not made yet is made, and its save read), the RAM the
 * game wrote is written to the file at once (the quiet moment is not waited for), and each cartridge is powered off and on, which resets its
 * mapper. The cartridges themselves -- ROM, RAM, clock, the file's path -- stay as they are (n64rt_gbv_keep): the first status the game reads
 * after it starts finds them in. */
void n64rt_gbv_restart_prep(void)
{
    for (int n = 0; n < 2; n++) {
        poll(n, 1);
        sel(n);
        flush("the game restarts");
        if (g.have) trace_crcs("the game restarts", g.rom_bytes);
        n64rt_gbv_power(n, 0);
        n64rt_gbv_power(n, 1);
    }
}
/* (a GBTRACE build: the mapper's registers, for the trace of the game's accesses to the pak) */
void n64rt_gbv_regs(int n, u32 *v)
{
    sel(n);
    v[0] = g.kind; v[1] = g.rom_bank; v[2] = g.ram_sel; v[3] = g.ram_en; v[4] = (u32)g.mode | (u32)g.hi2 << 1; v[5] = g.in;
}
/* the cartridges' state, which a restart keeps whole (the runtime's other memory starts over) */
void *n64rt_gbv_keep(u32 *bytes) { *bytes = sizeof s_gbv; return s_gbv; }

/* ---- the bus ----------------------------------------------------------------------- */
static u8 rom_at(u32 off) { return off < g.rom_bytes ? ROM_MEM[off] : 0xFFu; }
static u32 bank_hi(void)
{
    switch (g.kind) {
    case K_MBC1: return (((u32)g.hi2 << 5) | (g.rom_bank & 0x1Fu ? g.rom_bank & 0x1Fu : 1u)) & (g.rom_banks - 1u);
    case K_MBC2: return (g.rom_bank & 0xFu ? g.rom_bank & 0xFu : 1u) & (g.rom_banks - 1u);
    case K_MBC3: return (g.rom_bank & 0x7Fu ? g.rom_bank & 0x7Fu : 1u) & (g.rom_banks - 1u);
    case K_MBC5: return g.rom_bank & (g.rom_banks - 1u);
    default:     return 1u;
    }
}
static u32 ram_off(u32 o)                       /* a RAM address in the 8 KiB window -> its place in the RAM, or ~0 */
{
    if (!g.ram_bytes) return ~0u;
    if (g.kind == K_MBC2) return o & 0x1FFu;
    u32 bank = g.kind == K_MBC1 ? (g.mode ? g.hi2 : 0u) : g.ram_sel;
    if (g.ram_bytes < 0x2000u) return o & (g.ram_bytes - 1u);
    return (bank % g.ram_banks) * 0x2000u + o;
}

/* The pak reads a block of 32 bytes (n64rt_pak.c) from the cartridge address a: the cache's lines of its bytes in the ROM's or the RAM's memory are dropped first,
 * so that what the game reads is what the memory has -- the Pocket's bridge wrote the ROM, and the RAM from the save's file, under the cache -- not a line
 * an earlier read left. A block is 32-byte aligned and so is in one bank. */
void n64rt_gbv_block(int n, u32 a)
{
    sel(n);
    if (!g.in) return;
    if (a < 0x4000u) flush_cpu_dcache_range((const void *)(ROM_MEM + ((g.kind == K_MBC1 && g.mode ? ((u32)g.hi2 << 5) & (g.rom_banks - 1u) : 0u) << 14 | a)), 32);
    else if (a < 0x8000u) flush_cpu_dcache_range((const void *)(ROM_MEM + (bank_hi() << 14 | (a & 0x3FFFu))), 32);
    else if (a >= 0xA000u && a < 0xC000u && g.ram_en) {
        const u32 r = ram_off(a & 0x1FFFu);
        if (r < g.ram_bytes) flush_cpu_dcache_range((const void *)(RAM_MEM + r), 32);
    }
}

u8 n64rt_gbv_read(int n, u32 a)
{
    sel(n);
    if (!g.in) return 0xFFu;
    if (a < 0x4000u) return rom_at((g.kind == K_MBC1 && g.mode ? ((u32)g.hi2 << 5) & (g.rom_banks - 1u) : 0u) << 14 | a);
    if (a < 0x8000u) return rom_at(bank_hi() << 14 | (a & 0x3FFFu));
    if (a < 0xA000u || a >= 0xC000u || !g.ram_en) return 0xFFu;
    u32 o = a & 0x1FFFu;
    if (g.kind == K_MBC3 && g.ram_sel >= 8u) {
        if (!g.timer || g.ram_sel > 12u) return 0xFFu;
        return (u8)g.l[g.ram_sel - 8u];
    }
    if (g.kind == K_MBC3 && g.ram_sel > 3u) return 0xFFu;
    u32 r = ram_off(o);
    if (r >= g.ram_bytes) return 0xFFu;
    return g.kind == K_MBC2 ? (u8)(0xF0u | RAM_MEM[r]) : RAM_MEM[r];
}

void n64rt_gbv_write(int n, u32 a, u8 v)
{
    sel(n);
    if (!g.in) return;
    if (a < 0x8000u) {
        switch (g.kind) {
        case K_MBC1:
            if (a < 0x2000u) g.ram_en = (v & 0xFu) == 0xAu;
            else if (a < 0x4000u) g.rom_bank = v & 0x1Fu;
            else if (a < 0x6000u) g.hi2 = v & 3u;
            else g.mode = v & 1u;
            break;
        case K_MBC2:
            if (a < 0x4000u) { if (a & 0x100u) g.rom_bank = v & 0xFu; else g.ram_en = (v & 0xFu) == 0xAu; }
            break;
        case K_MBC3:
            if (a < 0x2000u) g.ram_en = (v & 0xFu) == 0xAu;
            else if (a < 0x4000u) g.rom_bank = v & 0x7Fu;
            else if (a < 0x6000u) g.ram_sel = v & 0xFu;
            else { if (g.latch_arm && v == 1u && g.timer) rtc_latch(); g.latch_arm = v == 0u; }
            break;
        case K_MBC5:
            if (a < 0x2000u) g.ram_en = (v & 0xFu) == 0xAu;
            else if (a < 0x3000u) g.rom_bank = (u16)((g.rom_bank & 0x100u) | v);
            else if (a < 0x4000u) g.rom_bank = (u16)((g.rom_bank & 0xFFu) | (u16)(v & 1u) << 8);
            else if (a < 0x6000u) g.ram_sel = v & 0xFu;
            break;
        default: break;
        }
        return;
    }
    if (a < 0xA000u || a >= 0xC000u || !g.ram_en) return;
    u32 o = a & 0x1FFFu;
    if (g.kind == K_MBC3 && g.ram_sel >= 8u) {
        if (g.timer && g.ram_sel <= 12u) { rtc_write(g.ram_sel, v); g.wcount++; g.dirty = 1; }
        return;
    }
    if (g.kind == K_MBC3 && g.ram_sel > 3u) return;
    u32 r = ram_off(o);
    if (r >= g.ram_bytes) return;
    g.wcount++; g.dirty = 1;
    RAM_MEM[r] = g.kind == K_MBC2 ? (u8)(v & 0xFu) : v;
}
