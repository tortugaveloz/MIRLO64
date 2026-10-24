/* Loading the ROM and booting it the way the N64's IPL3 leaves the machine:
 * the first MiB of the game (ROM 0x1000..) at its entry point, the boot
 * variables at 0x80000300 that libultra's osInitialize reads, and the CPU
 * at the entry point. This is also what Mirlo's BIOS will do before handing
 * the CPU to the game.
 */
#include <stdlib.h>
#include <string.h>

#include <zlib.h>
#include "n64.h"

/* The cartridge's CIC, from its IPL3's CRC32 (ROM 0x40..0xFFF): where IPL3
 * puts the game (the header's entry point, less 1 MiB on a 6103, 2 on a
 * 6106) and the seed it leaves in s6. A ROM patched by tools/n64patch.py
 * has the adjusted entry point in its header already (the trailer says so):
 * Mirlo's boot ROM jumps to the header's. */
static void cic_of(const n64_t *n, uint32_t *entry_sub, uint32_t *seed)
{
    uint8_t b[0xFC0];
    for (uint32_t i = 0; i < sizeof b; i++) b[i] = rd8(n->mem.rom, 0x40 + i);
    uint32_t c = (uint32_t)crc32(0, b, sizeof b);
    *entry_sub = 0; *seed = 0x3F;                              /* 6101, 6102 */
    if (c == 0x0B050EE0u) { *entry_sub = 0x100000u; *seed = 0x78; }     /* 6103 */
    else if (c == 0x98BC2C86u) *seed = 0x91;                            /* 6105 */
    else if (c == 0xACC8580Au) { *entry_sub = 0x200000u; *seed = 0x85; } /* 6106 */
}

int n64_load_rom(n64_t *n, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0x101000 || size > (long)ROM_MAX) { fclose(f); return -1; }
    uint8_t *b = malloc((size_t)size);
    if (fread(b, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(b); return -1; }
    fclose(f);
    /* to big-endian (.z64) byte order: .v64 swaps halfwords' bytes, .n64 is little-endian */
    uint32_t magic = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    for (long i = 0; i + 3 < size; i += 4) {
        uint8_t t;
        if (magic == 0x37804012u) { t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; t = b[i + 2]; b[i + 2] = b[i + 3]; b[i + 3] = t; }
        else if (magic == 0x40123780u) { t = b[i]; b[i] = b[i + 3]; b[i + 3] = t; t = b[i + 1]; b[i + 1] = b[i + 2]; b[i + 2] = t; }
    }
    /* to word-native (n64.h) */
    n->mem.rom = malloc((size_t)size);
    for (long i = 0; i + 3 < size; i += 4)
        wr32(n->mem.rom, (uint32_t)i, (uint32_t)b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3]);
    n->mem.rom_size = (uint32_t)size;
    free(b);
    return 0;
}

/* the hardware's reset state: what Mirlo's RCP block and CPU come up with */
void n64_reset(n64_t *n)
{
    n64_cpu_reset(n);
    memset(&n->rcp, 0, sizeof n->rcp);
    n->rcp.sp_status = 1;                           /* halted */
    n->rcp.ri[3] = 0x14;                            /* RI_SELECT, as the IPL leaves it */
    n->rcp.ri[0] = 0x0E;                            /* RI_MODE */
    n->rcp.ri[1] = 0x40;                            /* RI_CONFIG */
    n->cpu.pc = 0xBFC00000u;                        /* the boot ROM */
    n->cpu.next_pc = n->cpu.pc + 4;
}

/* Mirlo's boot ROM (lang/mips/boot), big-endian bytes -> word-native */
int n64_load_bootrom(n64_t *n, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t buf[0x7C0];
    size_t k = fread(buf, 1, sizeof buf, f);
    fclose(f);
    n->mem.bootrom = calloc(0x7C0, 1);
    n->mem.bootrom_size = (uint32_t)((k + 3) & ~3u);
    for (size_t i = 0; i < k; i++) wr8(n->mem.bootrom, (uint32_t)i, buf[i]);
    return 0;
}

void n64_boot(n64_t *n)
{
    n64_reset(n);

    /* the header and IPL3 in DMEM, as the PIF loads them */
    for (uint32_t i = 0; i < 0x1000; i += 4) wr32(n->mem.spmem, i, rd32(n->mem.rom, i));

    /* the game's first MiB at its entry point */
    uint32_t entry = rd32(n->mem.rom, 8), entry_sub, seed;
    cic_of(n, &entry_sub, &seed);
    int patched = n->mem.rom_size >= 32 && rd32(n->mem.rom, n->mem.rom_size - 32) == 0x4D49524Cu;   /* 'MIRL' */
    if (!patched) entry -= entry_sub;
    uint32_t dst = entry & 0x1FFFFFFFu;
    for (uint32_t i = 0; i < 0x100000; i += 4) wr32(n->mem.rdram, dst + i, rd32(n->mem.rom, 0x1000 + i));

    /* a ROM patched for Mirlo-N64 (tools/n64patch.py): its trailer, the last
     * 32 bytes -- 'MIRLON64' 1 blob_rom blob_ram blob_len crc1 crc2 -- names
     * our code, copied where it runs (Mirlo's boot ROM does the same) */
    uint32_t t = n->mem.rom_size - 32;
    if (n->mem.rom_size >= 64 && rd32(n->mem.rom, t) == 0x4D49524Cu && rd32(n->mem.rom, t + 4) == 0x4F4E3634u) {
        uint32_t from = rd32(n->mem.rom, t + 12), to = rd32(n->mem.rom, t + 16) & 0x1FFFFFFFu, len = rd32(n->mem.rom, t + 20);
        for (uint32_t i = 0; i < len; i += 4) wr32(n->mem.rdram, to + i, rd32(n->mem.rom, from + i));
        if (n->log) fprintf(n->log, "boot: a Mirlo-N64 ROM: %u bytes of our code at %08x\n", len, to | 0x80000000u);
    }

    /* boot variables (libultra's osTvType etc.) */
    wr32(n->mem.rdram, 0x300, 1);                   /* osTvType: NTSC */
    wr32(n->mem.rdram, 0x304, 0);                   /* osRomType: cartridge */
    wr32(n->mem.rdram, 0x308, 0xB0000000u);         /* osRomBase */
    wr32(n->mem.rdram, 0x30C, 0);                   /* osResetType: cold */
    wr32(n->mem.rdram, 0x310, 0);                   /* osCicId */
    wr32(n->mem.rdram, 0x314, 0);                   /* osVersion */
    wr32(n->mem.rdram, 0x318, 0x00400000u);         /* osMemSize: 4 MiB, no Expansion Pak */

    /* the PI's cartridge timings from the ROM header's first word */
    uint32_t h = rd32(n->mem.rom, 0);
    n->rcp.pi_dom[0] = h & 0xFF;                    /* latency */
    n->rcp.pi_dom[1] = (h >> 8) & 0xFF;             /* pulse width */
    n->rcp.pi_dom[2] = (h >> 16) & 0x0F;            /* page size */
    n->rcp.pi_dom[3] = (h >> 20) & 0x03;            /* release */

    /* the CPU as IPL3 hands it over */
    n64_cpu_t *c = &n->cpu;
    c->r[19] = 0;                                   /* s3: rom type */
    c->r[20] = 1;                                   /* s4: tv type */
    c->r[21] = 0;                                   /* s5: reset type */
    c->r[22] = seed;                                /* s6: the CIC's seed */
    c->r[23] = 0;                                   /* s7: version */
    c->r[29] = 0xA4001FF0u;                         /* sp */
    c->r[31] = 0xA4001550u;
    c->r[11] = 0xA4000040u;                         /* t3 */
    c->pc = entry;
    c->next_pc = entry + 4;
}
