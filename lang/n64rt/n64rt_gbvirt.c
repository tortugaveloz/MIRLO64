/* The virtual Transfer Pak's cartridges: a Game Boy cartridge made of two files the Pocket loads (data.json's slots
 * LAYOUT_GBROM_SLOT, the ROM, and LAYOUT_GBSAVE_SLOT, the cartridge's battery RAM), for the "Controller 1 pak" setting's
 * Virtual Transfer Pak (n64rt_pak.c); and a second, LAYOUT_GBROM2_SLOT and LAYOUT_GBSAVE2_SLOT, for the "Controller 2 pak" setting's.
 * The two are instances (n = 0, 1) of the same code, each with its own ROM, mapper, RAM and clock; one with no ROM in its slot is no cartridge
 * (the same for both). What a cartridge's bus does is done here: the mapper's registers at 0x0000-0x7FFF,
 * the ROM at 0x0000-0x7FFF, the RAM (or the clock) at 0xA000. The game's own osGbpak* code runs on top of it exactly as on
 * a real one.
 *
 * The mappers: none, MBC1, MBC2, MBC3 (with its clock) and MBC5, by the header's cartridge type (0x147); any other type is
 * not a cartridge here (it is not there for the game). The ROM's banks are its size rounded up to a power of two; its bytes
 * beyond the file read as 0xFF. The RAM's size is the header's (0x149; MBC2's 512 half-bytes are its own).
 *
 * The save file is the cartridge's RAM as the emulators have it; with MBC3's clock, 48 bytes follow it: the clock as
 * 10 little-endian words (seconds, minutes, hours, days, days' control, then the same latched) and a 64-bit time that is
 * left 0. The clock runs while the core does and is not kept for the time it is off (the core has no calendar).
 * A cartridge's RAM with no file is 0xFF. The slot takes a file size (what the Pocket writes back) when the game first
 * writes to the RAM or the clock, so a game that never saves leaves no file.
 *
 * Powering the cartridge on (the Transfer Pak's 0xB000) resets the mapper's registers, as a real cartridge's reset does:
 * the RAM disabled, the first ROM banks, the RAM's bank 0. */
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"

#define ROM_MEM    ((const volatile u8 *)(uintptr_t)g.rom_base)
#define RAM_MEM    ((volatile u8 *)(uintptr_t)(g.n ? LAYOUT_GBSAVE2 : LAYOUT_GBSAVE))
#define SAVE_SLOT  (g.n ? LAYOUT_GBSAVE2_SLOT : LAYOUT_GBSAVE_SLOT)
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
    u8  sized;                                  /* the save slot has its file size */
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

static void need_file(void)
{
    if (g.sized) return;
    g.sized = 1;
    u32 want = g.ram_bytes + (g.timer ? RTC_FOOT : 0u);
    if (n64rt_slot_size(SAVE_SLOT, 0) < want) n64rt_slot_size(SAVE_SLOT, want);
}
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
    need_file();
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

/* ---- the save slot -------------------------------------------------------------- */
/* the RAM's memory from the slot: what the file has, the rest 0xFF (the clock's footer 0) */
static void ram_init(void)
{
    u32 total = g.ram_bytes + (g.timer ? RTC_FOOT : 0u), have = n64rt_slot_size(SAVE_SLOT, 0);
    if (have > LAYOUT_GBSAVE_MAX) have = LAYOUT_GBSAVE_MAX;
    flush_cpu_dcache_range((const void *)RAM_MEM, total);
    for (u32 i = have < total ? have : total; i < total; i++) RAM_MEM[i] = i < g.ram_bytes ? 0xFFu : 0u;
    g.sized = have >= total;
    g.l[0] = g.l[1] = g.l[2] = g.l[3] = g.l[4] = 0;
    g.s = g.m = g.h = g.d = 0; g.halt = g.carry = 0; g.t0 = now();
    if (g.timer) rtc_load();
}

/* ---- the header ------------------------------------------------------------------ */
int n64rt_gbv_look(int n)
{
    u8 h[0x50];
    sel(n);
    u32 size = n64rt_slot_size(n ? LAYOUT_GBROM2_SLOT : LAYOUT_GBROM_SLOT, 0);
    if (size > LAYOUT_GBROM_MAX) size = LAYOUT_GBROM_MAX;
    if (size < 0x150u) { g.in = 0; return 0; }
    g.rom_base = n ? LAYOUT_GBROM2 : LAYOUT_GBROM;
    flush_cpu_dcache_range((const void *)(ROM_MEM + 0x100), sizeof h);
    for (u32 i = 0; i < sizeof h; i++) h[i] = ROM_MEM[0x100u + i];
    u8 x = 0;
    for (u32 i = 0x34; i <= 0x4C; i++) x = (u8)(x - h[i] - 1u);
    if (x != h[0x4D] || crc32(h + 4, 48) != LOGO_CRC) { g.in = 0; return 0; }
    u32 id = crc32(h, sizeof h);
    if (g.in && id == g.id && size == g.rom_bytes) return 1;
    /* (a cartridge new to the slot) */
    u8 t = h[0x47], timer = 0, k;
    switch (t) {
    case 0x00: case 0x08: case 0x09: k = K_ROM; break;
    case 0x01: case 0x02: case 0x03: k = K_MBC1; break;
    case 0x05: case 0x06: k = K_MBC2; break;
    case 0x0F: case 0x10: timer = 1; k = K_MBC3; break;
    case 0x11: case 0x12: case 0x13: k = K_MBC3; break;
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: k = K_MBC5; break;
    default: g.in = 0; return 0;                /* another kind of cartridge: not one here */
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
    g.in = 1;
    ram_init();
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: virtual cartridge: type %02x, %lu ROM banks, %lu bytes of RAM%s\n", t, (unsigned long)g.rom_banks,
              (unsigned long)g.ram_bytes, timer ? ", a clock" : "");
#endif
    return 1;
}
u32 n64rt_gbv_id(int n) { sel(n); return g.id; }

/* a save state: the RAM and the clock's footer, as many bytes as the cartridge has, rounded to the words the state's tokens are */
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
    } else if (g.in && g.timer) { rtc_tick(); rtc_store(); }
}

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
        if (g.timer && g.ram_sel <= 12u) { need_file(); rtc_write(g.ram_sel, v); }
        return;
    }
    if (g.kind == K_MBC3 && g.ram_sel > 3u) return;
    u32 r = ram_off(o);
    if (r >= g.ram_bytes) return;
    need_file();
    RAM_MEM[r] = g.kind == K_MBC2 ? (u8)(v & 0xFu) : v;
}
