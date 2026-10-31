/* tlbstate_test: a game whose TLB must come back with a save state (n64rt_state.c) -- a program linked for 0x8000_0400 that runs as a
 * game under the loader (OWN) on the SoC in sim/mips_soc. It maps six pairs of pages of 4 KiB .. 64 KiB, some global and some of
 * ASID 5, onto two sets of frames. In phase 0 (from its start) a pair's even page is the frame set 0's and its odd page the set 1's;
 * at Count 0x0340_0000 -- about field 70 -- it maps them all the other way round (phase 1), and goes on forever: each turn reads a word
 * of every page, does tlbp for every pair, tlbr for every entry, and compares the registers it gets with those of its phase; Wired,
 * Context's PTEBase and EntryHi's ASID are kept and checked at each turn too.
 * A state made after the switch and put back into a run that booted afresh and is still in phase 0 (the load comes before field 70)
 * brings the phase with the RDRAM, the entries with the TLB: if the TLB did not come back, the pages are those of phase 0 and the
 * checks fail.
 * The report is in the RDRAM at 0x8030_0000 (DUMP=0x43B00000,32 in the sim): word 0 the magic 0x600D0000 + the turns done (the
 * turns count only grows), word 1 the failed checks, word 2 the phase, words 3-6 the first failed check (which, got, want, turn).
 *
 *   make -f tlbstate_test.mk; ROM=build_tlbstate/tlbstate_test.z64 SS_SAVE=100 SS_EXIT=1 obj_soc/tb_soc <loader n64rt.bin> -c 400 -o <dir>
 *   ROM=... SS_LOAD=<dir>/state.bin SS_LOAD_AT=40 DUMP=0x43B00000,32 obj_soc/tb_soc <loader n64rt.bin> -c 400   (the report has phase 1, no
 *   failed check, and more turns than the state's) */
typedef unsigned int u32;
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)))
#define TLBWI() __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop")
#define TLBR()  __asm__ volatile(".set push\n.set mips3\ntlbr\n.set pop")
#define TLBP()  __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop")
#define REPORT ((volatile u32 *)0x80300000u)
#define NPAIR 6u
#define ASID 5u

/* pair i: virtual 0x0400_0000 + i * 0x0100_0000, pages of SIZE(i) bytes; global when i is even */
static const u32 s_mask[NPAIR] = { 0x00000000u, 0x00006000u, 0x0001E000u, 0x00006000u, 0x0001E000u, 0x00000000u };
static u32 size(u32 i) { return (s_mask[i] + 0x2000u) >> 1; }
static u32 va(u32 i) { return 0x04000000u + i * 0x01000000u; }
static u32 frame(u32 set, u32 i) { return 0x00400000u + set * 0x00100000u + i * 0x00020000u; }       /* a frame of up to 64 KiB, word k = set << 28 | i << 24 | k */
static u32 lo(u32 pa, u32 g) { return (pa >> 6) | 0x1Au | g; }                                       /* cached, valid, dirty */
static u32 eset(u32 phase, u32 odd) { return phase ^ odd; }                                          /* the frame set of an even (0) or odd (1) page */

static u32 s_fails;
static void check(u32 what, u32 got, u32 want, u32 turn)
{
    if (got == want) return;
    if (!s_fails) { REPORT[3] = what; REPORT[4] = got; REPORT[5] = want; REPORT[6] = turn; }
    s_fails++;
    REPORT[1] = s_fails;
}
static void map(u32 phase)
{
    for (u32 i = 0; i < NPAIR; i++) {
        const u32 g = i % 2u == 0u;
        MTC0(0, i); MTC0(5, s_mask[i]); MTC0(10, va(i) | (g ? 0u : ASID));
        MTC0(2, lo(frame(eset(phase, 0), i), g)); MTC0(3, lo(frame(eset(phase, 1), i), g));
        TLBWI();
    }
    MTC0(10, ASID);
}

void game_main(void)
{
    for (u32 set = 0; set < 2u; set++)
        for (u32 i = 0; i < NPAIR; i++)
            for (u32 k = 0; k < 0x4000u; k++) *K1(frame(set, i) + 4u * k) = set << 28 | i << 24 | k;
    for (u32 i = 0; i < 32u; i++) { MTC0(0, i); MTC0(5, 0); MTC0(10, 0x80000000u + i * 0x2000u); MTC0(2, 0); MTC0(3, 0); TLBWI(); }
    REPORT[0] = 0x600D0000u;
    u32 phase = 0;
    map(phase);
    MTC0(6, 7);                                     /* Wired */
    MTC0(4, 0x23000000u);                           /* Context's PTEBase */
    for (u32 turn = 1;; turn++) {
        if (phase == 0 && MFC0(9) >= 0x03400000u) { phase = 1; map(phase); REPORT[2] = phase; }
        check(1, MFC0(6), 7, turn);
        check(2, MFC0(4) & 0xFF800000u, 0x23000000u, turn);
        check(3, MFC0(10) & 0xFFu, ASID, turn);
        for (u32 i = 0; i < NPAIR; i++) {
            const u32 s = size(i), g = i % 2u == 0u;
            const u32 o = (s - 4u) & ~3u;                                         /* the page's last word */
            check(0x10 + i, *(volatile u32 *)(va(i) + o), eset(phase, 0) << 28 | i << 24 | (o >> 2), turn);
            check(0x20 + i, *(volatile u32 *)(va(i) + s + o), eset(phase, 1) << 28 | i << 24 | (o >> 2), turn);
            MTC0(10, va(i) | (g ? 0u : ASID)); TLBP();
            check(0x30 + i, MFC0(0), i, turn);
            MTC0(0, i); TLBR();
            check(0x40 + i, MFC0(5), s_mask[i], turn);
            check(0x50 + i, MFC0(10), va(i) | (g ? 0u : ASID), turn);
            check(0x60 + i, MFC0(2), lo(frame(eset(phase, 0), i), g), turn);
            check(0x70 + i, MFC0(3), lo(frame(eset(phase, 1), i), g), turn);
            MTC0(10, ASID);
        }
        REPORT[0] = 0x600D0000u + turn;
        REPORT[2] = phase;
    }
}

__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
