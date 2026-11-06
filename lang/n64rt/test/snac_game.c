/* snac_game: a game that talks to the PIF the way libultra does -- the SI's DMA of a 64-byte block of joybus commands with the run flag, then the
 * block read back with the replies -- and checks what the Analogizer's SNAC port and the virtual channels answer (n64rt_io.c, pif_run;
 * n64rt_snac.c). A program linked for 0x8000_0400 that runs under the loader (OWN) on the SoC in sim/mips_soc, with the Joybus device models of
 * sim/analogizer/snac/jb_dev.sv on the four wires (sim/mips_soc/snac_check.sh gives them the +snac<n> options and the menu's value for the run, and
 * builds this with -DSNAC_RUN= to match). The menu is the core's "Analogizer" (0 Off, 1..9 a video mode, 10 N64 SNAC): anything but Off turns the
 * Joybus on.
 *
 *   A  the menu is Off, devices on the wires: controller 1 is the Pocket's pad with a virtual Controller Pak, the others do not answer
 *   B  the menu is N64 SNAC, devices on ports 1 (pad + pak), 2 (pad), 4 (pad + pak); none on port 3: the channels with a device are the real ones
 *   C  the menu is RGBS (a video mode), a device on port 2 only: controller 1 stays the Pocket's pad and the virtual Controller Pak, controller 2 is real
 *   D  as B, with a Game Boy cartridge in the slot: the pins are the cartridge's whatever the menu says, no device is seen: as A
 *   E  the menu is Off at the start (as A) and is changed to N64 SNAC while the game runs: the game polls until controller 2 answers, then as B
 * In every run the cartridge's EEPROM (channel 4) is the runtime's.
 *
 * The report, in the RDRAM at 0x8030_0000 (DUMP=0x43B00000,64 in the sim): word 0 the magic 0x600D600D when done, 1 the number of failed checks, 2 the
 * checks made, 3 the Count ticks (46.875 MHz) from the start of the SI's write of a block of four buttons commands to the end of its read
 * back, 4 the same for a block of four status commands, 5 the same for the write of a pak block (or, with no real pak, 0), 7 (E) the polls it took
 * for controller 2 to answer, 8..15 the first failed checks' numbers, 16..31 the status replies (bytes of four channels, 3 each), 32..47
 * the buttons replies (4 each). */
typedef unsigned int u32;
typedef unsigned char u8;
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define REPORT ((volatile u32 *)0x80300000u)
#define SI_DRAM_ADDR 0x04800000u
#define SI_RD64B     0x04800004u
#define SI_WR64B     0x04800010u
#define SI_STATUS    0x04800018u
#define PIF_RAM      0x1FC007C0u
static u32 blk[16] __attribute__((aligned(16)));
static u32 count(void) { u32 c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
static volatile u8 *block(void) { return (volatile u8 *)(0xA0000000u | ((u32)blk & 0x1FFFFFFFu)); }
static u32 phys(void) { return (u32)blk & 0x1FFFFFFFu; }
static void wait_si(void) { while (*K1(SI_STATUS) & 3u) ; }
static u32 s_read_ticks, s_write_ticks;
/* the block in blk (64 bytes with the run flag) to the PIF and the replies back */
static void pif(void)
{
    *K1(SI_DRAM_ADDR) = phys();
    u32 t0 = count();
    *K1(SI_WR64B) = PIF_RAM;
    wait_si();
    s_write_ticks = count() - t0;
    *K1(SI_DRAM_ADDR) = phys();
    t0 = count();
    *K1(SI_RD64B) = PIF_RAM;
    wait_si();
    s_read_ticks = count() - t0;
}
static void clear_block(void) { volatile u8 *b = block(); for (u32 i = 0; i < 64u; i++) b[i] = 0; b[63] = 1; }

static u32 fails, checks;
static void check(u32 id, int ok)
{
    checks++;
    if (ok) return;
    if (fails < 8u) REPORT[8 + fails] = id;
    fails++;
}
static u8 crc8(const u8 *d, int n)
{
    u8 c = 0;
    for (int i = 0; i <= n; i++)
        for (int k = 7; k >= 0; k--) {
            u8 x = (c & 0x80) ? 0x85 : 0;
            c = (u8)(c << 1);
            if (i < n && ((d[i] >> k) & 1)) c |= 1;
            c ^= x;
        }
    return c;
}
static u8 pmem(u32 a) { return (u8)(a * 37u + 11u); }          /* the pak model's contents */

#ifndef SNAC_RUN
#define SNAC_RUN 'B'
#endif
/* which channels a real device answers (the others: the Pocket's pad on 1 if nothing is there, nothing on 2 to 4) */
static u32 s_real, s_pak;
#define REAL(ch)  ((s_real >> (ch)) & 1u)
#define REALPAK(ch) ((s_pak >> (ch)) & 1u)
#define THERE(ch) ((ch) == 0 || REAL(ch))
static const u32 btn[4] = { 0xA5C3, 0x0001, 0, 0x0F0F };
static const u32 stk[4] = { 0x1020, 0xF008, 0, 0x7F81 };

/* four commands of the same kind on the four channels, after an optional pad byte (libultra's 0xFF) */
static void four(u8 tx, u8 rx, u8 cmd, int pad)
{
    volatile u8 *b = block();
    clear_block();
    u32 o = 0;
    if (pad) b[o++] = 0xFF;
    for (int ch = 0; ch < 4; ch++) { b[o++] = tx; b[o++] = rx; b[o++] = cmd; for (u32 i = 0; i < rx; i++) b[o++] = 0xFF; }
    b[o] = 0xFE;
    pif();
}
static void status_block(int pad, u32 id)
{
    four(1, 3, 0, pad);
    volatile u8 *b = block();
    const u32 o = pad ? 1u : 0u;
    for (int ch = 0; ch < 4; ch++) {
        volatile u8 *c = b + o + 6u * ch;
        for (int i = 0; i < 3; i++) REPORT[16 + 4 * ch + i] = c[3 + i];
        REPORT[16 + 4 * ch + 3] = c[1];
        check(id + 10u * ch + 0, c[0] == 1 && c[2] == 0);                    /* the game's own tx and cmd bytes */
        if (THERE(ch)) {
            check(id + 10u * ch + 1, (c[1] & 0xC0) == 0);
            check(id + 10u * ch + 2, c[3] == 0x05 && c[4] == 0x00);
            const u8 want = REAL(ch) ? (REALPAK(ch) ? 0x01 : 0x02) : 0x01;   /* the virtual Controller Pak of controller 1: status 0x01 */
            check(id + 10u * ch + 3, c[5] == want);
        } else check(id + 10u * ch + 4, (c[1] & 0xC0) == 0x80);              /* no response */
    }
}
static void buttons_block(u32 id)
{
    four(1, 4, 1, 0);
    volatile u8 *b = block();
    for (int ch = 0; ch < 4; ch++) {
        volatile u8 *c = b + 7u * ch;
        for (int i = 0; i < 4; i++) REPORT[32 + 4 * ch + i] = c[3 + i];
        if (REAL(ch)) {
            check(id + 10u * ch + 0, (c[1] & 0xC0) == 0);
            check(id + 10u * ch + 1, c[3] == (btn[ch] >> 8) && c[4] == (btn[ch] & 255));
            check(id + 10u * ch + 2, c[5] == (stk[ch] >> 8) && c[6] == (stk[ch] & 255));
        } else if (ch == 0) check(id + 10u * ch + 3, (c[1] & 0xC0) == 0);   /* the Pocket's pad */
        else check(id + 10u * ch + 4, (c[1] & 0xC0) == 0x80);
    }
}
/* a pak's 32-byte block read (cmd 2) or written (cmd 3) on channel `ch`: the channels before are skipped */
static volatile u8 *pak_cmd(int ch, int write, u32 a, const u8 *data, u32 *flags)
{
    volatile u8 *b = block();
    clear_block();
    u32 o = 0;
    for (int c = 0; c < ch; c++) b[o++] = 0;
    b[o++] = write ? 35u : 3u; b[o++] = write ? 1u : 33u; b[o++] = write ? 3u : 2u; b[o++] = (u8)(a >> 8); b[o++] = (u8)(a & 0xE0);
    const u32 rep = o;
    if (write) { for (u32 i = 0; i < 32u; i++) b[o++] = data[i]; b[o++] = 0xFF; }
    else for (u32 i = 0; i < 33u; i++) b[o++] = 0xFF;
    b[o] = 0xFE;
    pif();
    *flags = b[ch + 1] & 0xC0;
    return b + rep + (write ? 32u : 0u) - (write ? 0u : 0u);
}
static void paks(void)
{
    u8 data[32];
    u32 fl;
    for (int ch = 0; ch < 4; ch++) {
        if (REALPAK(ch)) {
            volatile u8 *r = pak_cmd(ch, 0, 0x0120, 0, &fl);
            u8 d[32]; int ok = 1;
            for (int i = 0; i < 32; i++) { d[i] = r[i]; ok &= r[i] == pmem(0x120u + i); }
            check(200u + ch, fl == 0 && ok && r[32] == crc8(d, 32));         /* the device's data and its own CRC byte, unchanged */
            for (int i = 0; i < 32; i++) data[i] = (u8)(0x40 + 5 * i + ch);
            r = pak_cmd(ch, 1, 0x0210, data, &fl);
            if (ch == 0 || REPORT[5] == 0) REPORT[5] = s_write_ticks + s_read_ticks;
            check(210u + ch, fl == 0 && r[0] == crc8(data, 32));
            r = pak_cmd(ch, 0, 0x0210, 0, &fl);
            ok = 1;
            for (int i = 0; i < 32; i++) ok &= r[i] == data[i];
            check(220u + ch, fl == 0 && ok);
        }
    }
    if (REAL(1) && !REALPAK(1)) {                                            /* a pad with no pak: nobody answers a read of memory */
        pak_cmd(1, 0, 0x0100, 0, &fl);
        check(230u, fl == 0x80);
    }
    if (!REAL(0)) {                                                          /* the virtual Controller Pak of controller 1 */
        for (int i = 0; i < 32; i++) data[i] = (u8)(0x90 + i);
        volatile u8 *r = pak_cmd(0, 1, 0x0300, data, &fl);
        check(240u, fl == 0 && r[0] == crc8(data, 32));
        r = pak_cmd(0, 0, 0x0300, 0, &fl);
        u8 d[32]; int ok = 1;
        for (int i = 0; i < 32; i++) { d[i] = r[i]; ok &= r[i] == data[i]; }
        check(241u, fl == 0 && ok && r[32] == crc8(d, 32));
    }
}
/* the cartridge's EEPROM: channel 4, the runtime's whatever the wires hold */
static void eeprom(void)
{
    volatile u8 *b = block();
    static const u8 e[8] = { 0x4D, 0x49, 0x52, 0x4C, 0x4F, 0x21, 0x00, 0xA5 };
    clear_block();
    b[0] = b[1] = b[2] = b[3] = 0;
    b[4] = 1; b[5] = 3; b[6] = 0; b[10] = 0xFE;
    pif();
    check(250u, (b[5] & 0xC0) == 0 && b[7] == 0x00 && b[8] == 0x80);        /* a 4 Kbit EEPROM */
    clear_block();
    b[0] = b[1] = b[2] = b[3] = 0;
    b[4] = 10; b[5] = 1; b[6] = 5; b[7] = 3; for (int i = 0; i < 8; i++) b[8 + i] = e[i];
    b[16] = 0xFF; b[17] = 0xFE;
    pif();
    check(251u, (b[5] & 0xC0) == 0);
    clear_block();
    b[0] = b[1] = b[2] = b[3] = 0;
    b[4] = 2; b[5] = 8; b[6] = 4; b[7] = 3; for (int i = 0; i < 8; i++) b[8 + i] = 0xFF;
    b[16] = 0xFE;
    pif();
    int ok = 1;
    for (int i = 0; i < 8; i++) ok &= b[8 + i] == e[i];
    check(252u, (b[5] & 0xC0) == 0 && ok);
}
static void wait_ticks(u32 n) { u32 t0 = count(); while (count() - t0 < n) ; }
void game_main(void)
{
    for (u32 i = 0; i < 64u; i++) REPORT[i] = 0;
    /* a moment for the hardware's first scan to have found the devices (the runtime waits for it before the game starts, too) */
    for (volatile u32 i = 0; i < 20000u; i++) ;
    if (SNAC_RUN == 'B') { s_real = 0xB; s_pak = 0x9; }
    if (SNAC_RUN == 'C') { s_real = 0x2; s_pak = 0; }
    if (SNAC_RUN == 'E') {
        status_block(0, 900u);                     /* the menu is Off: virtual only */
        wait_ticks(8000000u);
        u32 polls = 0;
        for (;;) {                                 /* until the menu (the bench changes it) brings controller 2 */
            four(1, 3, 0, 0);
            polls++;
            if ((block()[6 + 1] & 0xC0) == 0 || polls > 400u) break;
            wait_ticks(200000u);
        }
        REPORT[7] = polls;
        check(950u, polls <= 400u);
        s_real = 0xB; s_pak = 0x9;
    }
    status_block(0, 100u);
    REPORT[4] = s_write_ticks + s_read_ticks;
    status_block(1, 400u);                         /* libultra's block starts with a pad byte */
    buttons_block(500u);
    REPORT[3] = s_write_ticks + s_read_ticks;
    paks();
    eeprom();
    status_block(0, 700u);                         /* and again: nothing left behind */
    buttons_block(800u);
    REPORT[1] = fails; REPORT[2] = checks;
    REPORT[0] = 0x600D600Du;
    for (;;) ;
}
__asm__(
"    .set noreorder\n"
"    .section .text.start, \"ax\"\n"
"    .globl _start\n"
"_start:\n"
"    lui $sp, 0x8010\n"
"    jal game_main\n"
"    addiu $sp, $sp, -16\n"
"1:  b 1b\n"
"    nop\n");
