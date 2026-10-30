/* The accessory in controller 1 -- the core's "Controller 1 pak" setting
 * (interact.json: word 0's bits 4-0), read at every access, so it can be
 * changed while a game runs -- and in controller 2: the "Controller 2 pak" setting (word 0's bits 24-20), None or a
 * Virtual Transfer Pak, which makes controller 2 a pad that is there (with nothing pressed, when the dock reports none) so that
 * the game finds the pak. The values of controller 1's:
 *   0  nothing (the default, and what the setting word is until the Pocket
 *      has sent it: the cartridge slot is left alone)
 *   1  a Controller Pak
 *   2  a Transfer Pak with the Game Boy cartridge that is in the Pocket's
 *      own slot
 *   3  a Transfer Pak with a virtual cartridge: a Game Boy ROM and its save,
 *      files the Pocket loads (the slots "GB Transfer Pak #1" and "Game Boy Save #1"; n64rt_gbvirt.c)
 * Any other value is nothing. Controller 2's takes 3 as well (its own cartridge: the slots "#2"); controllers 3 and 4 have none.
 *
 * It is the accessory as the controller's joybus sees it: 32-byte blocks
 * read and written at a 16-bit address. The game's own libultra runs on top
 * of it -- osPfs*, osGbpak* are the game's code; the runtime's part is
 * __osContRamRead / __osContRamWrite / __osPfsGetStatus / osPfsIsPlug and
 * the PIF's commands 2 and 3 (n64rt_io.c). lang/n64rt/test runs libultra's
 * code against it.
 *
 * The Controller Pak: 32 KiB, data.json's slot 2 ("Controller Pak", the
 * game's name with .mpk). An image that is not a formatted pak (a new file)
 * is formatted, empty.
 *
 * Where the file's memory is: in the SDRAM between the ROM and the RDRAM, like
 * the EEPROM (n64rt_io.c; lang/mips/include/layout.h) -- a part that no ROM
 * reaches, whatever its size, so it does not matter in which order the
 * Pocket writes its slots, and every game has it, whatever RDRAM it uses. The
 * nonvolatile slot takes a file size (what the Pocket writes back) only
 * when the game first writes to it, so a game that never uses it leaves
 * no file.
 *
 * The Transfer Pak:
 *   0x8000  its power: 0x84 on, 0xFE off; reads back 0x84 while on
 *   0xA000  which 16 KiB of the cartridge's address space is at 0xC000
 *   0xB000  bit 0 the cartridge's power (written); read: bit 7 a cartridge
 *           is in, bit 0 its power, bit 3 its reset line released (with the
 *           power), bit 2 once after the power was written, bit 6 once
 *           after the cartridge in the slot was found gone or another one
 *   0xC000  the cartridge
 *
 * The cartridge in the slot is reached a byte at a time through the SoC's
 * GB_CART registers (rtl/soc/gb_cart.sv, a bitstream built with
 * MIRLO_GB_CART): what the game reads and writes at 0xC000 goes to the
 * cartridge's own bus, so its mapper, its RAM and its battery are the real
 * ones -- nothing is emulated and nothing is saved by the Pocket. The
 * cartridge is held in reset except while the game has its power on (0xB000)
 * and while it is looked for: its header (0x100-0x14F) is read, with nothing
 * but its address pins and /RD driven, when the core starts, when the game
 * turns its power on, and once a second while the game asks for the status
 * with the power off. It is there when the header's checksum and the logo
 * (by its CRC) are right; only then are writes and the RAM's chip select let
 * through (GB_CART.CTRL's TRUST), so that a cartridge of another kind left in
 * the slot never sees more than reads. On a SoC without the registers
 * (GB_CART.ID reads 0) the Transfer Pak is there with no cartridge in it.
 *
 * The virtual Transfer Pak is the same accessory with another cartridge: what the game reads and writes at 0xC000 goes to
 * n64rt_gbvirt.c's mapper, ROM and RAM, and the Pocket keeps its save. The cartridge is looked for in its slot where the
 * physical one is looked for in the Pocket's. It does not touch the port, so a cartridge in the Pocket's slot is left alone. */
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include "n64rt.h"

#define CPAK_SLOT     LAYOUT_CPAK_SLOT          /* data.json's */
#define CPAK_SDRAM    ((u32)LAYOUT_CPAK)
#define PAK_BYTES     ((u32)LAYOUT_PAK_BYTES)

#define NPAK 2                                  /* the controllers with an accessory: 1 and 2 */
static struct { u8 on, access, changed, bank; } s_tpv[NPAK];   /* the Transfer Pak of each */
static u8 s_sized;                              /* the Controller Pak's file size is in the table */

/* ---- the cartridge in the Pocket's slot ---------------------------------------- */
#define GBC_ID    0x47420001u
#define LOGO_CRC  0x46195417u                   /* CRC-32 of the header's 48 logo bytes */
static struct {
    u8  hw;                                     /* the SoC has the port */
    u8  virt;                                   /* the cartridge looked for, and the one the power is on for, is the virtual one */
    u8  in;                                     /* a Game Boy cartridge answered the last look */
    u8  bus;                                    /* the port is on (the game's power) */
    u8  pulled;                                 /* another cartridge, or none, since the status was read */
    u32 id;                                     /* its header's CRC */
    u64 looked;                                 /* when */
} s_slotv[NPAK];                                /* (the real cartridge's port, `hw`, is controller 1's alone) */
static int s_ch;                                /* the controller the call is for: the two names below are its */
#define s_slot (s_slotv[s_ch])
#define s_tp   (s_tpv[s_ch])
static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static void wait_us(u32 us) { u64 e = now() + (u64)us * (CONFIG_CLOCK_FREQUENCY / 1000000u); while (now() < e) { } }
static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
/* one byte on the cartridge's bus; 0 if the port does not answer (never: it is then left alone) */
static int slot_xfer(u32 addr, u32 we, u8 wd, u8 *rd)
{
    gb_cart_access_write(we << 24 | (addr & 0xFFFFu) << 8 | wd);
    for (u32 i = 0; i < 4096u; i++) {           /* (an access is two PHI cycles at most: ~130 clocks) */
        u32 r = gb_cart_access_read();
        if (!(r >> 31)) { if (rd) *rd = (u8)r; return 1; }
    }
    s_slot.hw = 0; s_slot.in = 0;
    gb_cart_ctrl_write(0);
    return 0;
}
/* one byte on the cartridge's bus, the real one's or the virtual one's */
static int xfer(u32 addr, u32 we, u8 wd, u8 *rd)
{
    if (!s_slot.virt) return slot_xfer(addr, we, wd, rd);
    if (we) n64rt_gbv_write(s_ch, addr, wd);
    else if (rd) *rd = n64rt_gbv_read(s_ch, addr);
    return 1;
}
static void slot_power(int on, int virt)
{
    if (on) s_slot.virt = (u8)virt;
    if (s_slot.virt) { n64rt_gbv_power(s_ch, on); s_slot.bus = (u8)on; return; }
    if (!s_slot.hw) return;
    gb_cart_ctrl_write(on ? 1u | (s_slot.in ? 2u : 0u) : 0u);
    s_slot.bus = (u8)on;
    if (on) wait_us(200);
}
/* is a Game Boy cartridge there? (the port off before and after); the virtual one: is a ROM in its slot */
static void slot_look(int virt)
{
    u8 h[0x50] = { 0 }, was = s_slot.in;
    u32 id = s_slot.id;
    s_slot.looked = now();
    s_slot.in = 0;
    if (virt) {
        s_slot.virt = 1;
        s_slot.in = (u8)n64rt_gbv_look(s_ch);
        if (s_slot.in) s_slot.id = n64rt_gbv_id(s_ch);
        if (s_slot.in != was || (s_slot.in && s_slot.id != id)) s_slot.pulled = 1;
        return;
    }
    s_slot.virt = 0;
    if (!s_slot.hw) return;
    gb_cart_ctrl_write(1);                      /* out of reset; no TRUST: reads of the ROM only */
    wait_us(200);
    int ok = 1;
    for (u32 i = 0; i < sizeof h && ok; i++) ok = slot_xfer(0x100u + i, 0, 0, &h[i]);
    if (ok) {
        u8 x = 0;
        for (u32 i = 0x34; i <= 0x4C; i++) x = (u8)(x - h[i] - 1u);
        if (x == h[0x4D] && crc32(h + 4, 48) == LOGO_CRC) { s_slot.in = 1; s_slot.id = crc32(h, sizeof h); }
        gb_cart_ctrl_write(0);
    }
    if (s_slot.in != was || (s_slot.in && s_slot.id != id)) s_slot.pulled = 1;
#ifdef N64RT_DEBUG
    { static u32 s_n; if (s_n++ < 8u) n64rt_log("n64rt: the cartridge slot: %s (header %02x %02x %02x %02x, type %02x)\n",
                                                 s_slot.in ? "a Game Boy cartridge" : "none", h[0], h[1], h[2], h[3], h[0x47]); }
#endif
}

static void settle(void) { for (volatile int i = 0; i < 256; i++) { } }
/* a slot's file size in the core's table (0: no file); `want` sets it, so the
 * Pocket writes that many bytes back to a nonvolatile slot's file */
u32 n64rt_slot_size(u32 id, u32 want)
{
    u32 prev = apf_bridge_slot_id_read();
    apf_bridge_slot_id_write(id);
    settle();
    u32 size = apf_bridge_file_size_read();
    if (want && size != want) { apf_bridge_file_size_write(want); settle(); }
    apf_bridge_slot_id_write(prev);
    return size;
}

/* the game writes to the Controller Pak: from now on it has a file */
static void cpak_dirty(void)
{
    if (s_sized) return;
    s_sized = 1;
    n64rt_slot_size(CPAK_SLOT, PAK_BYTES);
}

/* ---- the Controller Pak's file system, empty ----------------------------------- */
static u16 be16(const volatile u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static int cpak_id_ok(const volatile u8 *id)
{
    u16 sum = 0;
    for (int i = 0; i < 28; i += 2) sum = (u16)(sum + be16(id + i));
    return be16(id + 28) == sum && be16(id + 30) == (u16)(0xFFF2u - sum) && (id[25] & 1u);
}
static void cpak_format(void)
{
    volatile u8 *m = (volatile u8 *)(uintptr_t)CPAK_SDRAM;
    for (u32 i = 0; i < PAK_BYTES; i++) m[i] = 0;
    /* the ID block, four times: repaired, a number, a serial, device 1, one
     * bank, version 0, the sum of its 14 words and the sum's complement */
    u8 id[32];
    memset(id, 0xFF, 4);
    u32 t = (u32)timer0_uptime_cycles_read() * 2654435761u + 0x4D49524Cu;
    for (int i = 4; i < 24; i++) { t = t * 1664525u + 1013904223u; id[i] = (u8)(t >> 24); }
    id[24] = 0x00; id[25] = 0x01; id[26] = 0x01; id[27] = 0x00;
    u16 sum = 0;
    for (int i = 0; i < 28; i += 2) sum = (u16)(sum + (id[i] << 8 | id[i + 1]));
    id[28] = (u8)(sum >> 8); id[29] = (u8)sum;
    id[30] = (u8)((0xFFF2u - sum) >> 8); id[31] = (u8)(0xFFF2u - sum);
    static const u16 at[4] = { 0x20, 0x60, 0x80, 0xC0 };
    for (int k = 0; k < 4; k++) for (int i = 0; i < 32; i++) m[at[k] + i] = id[i];
    /* the page table and its copy: pages 5 to 127 free (3), a checksum of them */
    for (u32 base = 0x100; base <= 0x200; base += 0x100) {
        u8 sum8 = 0;
        for (u32 pg = 5; pg < 128; pg++) { m[base + 2 * pg + 1] = 3; sum8 = (u8)(sum8 + 3u); }
        m[base + 1] = sum8;
    }
}

void n64rt_pak_init(void)
{
    memset(s_tpv, 0, sizeof s_tpv);
    memset(s_slotv, 0, sizeof s_slotv);
    s_ch = 0;
    s_slot.hw = gb_cart_id_read() == GBC_ID;
    if (s_slot.hw) gb_cart_ctrl_write(0);
    for (int ch = 0; ch < NPAK; ch++) {
        s_ch = ch;
        int k = n64rt_pak_kind(ch);
        if (k == N64RT_PAK_TRANSFER || k == N64RT_PAK_VIRTUAL) { slot_look(k == N64RT_PAK_VIRTUAL); s_slot.pulled = 0; }   /* (any other setting: the port is left alone) */
    }
    s_ch = 0;
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: the cartridge slot's port: %s\n", s_slot.hw ? "there" : "not in this SoC");
#endif
    s_sized = n64rt_slot_size(CPAK_SLOT, 0) >= PAK_BYTES;
    if (!cpak_id_ok((const volatile u8 *)(uintptr_t)(CPAK_SDRAM + 0x20u))) cpak_format();
}

int n64rt_pak_kind(int ch)
{
    if (ch < 0 || ch >= NPAK) return N64RT_PAK_NONE;
#ifdef N64RT_PAK_FORCE                          /* (a simulation, where nothing sets the core's settings) */
    u32 k = ch == 0 ? N64RT_PAK_FORCE : 0u;
#else
    u32 k = (apf_interact_interact0_read() >> (ch == 0 ? 0 : 20)) & 31u;
#endif
    if (ch == 1) return k == 3u ? N64RT_PAK_VIRTUAL : N64RT_PAK_NONE;     /* (controller 2's: only the virtual one) */
    return k == 1u ? N64RT_PAK_CONTROLLER : k == 2u ? N64RT_PAK_TRANSFER : k == 3u ? N64RT_PAK_VIRTUAL : N64RT_PAK_NONE;
}

/* a block of 32 bytes at address a (its low 5 bits clear); 0, or 1 with no pak there */
int n64rt_pak_rw(int ch, u32 a, u8 *b, int write)
{
    int kind = n64rt_pak_kind(ch), virt = kind == N64RT_PAK_VIRTUAL;
    a &= 0xFFE0u;
    if (ch < 0 || ch >= NPAK) return 1;
    s_ch = ch;
    if (kind != N64RT_PAK_TRANSFER && !virt && s_slot.bus) { slot_power(0, 0); s_tp.access = 0; }   /* (the setting changed) */
    if (s_slot.bus && s_slot.virt != (u8)virt) { slot_power(0, 0); s_tp.access = 0; }   /* (the other cartridge) */
    if (kind == N64RT_PAK_NONE) return 1;
    if (kind == N64RT_PAK_CONTROLLER) {
        volatile u8 *m = (volatile u8 *)(uintptr_t)(CPAK_SDRAM + a);
        if (a < PAK_BYTES) {
            if (write) { cpak_dirty(); for (int i = 0; i < 32; i++) m[i] = b[i]; }
            else for (int i = 0; i < 32; i++) b[i] = m[i];
        }
        else if (!write) memset(b, 0, 32);
        return 0;
    }
    u32 hi = a >> 12;
    if (write) {
        if (hi == 0x8u) {
            if (b[0] == 0x84u) s_tp.on = 1;
            else if (b[0] == 0xFEu) { s_tp.on = 0; s_tp.access = 0; slot_power(0, virt); }
        } else if (!s_tp.on) {
        } else if (hi == 0xAu) s_tp.bank = b[0] & 3u;
        else if (hi == 0xBu) {
            u8 acc = b[0] & 1u;
            if (acc && !s_tp.access) { slot_look(virt); slot_power(1, virt); }   /* (the cartridge powers up) */
            if (!acc && s_tp.access) slot_power(0, virt);
            s_tp.access = acc;
            s_tp.changed = 1;
        } else if (hi >= 0xCu && s_tp.access) {
            u32 g = (u32)s_tp.bank << 14 | (a & 0x3FFFu);
            if (s_slot.in && s_slot.bus && g < 0xC000u) for (u32 i = 0; i < 32u && xfer(g + i, 1, b[i], 0); i++) { }
        }
        return 0;
    }
    if (hi == 0xBu && s_tp.on && !s_tp.access && now() - s_slot.looked > CONFIG_CLOCK_FREQUENCY)
        slot_look(virt);                                /* (it may have been put in, or taken out) */
    memset(b, 0, 32);
    if (!s_tp.on) return 0;
    if (hi == 0x8u) memset(b, 0x84, 32);
    else if (hi == 0xBu) {
        /* (libultra: no bit 7 is PFS_ERR_NO_GBCART; osGbpakReadId wants bit 3 after the power-on) */
        memset(b, (s_slot.in ? 0x80 : 0) | (s_tp.access ? (s_slot.in ? 0x09 : 0x01) : 0) | (s_tp.changed ? 0x04 : 0)
                  | (s_slot.pulled ? 0x40 : 0), 32);
        s_tp.changed = 0;
        s_slot.pulled = 0;
    } else if (hi >= 0xCu && s_tp.access && s_slot.in) {
        u32 g = (u32)s_tp.bank << 14 | (a & 0x3FFFu);
        if (s_slot.bus && g < 0xC000u) for (u32 i = 0; i < 32u && xfer(g + i, 0, 0, &b[i]); i++) { }
    }
    return 0;
}

/* the joybus's checksum of a block's 32 bytes */
u8 n64rt_pak_crc(const u8 *d)
{
    u8 c = 0;
    for (int i = 0; i <= 32; i++)
        for (int k = 7; k >= 0; k--) {
            u8 x = (c & 0x80u) ? 0x85u : 0u;
            c = (u8)(c << 1);
            if (i < 32 && ((d[i] >> k) & 1u)) c |= 1u;
            c ^= x;
        }
    return c;
}
