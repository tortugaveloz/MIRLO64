/* restart_test: a game for the core's "Restart Game" action -- a program linked for 0x8000_0400 that runs as a game under the loader
 * (OWN) on the SoC in sim/mips_soc. It does what a game that looks at its Transfer Pak once, a moment after it boots, does: it waits
 * a fixed time, then asks controller 1's pak (through the PIF: the SI's DMA of a block of commands, as libultra's osGbpak* do) for its
 * power (0x8000 written 0x84) and for its status (0xB000 read: bit 7 says a cartridge is in), and never reads the status again.
 *
 * With a cartridge in, it counts its starts in the cartridge's RAM (byte 0: read, written back plus one, read again) and leaves the pak
 * powered, with that write not yet written to the Game Boy game's save file: a restart writes it. Then it leaves the hardware busy, as a
 * game in the middle of its frame has it, and spins: the RSP runs a microcode that never ends, the AI plays 1.5 s of silence and the RDP
 * draws a list of 0.25 s again and again.
 *
 * The report is in the RDRAM at 0x8030_0000 (DUMP=0x43B00000,64 in the sim): word 0 the magic 0x600D600D when the status was read,
 * word 1 the status byte, word 2 Count when it was read, word 3 the number of loops of the wait before it, word 4 0xD0DE when done,
 * word 5 a word the game writes at its start (word 6 is 1 when it finds it there: the RDRAM was not cleared), words 8 and 9 the
 * cartridge RAM's byte before and after the game's write, word 10 the header's destination code and word 11 the sum of a save's bytes as a game
 * that checks one reads them (0xFF for a save with a right sum). The runtime's own log (a build with GBTRACE=1 or DEBUG=1) carries the status
 * the game reads, the first time and whenever it changes.
 *
 *   make -f restart_test.mk; sim/mips_soc/restart_check.sh pak (the scenario) */
typedef unsigned int u32;
typedef unsigned char u8;
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define REPORT ((volatile u32 *)0x80300000u)
#define SI_DRAM_ADDR 0x04800000u
#define SI_RD64B     0x04800004u
#define SI_WR64B     0x04800010u
#define SI_STATUS    0x04800018u
#define PIF_RAM      0x1FC007C0u
static u32 blk[16] __attribute__((aligned(16)));          /* (in the RDRAM; the SI reads and writes it by its physical address) */
static u32 count(void) { u32 c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
static volatile u8 *block(void) { return (volatile u8 *)(0xA0000000u | ((u32)blk & 0x1FFFFFFFu)); }
static u32 phys(void) { return (u32)blk & 0x1FFFFFFFu; }
static void wait_si(void) { while (*K1(SI_STATUS) & 3u) ; }
/* the commands in blk, 64 bytes with the run flag; the PIF runs them and the replies are read back into blk */
static void pif(void)
{
    *K1(SI_DRAM_ADDR) = phys();
    *K1(SI_WR64B) = PIF_RAM;
    wait_si();
    *K1(SI_DRAM_ADDR) = phys();
    *K1(SI_RD64B) = PIF_RAM;
    wait_si();
}
static void clear_block(void) { volatile u8 *b = block(); for (u32 i = 0; i < 64u; i++) b[i] = 0; b[63] = 1; }
/* the pak's 32-byte block at the byte address `a` of controller 1, written (data = the byte repeated) or read; the first byte of the reply */
static u32 pak(u32 a, int write, u8 fill)
{
    volatile u8 *b = block();
    clear_block();
    b[0] = write ? 0x23u : 0x03u; b[1] = write ? 0x01u : 0x21u; b[2] = write ? 0x03u : 0x02u;
    b[3] = (u8)(a >> 8); b[4] = (u8)a;
    if (write) { for (u32 i = 0; i < 32u; i++) b[5 + i] = fill; b[38] = 0xFEu; }
    else b[38] = 0xFEu;
    pif();
    return b[5];
}
/* The hardware left busy, as a game in the middle of its frame has it: the RSP running a microcode that never ends (one `j 0`), and the AI with
 * a buffer of 1.5 s of silence to play. A restart has to stop the first and play out the second. */
#define SP_MEM_ADDR  0x04040000u
#define SP_DRAM_ADDR 0x04040004u
#define SP_RD_LEN    0x04040008u
#define SP_STATUS_   0x04040010u
#define SP_DMA_BUSY  0x04040018u
#define SP_PC        0x04080000u
#define AI_DRAM_ADDR 0x04500000u
#define AI_LEN       0x04500004u
#define AI_CONTROL   0x04500008u
#define AI_DACRATE   0x04500010u
#define DPC_START    0x04100000u
#define DPC_END      0x04100004u
#define DPC_CURRENT  0x04100008u
static u32 list[2 * 320] __attribute__((aligned(16)));
static u32 list_n;
static u32 prog[4] __attribute__((aligned(16)));
static u32 abuf[0x10000 / 4] __attribute__((aligned(16)));
static void busy(void)
{
    volatile u32 *p = (volatile u32 *)(0xA0000000u | ((u32)prog & 0x1FFFFFFFu));
    p[0] = 0x08000000u; p[1] = 0;                       /* j 0; nop */
    *K1(SP_STATUS_) = 0x2u;
    *K1(SP_MEM_ADDR) = 0x1000u; *K1(SP_DRAM_ADDR) = (u32)prog & 0x1FFFFFFFu; *K1(SP_RD_LEN) = 7u;
    while (*K1(SP_DMA_BUSY)) ;
    *K1(SP_PC) = 0;
    *K1(SP_STATUS_) = 0x1u;                             /* the RSP runs */
    *K1(AI_DRAM_ADDR) = (u32)abuf & 0x1FFFFFFFu;
    *K1(AI_DACRATE) = 4414u;                            /* 11 kHz: 64 KiB are 1.5 s */
    *K1(AI_CONTROL) = 1u;
    *K1(AI_LEN) = 0x10000u;
    /* the RDP's list: a 16-bit colour image of 320 x 240 at 5 MiB, fill mode, one fill of all of it and a SYNC FULL (some 0.25 s of drawing) */
    volatile u32 *l = (volatile u32 *)(0xA0000000u | ((u32)list & 0x1FFFFFFFu));
    u32 n = 0;
    l[n++] = 0xFF10013Fu; l[n++] = 0x00500000u;        /* SET_COLOR_IMAGE: RGBA 16 bit, 320 wide */
    l[n++] = 0xED000000u; l[n++] = 0x005003C0u;        /* SET_SCISSOR 0,0 - 320,240 */
    l[n++] = 0xEF300000u; l[n++] = 0u;                  /* SET_OTHER_MODES: fill cycle */
    l[n++] = 0xF7000000u; l[n++] = 0xF800F800u;         /* SET_FILL_COLOR */
    for (u32 i = 0; i < 1u; i++) { l[n++] = 0xF64FC3BCu; l[n++] = 0u; }   /* FILL_RECTANGLE 0,0 - 319,239 */
    l[n++] = 0xE9000000u; l[n++] = 0u;                  /* SYNC_FULL */
    list_n = n * 4u;
}
/* the RDP is given its list again whenever it has done the last one: it is drawing nearly all the time */
static void rdp_kick(void)
{
    if (*K1(DPC_CURRENT) < *K1(DPC_END)) return;
    *K1(DPC_START) = (u32)list & 0x1FFFFFFFu;
    *K1(DPC_END) = ((u32)list & 0x1FFFFFFFu) + list_n;
}
/* a block read into `out`, a block written */
static void pak_block(u32 a, u8 *out)
{
    pak(a, 0, 0);
    volatile u8 *b = block();
    for (u32 i = 0; i < 32u; i++) out[i] = b[5 + i];
}
static void pak_put(u32 a, const u8 *data)
{
    volatile u8 *b = block();
    clear_block();
    b[0] = 0x23u; b[1] = 0x01u; b[2] = 0x03u; b[3] = (u8)(a >> 8); b[4] = (u8)a;
    for (u32 i = 0; i < 32u; i++) b[5 + i] = data[i];
    b[38] = 0xFEu;
    pif();
}
/* What a game that checks a Game Boy save's sum does, with the cartridge's own bytes: the header's destination code (byte 0x4A of the block
 * at cartridge address 0x140), the MBC's banking mode (1s to 0x6000), the RAM on as a game writes it (a block of zeros whose last byte is
 * 0x0A, to 0x0000), RAM bank 1 (1s to 0x4000), the 0xFC0 bytes at RAM 0x2580 (cartridge 0xA580) and their sum from RAM 0x2598 to 0x3523.
 * The pak's bank (its register at 0xA000) is the cartridge address's bits 15:14, the block's address the low 14 bits plus 0xC000. */
static u32 save_check(u32 *dest)
{
    u8 blk[32], on[32], ones[32];
    static u8 ram[0xFC0];
    for (u32 i = 0; i < 32u; i++) { ones[i] = 1; on[i] = 0; }
    on[31] = 0x0Au;
    pak(0xA000u, 1, 0u);
    pak_block(0xC140u, blk);
    *dest = blk[0x0A];
    pak(0xA000u, 1, 1u);
    pak_put(0xC000u + 0x2000u, ones);                   /* 0x6000 */
    pak(0xA000u, 1, 0u);
    pak_put(0xC000u, on);                               /* 0x0000 */
    pak(0xA000u, 1, 1u);
    pak_put(0xC000u, ones);                             /* 0x4000 */
    pak(0xA000u, 1, 2u);
    for (u32 o = 0; o < 0xFC0u; o += 32u) pak_block(0xC000u + 0x2580u + o, ram + o);
    u32 sum = 0;
    for (u32 i = 0x18; i <= 0xFA3u; i++) sum += ram[i];
    return sum & 0xFFu;
}
void game_main(void)
{
    REPORT[6] = REPORT[5] != 0;                         /* (a game started over finds the RDRAM as the boot left it: 0) */
    REPORT[5] = 0xA5A5A5A5u;
    const u32 t0 = count();
    u32 loops = 0;
    while (count() - t0 < 4000000u) loops++;            /* some 0.09 s of Count (46.875 MHz) */
    pak(0x8000u, 1, 0x84u);                             /* the pak's power on */
    const u32 st = pak(0xB000u, 0, 0);                  /* its status */
    REPORT[1] = st; REPORT[2] = count(); REPORT[3] = loops;
    if (st & 0x80u) {                                   /* a cartridge: its RAM's first byte counts the starts (an MBC with RAM) */
        pak(0xB000u, 1, 0x01u);                         /* its bus on */
        pak(0xC000u, 1, 0x0Au);                         /* cartridge address 0x0000: the RAM enabled */
        pak(0xA000u, 1, 0x02u);                         /* the pak's bank 2: the cartridge's 0x8000 - 0xBFFF */
        const u32 old = pak(0xE000u, 0, 0);             /* cartridge address 0xA000: the RAM's byte 0 */
        pak(0xE000u, 1, (u8)(old + 1u));
        REPORT[8] = old; REPORT[9] = pak(0xE000u, 0, 0);
        u32 dest = 0;
        REPORT[11] = save_check(&dest); REPORT[10] = dest;
    }
    REPORT[0] = 0x600D600Du;
    REPORT[4] = 0xD0DEu;
    busy();
    for (;;) { REPORT[7] = count(); rdp_kick(); }
}
__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
