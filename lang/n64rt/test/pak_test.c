/* The accessory (n64rt_pak.c) under libultra's own code: its Controller Pak
 * file system (osPfs*) and its Transfer Pak functions (osGbpak*), as a game
 * would call them. Run in sim/mips_soc with a Game Boy ROM as the cartridge
 * in the Pocket's slot (the SoC built with MIRLO_GB_CART:
 * sim/mips_soc/gb_cart_model.sv, which checks the port's timing):
 *     tb_soc build/pak_test.bin -c 120 +gbcart=<rom.gb>
 * Without +gbcart, or on a SoC without the port, the slot's Transfer Pak
 * must be one with no cartridge in it.
 * The virtual Transfer Pak (n64rt_gbvirt.c) has the same battery, with the ROM
 * and the save as the Pocket's slots 5 and 6:
 *     SLOT5=<rom.gb> [SLOT6=<save>] SLOTS_OUT=<dir> tb_soc build/pak_test.bin -c 150
 * With make_gb.py's ROM it runs the Transfer Pak's checks on it and the RAM it
 * wrote is in <dir>/slot6.bin; with another ROM it reads the whole cartridge,
 * through the game's own osGbpakReadWrite, and prints the CRC-32 of some of its ROM's
 * banks and of each of its RAM's: the files', for the host to compare.
 * The ROM is make_gb.py's: a valid header, MBC3, each bank's last byte its
 * number. */
#include <stdio.h>
#include "PR/os_internal.h"
#include "controller.h"
#include "PR/os_gbpak.h"

extern unsigned pak_test_kind;
void pak_test_boot(void);
int pak_test_slot_port(void);
enum { NONE, CPAK, SLOT, VIRT };            /* the setting's values */

static void fill(void *p, int v, unsigned n) { u8 *b = p; while (n--) *b++ = (u8)v; }
static int fails;
static void check(const char *what, s32 got, s32 want)
{
    if (got == want) return;
    fails++;
    printf("FAIL %s: got %ld want %ld\n", what, (long)got, (long)want);
}

OSMesgQueue pak_test_mq;                   /* (si_test: the SI's event queue) */
static OSPfs s_pfs;
static u8 s_buf[1024], s_back[1024];
static OSGbpakId id_dummy;

static void controller_pak(void)
{
    s32 bytes = 0, file = -1, file2 = -1;
    static u8 name[16] = { 0x26, 0x1A, 0x2B, 0x22, 0x28 }, ext[4] = { 0x1A };
    OSPfsState st;

    pak_test_kind = CPAK;
    check("osPfsInitPak on a new image", osPfsInitPak(&pak_test_mq, &s_pfs, 0), 0);
    check("osPfsFreeBlocks", osPfsFreeBlocks(&s_pfs, &bytes), 0);
    check("free bytes, empty", bytes, 123 * 256);
    check("osPfsAllocateFile", osPfsAllocateFile(&s_pfs, 0x3031, 0x4D49524C, name, ext, sizeof s_buf, &file), 0);
    for (unsigned i = 0; i < sizeof s_buf; i++) s_buf[i] = (u8)(i * 7u + 3u);
    check("osPfsReadWriteFile write", osPfsReadWriteFile(&s_pfs, file, PFS_WRITE, 0, sizeof s_buf, s_buf), 0);
    check("osPfsFreeBlocks", osPfsFreeBlocks(&s_pfs, &bytes), 0);
    check("free bytes, one file", bytes, 123 * 256 - (s32)sizeof s_buf);

    pak_test_boot();                                /* the core again: the image is kept, not formatted */
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osPfsInitPak with a file", osPfsInitPak(&pak_test_mq, &s_pfs, 0), 0);
    check("osPfsFindFile", osPfsFindFile(&s_pfs, 0x3031, 0x4D49524C, name, ext, &file2), 0);
    check("the file's number", file2, file);
    check("osPfsFileState", osPfsFileState(&s_pfs, file2, &st), 0);
    check("the file's size", (s32)st.file_size, (s32)sizeof s_buf);
    check("osPfsReadWriteFile read", osPfsReadWriteFile(&s_pfs, file2, PFS_READ, 0, sizeof s_back, s_back), 0);
    check("the file's bytes", bcmp(s_buf, s_back, sizeof s_buf), 0);
    check("osPfsDeleteFile", osPfsDeleteFile(&s_pfs, 0x3031, 0x4D49524C, name, ext), 0);
    check("osPfsFreeBlocks", osPfsFreeBlocks(&s_pfs, &bytes), 0);
    check("free bytes, empty again", bytes, 123 * 256);
    /* it is not a Transfer Pak */
    check("osGbpakInit on a Controller Pak", osGbpakInit(&pak_test_mq, &s_pfs, 0), PFS_ERR_DEVICE);
}

/* a Transfer Pak with no cartridge in it */
static void transfer_pak_empty(void)
{
    u8 status = 0;
    OSGbpakId id;
    pak_test_kind = SLOT;
    pak_test_boot();
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osGbpakInit, no cartridge", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    check("osGbpakGetStatus, no cartridge", osGbpakGetStatus(&s_pfs, &status), PFS_ERR_NO_GBCART);
    check("osGbpakReadId, no cartridge", osGbpakReadId(&s_pfs, &id, &status), PFS_ERR_NO_GBCART);
    check("osGbpakPower on, no cartridge", osGbpakPower(&s_pfs, OS_GBPAK_POWER_ON), 0);
    check("osGbpakGetStatus, no cartridge, on", osGbpakGetStatus(&s_pfs, &status), PFS_ERR_NO_GBCART);
    check("osGbpakPower off, no cartridge", osGbpakPower(&s_pfs, OS_GBPAK_POWER_OFF), 0);
}

static void transfer_pak(int kind)
{
    u8 status = 0, blk[32], back[32];
    OSGbpakId id;

    pak_test_kind = kind;
    pak_test_boot();
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osPfsInitPak on a Transfer Pak fails", osPfsInitPak(&pak_test_mq, &s_pfs, 0) != 0, 1);
    check("osGbpakInit", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    check("osGbpakGetStatus", osGbpakGetStatus(&s_pfs, &status), 0);
    check("a cartridge is in", status & OS_GBPAK_GBCART_ON, OS_GBPAK_GBCART_ON);
    check("osGbpakReadId", osGbpakReadId(&s_pfs, &id, &status), 0);
    check("the cartridge's reset line", status & OS_GBPAK_RSTB_STATUS, OS_GBPAK_RSTB_STATUS);
    printf("  cartridge \"%.16s\", type %02x\n", (const char *)id.game_title, id.cart_type);
    check("osGbpakCheckConnector", osGbpakCheckConnector(&s_pfs, &status), 0);
    check("no reset since", status & OS_GBPAK_RSTB_DETECTION, 0);
    /* ROM banks: each bank's last byte is its number */
    for (int bank = 1; bank < 8; bank += 3) {
        fill(blk, bank, sizeof blk);
        check("ROM bank write", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x2000, blk, 32), 0);
        check("ROM bank read", osGbpakReadWrite(&s_pfs, OS_READ, 0x7FE0, back, 32), 0);
        check("the bank's last byte", back[31], bank);
    }
    check("bank 0's last byte, read", osGbpakReadWrite(&s_pfs, OS_READ, 0x3FE0, back, 32), 0);
    check("bank 0's last byte", back[31], 0);
    /* the cartridge's RAM: off, on, two banks */
    check("RAM read, off", osGbpakReadWrite(&s_pfs, OS_READ, 0xA000, back, 32), 0);
    fill(blk, 0x0A, sizeof blk);
    check("RAM on", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x0000, blk, 32), 0);
    for (int bank = 0; bank < 2; bank++) {
        fill(blk, bank, sizeof blk);
        check("RAM bank", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x4000, blk, 32), 0);
        for (int i = 0; i < 32; i++) blk[i] = (u8)(0x40 * (bank + 1) + i);
        check("RAM write", osGbpakReadWrite(&s_pfs, OS_WRITE, 0xA100, blk, 32), 0);
    }
    for (int bank = 0; bank < 2; bank++) {
        fill(blk, bank, sizeof blk);
        check("RAM bank", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x4000, blk, 32), 0);
        check("RAM read", osGbpakReadWrite(&s_pfs, OS_READ, 0xA100, back, 32), 0);
        check("RAM bytes", back[0] == (u8)(0x40 * (bank + 1)) && back[31] == (u8)(0x40 * (bank + 1) + 31), 1);
    }
    check("osGbpakPower off", osGbpakPower(&s_pfs, OS_GBPAK_POWER_OFF), 0);
    check("osGbpakGetStatus, off", osGbpakGetStatus(&s_pfs, &status), 0);
    check("power off", status & OS_GBPAK_POWER, 0);

    pak_test_boot();                                /* the core again: the cartridge's RAM is kept */
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osGbpakInit again", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    check("osGbpakReadId again", osGbpakReadId(&s_pfs, &id, &status), 0);
    fill(blk, 0x0A, sizeof blk);
    check("RAM on", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x0000, blk, 32), 0);
    fill(blk, 1, sizeof blk);
    check("RAM bank", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x4000, blk, 32), 0);
    check("RAM read", osGbpakReadWrite(&s_pfs, OS_READ, 0xA100, back, 32), 0);
    check("RAM kept", back[31], 0x80 + 31);

    check("osGbpakPower off", osGbpakPower(&s_pfs, OS_GBPAK_POWER_OFF), 0);
}

static u32 crc_add(u32 c, const u8 *p, u32 n)
{
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c;
}
/* the cartridge as a game reads it: the ROM bank by bank (a bank's number to 0x2000: the MBC1, 3 and 5's), the RAM
 * bank by bank (0x0000: on, 0x4000: the bank); the CRC-32s are the host's to compare with the files' */
static u8 s_bank[0x4000];
static const u8 s_probe[] = { 1, 2, 3, 17, 31, 32, 33, 62, 63 };   /* (a 1 MiB ROM through the joybus takes long: bank 0 and these) */
static void whole_cartridge(int kind)
{
    u8 status = 0, blk[32];
    OSGbpakId id;
    static const u32 ram_bytes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };

    pak_test_kind = kind;
    pak_test_boot();
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osGbpakInit", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    check("osGbpakReadId", osGbpakReadId(&s_pfs, &id, &status), 0);
    u32 banks = 2u << id.rom_size, ram = id.ram_size < 6 ? ram_bytes[id.ram_size] : 0;
    printf("  cartridge \"%.16s\", type %02x, %lu ROM banks, %lu bytes of RAM\n", (const char *)id.game_title, id.cart_type,
           (unsigned long)banks, (unsigned long)ram);
    check("bank 0", osGbpakReadWrite(&s_pfs, OS_READ, 0x0000, s_bank, 0x4000), 0);
    printf("  ROM bank 0 crc %08lx\n", (unsigned long)~crc_add(0xFFFFFFFFu, s_bank, 0x4000));
    for (u32 i = 0; i < sizeof s_probe; i++) {
        u32 b = s_probe[i];
        if (b >= banks) continue;
        fill(blk, (int)(b & 0xFF), sizeof blk);
        check("ROM bank write", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x2000, blk, 32), 0);
        check("ROM bank read", osGbpakReadWrite(&s_pfs, OS_READ, 0x4000, s_bank, 0x4000), 0);
        printf("  ROM bank %lu crc %08lx\n", (unsigned long)b, (unsigned long)~crc_add(0xFFFFFFFFu, s_bank, 0x4000));
    }
    if (ram) {
        fill(blk, 0x0A, sizeof blk);
        check("RAM on", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x0000, blk, 32), 0);
        u32 rb = ram >= 0x2000u ? ram >> 13 : 1u, part = ram >= 0x2000u ? 0x2000u : ram;
        for (u32 b = 0; b < rb; b++) {
            fill(blk, (int)b, sizeof blk);
            check("RAM bank", osGbpakReadWrite(&s_pfs, OS_WRITE, 0x4000, blk, 32), 0);
            check("RAM read", osGbpakReadWrite(&s_pfs, OS_READ, 0xA000, s_bank, part), 0);
            printf("  RAM bank %lu crc %08lx\n", (unsigned long)b, (unsigned long)~crc_add(0xFFFFFFFFu, s_bank, part));
        }
    }
    check("osGbpakPower off", osGbpakPower(&s_pfs, OS_GBPAK_POWER_OFF), 0);
}

static void no_pak(void)
{
    pak_test_kind = NONE;
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osPfsInitPak, nothing", osPfsInitPak(&pak_test_mq, &s_pfs, 0), PFS_ERR_NOPACK);
    check("osGbpakInit, nothing", osGbpakInit(&pak_test_mq, &s_pfs, 0), PFS_ERR_NOPACK);
    pak_test_kind = CPAK;                           /* another controller's has none */
    check("osPfsInitPak, controller 2", osPfsInitPak(&pak_test_mq, &s_pfs, 1), PFS_ERR_NOPACK);
}

int main(void)
{
    u8 status = 0;
    printf("pak_test\n");
    pak_test_kind = CPAK;
    pak_test_boot();
    controller_pak();
    /* the cartridge in the Pocket's slot: there (the battery through the
     * port), or the Transfer Pak with none in it */
    pak_test_kind = SLOT;
    pak_test_boot();
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osGbpakInit, the slot", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    if (osGbpakGetStatus(&s_pfs, &status) == 0) {
        printf("  the slot: a cartridge (the port %s)\n", pak_test_slot_port() ? "is there" : "IS NOT THERE");
        transfer_pak(SLOT);
    } else {
        printf("  the slot: no cartridge (the port %s)\n", pak_test_slot_port() ? "is there" : "is not in this SoC");
        transfer_pak_empty();
    }
    /* the virtual Transfer Pak: its cartridge is made of the Pocket's slots 5 and 6 */
    pak_test_kind = VIRT;
    pak_test_boot();
    fill(&s_pfs, 0, sizeof s_pfs);
    check("osGbpakInit, virtual", osGbpakInit(&pak_test_mq, &s_pfs, 0), 0);
    if (osGbpakGetStatus(&s_pfs, &status) == 0) {
        OSGbpakId id;
        check("osGbpakReadId, virtual", osGbpakReadId(&s_pfs, &id, &status), 0);
        const int mine = bcmp(id.game_title, "MIRLO TEST", 10) == 0;
        printf("  the virtual slot: a cartridge%s\n", mine ? " (make_gb.py's)" : "");
        if (mine) transfer_pak(VIRT); else whole_cartridge(VIRT);
    } else {
        printf("  the virtual slot: no cartridge\n");
        pak_test_boot();
        check("osGbpakReadId, no virtual cartridge", osGbpakReadId(&s_pfs, &id_dummy, &status), PFS_ERR_NO_GBCART);
    }
    no_pak();
    printf(fails ? "pak_test: FAILED\n" : "pak_test: PASS\n");
    return fails;
}
