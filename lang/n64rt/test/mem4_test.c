/* mem4_test: a game for the core's "Expansion Pak" setting -- a program linked for 0x8000_0400 that runs as a game under the
 * loader (OWN) on the SoC in sim/mips_soc. It sizes the memory as libultra's osGetMemSize does (a megabyte at a time from 4 MiB:
 * a word written at its start and one at its end, read back) and compares it with what the boot code left in osMemSize:
 *   the Pak in (the default)               8 MiB by both, and the upper half is memory
 *   the Pak out (INTERACT=0x10000138:1)    4 MiB by both: a store to the upper half goes nowhere and a load of it is 0,
 *                                          through KSEG1 and KSEG0, while the lower half is untouched
 * The report is in the RDRAM at 0x8030_0000 (DUMP=0x43B00000,8 in the sim): word 0 the magic 0x600D600D or 0xBADBAD00 + the
 * number of failed checks, words 2-4 the first failed check (which, got, want), word 5 the size found, word 6 0xD0DE when done.
 *
 *   make -f mem4_test.mk; ROM=build_mem4/mem4_test.z64 [INTERACT=0x10000138:1] DUMP=0x43B00000,8 obj_soc/tb_soc <n64rt.bin> -c 60 */
typedef unsigned int u32;
#define K0(pa) ((volatile u32 *)(0x80000000u | (pa)))
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define REPORT K0(0x300000u)
static u32 s_fails;
static void check(u32 what, u32 got, u32 want)
{
    if (got == want) return;
    if (!s_fails) { REPORT[2] = what; REPORT[3] = got; REPORT[4] = want; }
    s_fails++;
}
void game_main(void)
{
    const u32 boot = *K0(0x318u);                               /* osMemSize, as the boot code left it */
    u32 size = 0x300000u;
    for (;;) {                                                  /* (osGetMemSize's loop) */
        size += 0x100000u;
        volatile u32 *a = K1(size), *b = K1(size + 0x100000u - 4u);
        u32 sa = *a, sb = *b;
        *a = 0x12345678u; *b = 0x87654321u;
        if (*a != 0x12345678u || *b != 0x87654321u) break;
        *a = sa; *b = sb;
        if (size >= 0xF00000u) { size += 0x100000u; break; }
    }
    REPORT[5] = size;
    check(1, size, boot);
    check(2, boot == 0x400000u || boot == 0x800000u, 1);
    *K1(0x3FFFF0u) = 0xCAFE0001u; check(3, *K1(0x3FFFF0u), 0xCAFE0001u);          /* the lower half's last words */
    *K0(0x3FFFE0u) = 0xCAFE0002u; check(4, *K0(0x3FFFE0u), 0xCAFE0002u);
    *K1(0x400010u) = 0xCAFE0003u; *K0(0x7FFF00u) = 0xCAFE0004u;
    if (boot == 0x800000u) { check(5, *K1(0x400010u), 0xCAFE0003u); check(6, *K0(0x7FFF00u), 0xCAFE0004u); }
    else {
        check(5, *K1(0x400010u), 0); check(6, *K0(0x7FFF00u), 0);
        check(7, *(volatile unsigned char *)(0x80400013u), 0);                    /* (a byte, a half: loads of 0 too) */
        *(volatile unsigned short *)(0xA0500002u) = 0x1234; check(8, *(volatile unsigned short *)(0xA0500002u), 0);
    }
    *K1(0x800000u) = 0xCAFE0005u; check(9, *K1(0x800000u), 0);                    /* past the RDRAM: nothing, with or without */
    REPORT[0] = s_fails ? 0xBADBAD00u + s_fails : 0x600D600Du;
    REPORT[6] = 0xD0DEu;
    for (;;) ;
}
__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
