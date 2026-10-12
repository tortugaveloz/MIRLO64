/* tlbkeep_test: a game for the entries the runtime keeps when a tlbwr puts them out (n64rt_os.c, "the entries a tlbwr put out") --
 * a program linked for 0x8000_0400 that runs as a game under the loader (OWN) on the SoC in sim/mips_soc, with a refill handler
 * of its own at 0x8000_0000 that fills the TLB from a page table by Context (PTEBase 0x8000_0000: 16 bytes a pair, EntryLo0,
 * EntryLo1) with tlbwr. Wired is 31: every tlbwr writes entry 31, so each refill puts the one before out.
 * What it checks (the refills are counted by the handler):
 *   1. 64 page pairs read, then read again: the data, and no refill the second time (the console would take 64 more: its TLB
 *      holds one of them -- that is what is kept); a store through a kept page
 *   2. tlbr of the 32 entries shows none of the kept pairs; a tlbp for one finds nothing -- and the page then takes a refill
 *   3. a frame of a kept pair mapped for another page (the game's page table changed): the kept pair is gone, its page takes a
 *      refill and reads its new frame
 *   4. the cartridge read into a kept pair's frame (a PI DMA): the same
 *   5. a tlbwi of the kept pair's own page: the new entry's frames, no refill
 *   6. three tlbwi in a row that map nothing (a flush): every kept pair takes a refill again
 * The report is in the RDRAM at 0x8030_0000 (DUMP=0x43B00000,8 in the sim): word 0 the magic 0x600D600D or 0xBADBAD00 + the
 * number of failed checks, words 2-4 the first failed check (which, got, want), word 5 the refills in all, word 6 0xD0DE when done.
 * A runtime built with NO_TLBKEEP=1 (every tlbwr as the console's) fails check 2 alone, by design: 128 refills, not 64.
 *
 *   make -f tlbkeep_test.mk; ROM=build_tlbkeep/tlbkeep_test.z64 DUMP=0x43B00000,8 obj_soc/tb_soc <loader n64rt.bin> -c 60 */
typedef unsigned int u32;
#define K0(pa) ((volatile u32 *)(0x80000000u | (pa)))
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)))
#define TLBWI() __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop")
#define TLBR()  __asm__ volatile(".set push\n.set mips3\ntlbr\n.set pop")
#define TLBP()  __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop")
#define REPORT ((volatile u32 *)0x80300000u)
#define REFILLS (*K0(0x10000))
#define GENERAL (*K0(0x10004))
#define LO(pa) ((((pa) >> 12) << 6) | 0x1Fu)             /* cached, D, V, G */
#define VA(i) (0x04000000u + (i) * 0x2000u)
#define FRAME(n) (0x00100000u + (n) * 0x1000u)           /* a data page: word 0 is 0xF0000000 | n */
#define NPAIR 64u

static u32 s_fails;
static void check(u32 what, u32 got, u32 want)
{
    if (got == want) return;
    if (!s_fails) { REPORT[2] = what; REPORT[3] = got; REPORT[4] = want; }
    s_fails++;
}
static void pte(u32 pair, u32 lo0, u32 lo1) { volatile u32 *e = K0((VA(pair) >> 13) << 4); e[0] = lo0; e[1] = lo1; }
static u32 rd(u32 pair, u32 odd) { return *(volatile u32 *)(VA(pair) + odd * 0x1000u); }
static void write_entry(u32 i, u32 hi, u32 lo0, u32 lo1) { MTC0(0, i); MTC0(5, 0); MTC0(10, hi); MTC0(2, lo0); MTC0(3, lo1); TLBWI(); }

extern char h_refill[], h_refill_end[], h_gen[], h_gen_end[];
__asm__(
"    .set push\n    .set noreorder\n    .set noat\n    .set mips3\n    .text\n"
"h_refill:\n"                                   /* (copied to 0x8000_0000) */
"    mfc0  $k0, $4\n"
"    lw    $k1, 0($k0)\n"
"    mtc0  $k1, $2\n"
"    lw    $k1, 4($k0)\n"
"    mtc0  $k1, $3\n"
"    mtc0  $zero, $5\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0($k0)\n"
"    nop\n"
"    tlbwr\n"
"    nop\n"
"    eret\n"
"    nop\n"
"h_refill_end:\n"
"h_gen:\n"                                      /* (0x8000_0180: nothing here is expected -- counted, and the instruction skipped) */
"    lui   $k0, 0x8001\n"
"    lw    $k1, 4($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 4($k0)\n"
"    mfc0  $k1, $14\n"
"    addiu $k1, $k1, 4\n"
"    mtc0  $k1, $14\n"
"    nop\n"
"    eret\n"
"    nop\n"
"h_gen_end:\n"
"    .set pop\n");
static void copy(u32 dst, const char *a, const char *b)
{
    for (const u32 *p = (const u32 *)a; p < (const u32 *)b; p++, dst += 4) *K1(dst) = *p;
}

void game_main(void)
{
    MTC0(12, MFC0(12) & ~0x1u);                                    /* no interrupts: the vectors are this program's */
    copy(0x0, h_refill, h_refill_end);
    copy(0x180, h_gen, h_gen_end);
    for (u32 o = 0; o < 0x200; o += 32) __asm__ volatile("cache 0x10, 0(%0)" :: "r"(0x80000000u + o));      /* (the I-cache's lines of the vectors) */
    REFILLS = 0; GENERAL = 0;
    for (u32 n = 0; n < 256; n++) *K1(FRAME(n)) = 0xF0000000u | n;
    for (u32 i = 0; i < 32; i++) write_entry(i, 0x80000000u + i * 0x2000u, 0, 0);
    MTC0(4, 0x80000000u);                                          /* Context: PTEBase */
    MTC0(6, 31);                                                   /* Wired: tlbwr writes entry 31 */
    MTC0(10, 0);
    for (u32 i = 0; i < NPAIR + 8; i++) pte(i, LO(FRAME(2 * i)), LO(FRAME(2 * i + 1)));

    /* 1. every pair read: a refill each; read again: none, and the data */
    for (u32 i = 0; i < NPAIR; i++) { check(100 + i, rd(i, 0), 0xF0000000u | (2 * i)); check(200 + i, rd(i, 1), 0xF0000000u | (2 * i + 1)); }
    check(1, REFILLS, NPAIR);
    for (u32 i = 0; i < NPAIR; i++) { check(300 + i, rd(i, 0), 0xF0000000u | (2 * i)); check(400 + i, rd(i, 1), 0xF0000000u | (2 * i + 1)); }
    check(2, REFILLS, NPAIR);
    *(volatile u32 *)(VA(3) + 0x10u) = 0x12345678u;                /* a store through a kept page */
    check(3, *K1(FRAME(6) + 0x10u), 0x12345678u);
    check(4, REFILLS, NPAIR);

    /* 2. the 32 entries as the VR4300 has them: the last pair in entry 31 and no other; a tlbp for a kept pair finds nothing,
     *    and the page is a refill away from then on */
    {
        u32 seen = 0;
        for (u32 i = 0; i < 32; i++) { MTC0(0, i); TLBR(); u32 hi = MFC0(10); if (hi >= VA(0) && hi < VA(NPAIR)) seen += 1u + (i << 8); }
        check(5, seen, 1u + (31u << 8));
        MTC0(10, VA(5)); TLBP(); check(6, MFC0(0) >> 31, 1);
        MTC0(10, VA(NPAIR - 1)); TLBP(); check(7, MFC0(0), 31);
        MTC0(10, 0);
        u32 r0 = REFILLS;
        check(8, rd(5, 0), 0xF0000000u | 10u);
        check(9, REFILLS, r0 + 1);
        check(10, rd(6, 1), 0xF0000000u | 13u);                    /* (another kept pair: still no refill) */
        check(11, REFILLS, r0 + 1);
    }

    /* 3. the frames of a kept pair (10) mapped for another page (NPAIR + 1), and the pair given other frames: it takes a refill
     *    and reads the new ones */
    {
        u32 r0 = REFILLS;
        pte(NPAIR + 1, LO(FRAME(20)), LO(FRAME(21)));
        pte(10, LO(FRAME(200)), LO(FRAME(201)));
        check(12, rd(NPAIR + 1, 0), 0xF0000000u | 20u);
        check(13, REFILLS, r0 + 1);
        check(14, rd(10, 0), 0xF0000000u | 200u);
        check(15, rd(10, 1), 0xF0000000u | 201u);
        check(16, REFILLS, r0 + 2);
    }

    /* 4. the cartridge read into a kept pair's frame (12: frames 24, 25), and the pair given other frames */
    {
        u32 r0 = REFILLS;
        check(17, rd(12, 0), 0xF0000000u | 24u);
        check(18, REFILLS, r0);
        *K1(0x04600000u) = FRAME(24);                              /* PI_DRAM_ADDR */
        *K1(0x04600004u) = 0x10000000u;                            /* PI_CART_ADDR: the ROM's header */
        *K1(0x0460000Cu) = 0xFu;                                   /* PI_WR_LEN: 16 bytes */
        while (*K1(0x04600010u) & 3u) ;                            /* PI_STATUS: busy */
        *K1(0x04600010u) = 2u;                                     /* (the interrupt cleared) */
        check(19, *K1(FRAME(24)), 0x80371240u);
        pte(12, LO(FRAME(202)), LO(FRAME(203)));
        check(20, rd(12, 0), 0xF0000000u | 202u);
        check(21, REFILLS, r0 + 1);
    }

    /* 5. a tlbwi of a kept pair's own page (14) with other frames: those, at once */
    {
        u32 r0 = REFILLS;
        write_entry(5, VA(14), LO(FRAME(204)), LO(FRAME(205)));
        MTC0(10, 0);
        check(22, rd(14, 0), 0xF0000000u | 204u);
        check(23, rd(14, 1), 0xF0000000u | 205u);
        check(24, REFILLS, r0);
    }

    /* 6. a flush (three entries that map nothing, one after another): a kept pair is a refill away again */
    {
        u32 r0 = REFILLS;
        check(25, rd(16, 0), 0xF0000000u | 32u);
        check(26, REFILLS, r0);
        for (u32 i = 1; i < 4; i++) write_entry(i, 0x80000000u + i * 0x2000u, 0, 0);
        MTC0(10, 0);
        check(27, rd(16, 0), 0xF0000000u | 32u);
        check(28, REFILLS, r0 + 1);
        check(29, rd(17, 1), 0xF0000000u | 35u);
        check(30, REFILLS, r0 + 2);
    }
    check(31, GENERAL, 0);
    REPORT[5] = REFILLS;
    REPORT[0] = s_fails ? 0xBADBAD00u + s_fails : 0x600D600Du;
    REPORT[6] = 0xD0DEu;
    for (;;) ;
}

__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
