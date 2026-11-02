/* The runtime's program, n64rt.sys (data slot 3 of the core): it runs the N64 game in slot 0, a .z64 ROM as it is. The Pocket
 * loads this file at 0 and the ROM at 1 MiB (N64RT_ROM_FILE_OFF: its first 30 MiB go to PSRAM), untouched.
 *
 * The game runs its own libultra (N64RT_OWN), so nothing but the ROM is needed, and nothing of any game is in this program or
 * in the runtime: no table, no row, no patch, no address. What IPL3 knows comes from the ROM image -- the CIC from the boot
 * code's checksum, the entry from the header (as IPL3 reads it) --, the console is the same for every game (an N64 with the
 * Expansion Pak: 8 MiB, which a game reads from osMemSize and uses as it finds), the core takes the game's TLB instructions
 * and its TLB exceptions itself, and the save's kind is what the game does (n64rt_io.c). Build: lang/n64rt/Makefile. */
#include <stdio.h>
#include <system.h>
#include <generated/csr.h>
#include "n64rt.h"

#define ROM ((const volatile u32 *)(uintptr_t)N64RT_ROM_WIN)      /* (the file at 1 MiB, through the core's window on the ROM) */
#ifndef GAME_MEM
#define GAME_MEM 0x800000u              /* (the console has the Expansion Pak: a valid machine for every game) */
#endif

/* nothing to run: a plain screen says why to a Pocket with no cable on it
 * (red: no .z64 there; yellow: a game this build does not know) */
static void stop(const char *why, u16 colour) __attribute__((noreturn));
static void stop(const char *why, u16 colour)
{
    apf_bridge_savestate_write(0);              /* (no game: no save state, n64rt_state.c) */
    printf("loader: %s\n", why);
    volatile u16 *fb = (volatile u16 *)(uintptr_t)0x40C00000u;
    for (u32 i = 0; i < 320u * 240u; i++) fb[i] = colour;
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_width_write(0);
    video_framebuffer_dma_base_write(0x40C00000u);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    for (;;) ;
}


/* ---- what IPL3 knows of a ROM: its CIC and where it copies the boot segment to ---------------------------
 * The CIC by the CRC-32 of the boot code (ROM 0x40..0xFFF) -- the community's table of them --, and the entry
 * address, which the header holds as the CIC's IPL3 left it: the 6103's and the 6106's IPL3 takes 0x100000 and
 * 0x200000 from it. */
static u32 rom_cic(void)
{
    u32 crc = 0xFFFFFFFFu;
    for (u32 o = 0x40u; o < 0x1000u; o++) {
        crc ^= (u8)(ROM[o >> 2] >> (24 - 8 * (o & 3u)));
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    switch (~crc) {
    case 0x6170A4A1u: return 6101;
    case 0x90BB6CB5u: return 6102;
    case 0x0B050EE0u: return 6103;
    case 0x98BC2C86u: return 6105;
    case 0xACC8580Au: return 6106;
    case 0x009E9EA3u: return 6102;      /* (7102, the PAL 6102) */
    default: return 6102;               /* (a ROM with another boot code: the commonest) */
    }
}
static u32 rom_entry(u32 cic)
{
    u32 e = ROM[2];
    if (cic == 6103) e -= 0x100000u;
    if (cic == 6106) e -= 0x200000u;
    return e;
}

/* ---- an open-source IPL3 ------------------------------------------------------------------------------
 * A ROM whose boot code is not the console's kind -- one that is a program's own, the open-source IPL3 that initialises
 * the RDRAM itself -- has a banner in it, which the ROM's checksum does not need to be right for. Such a boot code does not
 * copy 1 MiB to the header's entry: the ELF build of it finds an ELF file in the ROM (at a 256-byte boundary from ROM
 * 0x1000 on) and loads its segments, where the program is of any size; the flat build copies from ROM 0x1000 to the
 * header's entry as many bytes as the header's word at 0x10 says (0, or more than the RDRAM has: 1 MiB). */
static u32 rom_byte(u32 o) { return (ROM[o >> 2] >> (24 - 8 * (o & 3u))) & 0xFFu; }
static int rom_has_banner(void)
{
    static const char banner[32] = " Libdragon IPL3 " " Coded by Rasky ";
    for (u32 o = 0x40u; o + 32u <= 0x2000u; o += 4u) {
        u32 i = 0;
        while (i < 32u && rom_byte(o + i) == (u8)banner[i]) i++;
        if (i == 32u) return 1;
    }
    return 0;
}
/* the ROM offset of the ELF file, or 0 (searched in the whole file; the boot code searches 64 MiB) */
static u32 rom_elf(u32 size)
{
    for (u32 o = 0x1000u; o + 0x40u <= size; o += 0x100u)
        if (ROM[o >> 2] == 0x7F454C46u) return o;
    return 0;
}

int main(void)
{
    if (!n64rt_restarted) n64rt_restart_snapshot();        /* (the .data a "Restart Game" puts back: n64rt_restart.c) */
    apf_bridge_slot_id_write(0);
    for (volatile int i = 0; i < 2000; i++) ;       /* (the slot's size: read in the bridge's clock) */
    u32 size = apf_bridge_file_size_read();
    printf("\nMirlo's N64 loader: a file of %lu bytes in slot 0\n", (unsigned long)size);
    if (n64rt_restarted) n64rt_restart_booted();
    /* a small file that is no ROM: a program linked to run where it is (a
     * test of the core's: lang/mips/test, its memory from 1 MiB) -- copied
     * there, to the SDRAM, from the PSRAM the Pocket wrote it to */
    while ((monitor_id_read() & 0x180u) != 0x180u) ;          /* (the PSRAM set up: the window answers) */
    if (size >= 64u && size < 0x100000u && ROM[0] != 0x80371240u) {
        printf("loader: a program of %lu bytes at 1 MiB\n", (unsigned long)size);
        apf_bridge_savestate_write(0);
        volatile u32 *to = (volatile u32 *)(uintptr_t)(0x40000000u + N64RT_ROM_FILE_OFF);
        for (u32 i = 0; i < (size + 3u) / 4u; i++) to[i] = ROM[i];
        flush_cpu_dcache(); flush_cpu_icache();
        ((void (*)(void))(uintptr_t)(0x40000000u + N64RT_ROM_FILE_OFF))();
    }
    const int ipl3_own = size >= 0x2000u && ROM[0] == 0x80371240u && rom_has_banner();     /* (a ROM of any size, then) */
    if ((size < 0x101000u && !ipl3_own) || ROM[0] != 0x80371240u)
        stop(ROM[0] == 0x37804012u || ROM[0] == 0x40123780u ? "the ROM's bytes are swapped: a .z64 is needed" : "no .z64 ROM", 0xF801u);
    const u32 cic = rom_cic(), entry = rom_entry(cic);
    static char name[21];
    for (int i = 0; i < 20; i++) name[i] = (char)(ROM[8 + i / 4] >> (24 - 8 * (i & 3)));     /* (the header's title, at 0x20) */
    name[20] = 0;
    for (int i = 19; i >= 0 && (name[i] == ' ' || name[i] == 0); i--) name[i] = 0;
    printf("%s on Mirlo (n64rt): CIC %lu, entry %08lx, check words %08lx %08lx (from the header)\n", name, (unsigned long)cic,
           (unsigned long)entry, (unsigned long)ROM[4], (unsigned long)ROM[5]);
    static n64rt_game_t g;
    g.name = name;
    g.boot_rom = 0x1000u;                /* IPL3 copies 1 MiB to the entry (the header's, by the CIC) */
    g.boot_vram = entry;
    g.boot_len = 0x100000u;
    g.entry = entry;
    if (ipl3_own) {
        const u32 elf = rom_elf(size);
        if (elf) {                       /* the program is an ELF file: its segments and its entry (n64rt_boot.c) */
            g.ipl3 = N64RT_IPL3_ELF;
            g.elf_off = elf;
            g.entry = ROM[(elf >> 2) + 1u] >> 24 == 2u ? ROM[(elf >> 2) + 7u] : ROM[(elf >> 2) + 6u];    /* (ELF64: the low word) */
            printf("loader: an open-source IPL3: the ELF file at ROM %08lx, entry %08lx\n", (unsigned long)elf, (unsigned long)g.entry);
        } else {                         /* a flat file: the header's word says how much */
            u32 len = ROM[4];
            if (len == 0 || len > 0x800000u - (entry & 0x1FFFFFFFu) - 0x8000u) len = 0x100000u;
            if (len > size - 0x1000u) len = size - 0x1000u;               /* (what the ROM has) */
            g.ipl3 = N64RT_IPL3_FLAT;
            g.boot_len = len & ~3u;
            printf("loader: an open-source IPL3, a flat file: %lu bytes from ROM 1000 to %08lx\n", (unsigned long)g.boot_len, (unsigned long)entry);
        }
    }
    g.mem_size = GAME_MEM;
    /* The core's "Expansion Pak" setting (interact.json: field 14, INTERACT1's bit 24; 0 with the Pak in, which is how the
     * core starts): without it the console has 4 MiB */
    if ((apf_interact_interact1_read() >> 24) & 1u) g.mem_size = 0x400000u;
    g.cic = cic;
    n64rt_boot_rom(&g, size);
}
