/* il_test: a serrated hi-res picture (480i) as libultra drives one, for the scan-out's fields -- a program linked for
 * 0x8000_0400 that runs as a game under the loader (OWN), on the SoC in sim/mips_soc or on a Pocket.
 *
 * The picture is 640 x 480 RGBA5551 at 0x8010_0000, WIDTH 640, Y_SCALE 2.0, SERRATE. At each retrace (VI_CURRENT's line 1,
 * after the VI interrupt's line) it does what libultra's handler does: reads CURRENT's field bit and writes that field's
 * ORIGIN and V_START (libultra's interlaced hi-res modes: field 0 one row in, V_START 0x2301FD; field 1 two rows in,
 * 0x2501FF). Woven, the frame shows the rows in order:
 *   - rows coloured by their number, r mod 4: red, green, blue, white -- a field put on the wrong lines reads green, red,
 *     white, blue instead;
 *   - two diagonals a pixel wide from the top: staircases of pairs if the fields are crossed;
 *   - with ROWCODE set, instead, each row's number as a bar code (bit b white or black in the columns 64 b .. 64 b + 63, the
 *     last 64 columns white), for reading off a capture of the Pocket's output which row each of its lines shows.
 * With FULLWIN set the window is 240 lines a field (V_START 0x230203 / 0x250205), the field's every line a row of the picture.
 * With FLASH set, a second picture, white, 1 MiB on: the fields go to it and back every two fields (a whole frame each),
 * for catching a tear in what shows them.
 * With PROG_EVERY=n, every n fields the picture goes progressive (SERRATE off) for a second, and back: the change of mode.
 *
 * With PROGONLY set the picture is never serrated (the same rows, progressive: the comparison).
 *
 *   make -f il_test.mk [EXTRA="-DROWCODE=1 -DFULLWIN=1 -DFLASH=1 -DPROGONLY=1 -DPROG_EVERY=600"]
 *   ROM=build_il/il_test.z64 VGA=1 VGAT=1 obj_soc/tb_soc <n64rt.bin> -c 70 -o out */
typedef unsigned int u32;
typedef unsigned short u16;
#define VI(r) (*(volatile u32 *)(0xA4400000u + (r)))
#define FB_PA 0x100000u
#define FB ((volatile u16 *)(0xA0000000u | FB_PA))
#ifndef PROG_EVERY
#define PROG_EVERY 0
#endif
static const u16 s_col[4] = { 0xF801u, 0x07C1u, 0x003Fu, 0xFFFFu };     /* red, green, blue, white */
#ifdef FULLWIN
#define VS0 0x00230203u
#define VS1 0x00250205u
#else
#define VS0 0x002301FDu
#define VS1 0x002501FFu
#endif
static u32 ctrl(u32 il)                                                  /* 16 bits, gamma off, AA off (resample), SERRATE */
{
#ifdef PROGONLY
    il = 0;                                                              /* (PROGONLY: the same picture, never serrated) */
#endif
    return 0x00000202u | (il ? 0x40u : 0u);
}
void game_main(void)
{
    for (u32 y = 0; y < 482; y++) {                             /* (a word, two pixels, at a time) */
#ifdef ROWCODE
        for (u32 x = 0; x < 640; x += 2) {
            const u32 b = x >> 6, w = b >= 9u || ((y >> b) & 1u) ? 0xFFFFu : 0x0001u;
            *(volatile u32 *)&FB[y * 640u + x] = w << 16 | w;
        }
#else
        const u32 c = s_col[y & 3u];
        for (u32 x = 0; x < 640; x += 2) *(volatile u32 *)&FB[y * 640u + x] = c << 16 | c;
        FB[y * 640u + (y < 640u ? y : 639u)] = 0x0001u;
        if (y + 160u < 640u) FB[y * 640u + y + 160u] = 0x0001u;
#endif
    }
#ifdef FLASH
    for (u32 i = 0; i < 482u * 640u; i += 2) *(volatile u32 *)&FB[0x80000u + i] = 0xFFFFFFFFu;
#endif
    VI(0x00) = ctrl(1);
    VI(0x08) = 640;                     /* WIDTH */
    VI(0x0C) = 2;                       /* V_INTR */
    VI(0x14) = 0x03E52239u;             /* BURST */
    VI(0x18) = 0x20Cu;                  /* V_SYNC: 525 lines, interlaced */
    VI(0x1C) = 0x00000C15u;             /* H_SYNC */
    VI(0x20) = 0x0C150C15u;             /* LEAP */
    VI(0x24) = 0x006C02ECu;             /* H_START */
    VI(0x2C) = 0x000E0204u;             /* V_BURST */
    VI(0x30) = 0x400u;                  /* X_SCALE 1.0 */
    VI(0x34) = 0x02000800u;             /* Y_SCALE 2.0 */
    VI(0x28) = VS1;
    VI(0x04) = FB_PA + 1280u;
    u32 last = ~0u, fields = 0;
    for (;;) {
        const u32 cur = VI(0x10), line = cur >> 1;
        if (line == last) continue;
        last = line;
        if (line != 1u) continue;
        const u32 f = cur & 1u;          /* (the handler: CURRENT's field, then that field's registers) */
#ifdef FLASH
        const u32 pic = ((fields >> 1) & 1u) ? 0x100000u : 0u;
        if (!PROG_EVERY) fields++;
#else
        const u32 pic = 0;
#endif
        VI(0x04) = FB_PA + pic + (f ? 2560u : 1280u);
        VI(0x28) = f ? VS1 : VS0;
        if (PROG_EVERY) {
            const u32 k = ++fields % PROG_EVERY;
            if (k == PROG_EVERY - 60u) VI(0x00) = ctrl(0);
            else if (k == 0u) VI(0x00) = ctrl(1);
        }
    }
}
__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
