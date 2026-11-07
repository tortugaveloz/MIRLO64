/* The "reserved encodings" groups of nemu64-test (tests/exception_instructions/reserved.rs: 653-662 of its index), ported to C for the bench, with the
 * console's expected values: the COP0 CO functions are decoded by the function alone (the undefined ones, but 0x10, do nothing, whatever the other
 * bits; tlbr, tlbp and eret execute with any), CFC0, CTC0 and BC0 do nothing, the reserved integer encodings raise Reserved Instruction, and a COP1
 * instruction with COP1 unusable raises Coprocessor Unusable (CE 1) before anything else, and one the FPU does not have (COP1 usable) is a Reserved
 * Instruction of the core's (656; the runtime makes it the console's Floating-Point Exception). A check's id is (test number << 8) + the check.
 *
 *   ./tlb_rom.sh OUT.z64 reserved_rom.c && obj_c/Vmips_core OUT.z64 -x         (the model has these too: the lockstep holds)
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_) :: "memory"); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)) : "memory")
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define K0(pa) ((volatile u32 *)(0x80000000u | (pa)))

volatile u32 h_cause, h_epc, h_badv, h_count, h_ret, h_status;
volatile u32 sink;

static u32 nfail;
static void check(u32 id, u64 got, u64 want)
{
    if (got == want) return;
    if (nfail < 32) {
        volatile u32 *e = K1(0x000F0F08 + 20 * nfail);
        e[0] = id; e[1] = (u32)(got >> 32); e[2] = (u32)got; e[3] = (u32)(want >> 32); e[4] = (u32)want;
    }
    nfail++;
}
#ifndef LIM
#define LIM 1
#endif
/* a test's result: failed checks are counted per test, in a table of 0x200 words at 0x000F2000 (1 = failed) */
static u32 cur;
static void tcheck(u32 n, u64 got, u64 want)
{
    if (got != want) {
        volatile u32 *f = K1(0x000F2000 + 4 * cur);             /* (only the first failed check of a test is reported; the count is kept) */
        if (*f < LIM) check((cur << 8) | n, got, want);
        *f += 1;
    }
}

__asm__(
"    .section .text.start,\"ax\"\n"
"    .set noreorder\n"
"    .globl _start\n"
"_start:\n"
"    lui   $sp, 0x800F\n"
"    ori   $sp, $sp, 0xF000\n"
"    jal   main\n"
"    nop\n"
"1:  b     1b\n"
"    nop\n"
"    .text\n"
"    .set noat\n"
"    .globl h_entry\n"
"h_entry:\n"
"    mfc0  $k0, $13\n"
"    lui   $k1, %hi(h_cause)\n"
"    sw    $k0, %lo(h_cause)($k1)\n"
"    mfc0  $k0, $14\n"
"    sw    $k0, %lo(h_epc)($k1)\n"
"    mfc0  $k0, $8\n"
"    sw    $k0, %lo(h_badv)($k1)\n"
"    lw    $k0, %lo(h_count)($k1)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, %lo(h_count)($k1)\n"
"    lw    $k0, %lo(h_cause)($k1)\n"
"    lw    $k1, %lo(h_ret)($k1)\n"
"    bnez  $k1, 2f\n"
"    nop\n"
"    mfc0  $k1, $14\n"
"    bgez  $k0, 3f\n"
"    nop\n"
"    addiu $k1, $k1, 4\n"
"3:  addiu $k1, $k1, 4\n"
"2:  mtc0  $k1, $14\n"
"    lui   $k0, %hi(h_ret)\n"
"    sw    $zero, %lo(h_ret)($k0)\n"
"    eret\n"
"    nop\n"
"    .set reorder\n"
"    .set at\n"
);
extern void h_entry(void);

#define DATA 0x00300000u            /* 8 KiB-aligned; 24 KiB */
#define CACHE(op, off, base) __asm__ volatile("cache %0, %1(%2)" :: "i"(op), "i"(off), "r"(base) : "memory")
#define LTAG(op, off, base) ({ u32 t_; __asm__ volatile("cache %1, %2(%3)\n\tnop\n\tnop\n\tmfc0 %0, $28\n\tnop\n\tnop" : "=r"(t_) : "i"(op), "i"(off), "r"(base) : "memory"); t_; })
#define D_IWBI 0x01
#define D_ILT  0x05
#define D_IST  0x09
#define D_CDE  0x0D
#define D_HI   0x11
#define D_HWBI 0x15
#define D_HWB  0x19

#define BUF 0x00310000u
static u32 run_iso(u32 word)
{
    volatile u32 *b = K1(BUF);
    b[0] = word; b[1] = 0; b[2] = 0x03E00008; b[3] = 0; b[4] = 0x03E00008; b[5] = 0; b[6] = 0x03E00008; b[7] = 0;
    u32 landing = 0xA0000000u | BUF | 8;
    h_count = 0; h_cause = 0;
    MTC0(14, landing);
    void (*f)(void) = (void (*)(void))(0xA0000000u | BUF);
    f();
    return h_count;
}
#define CO 0x42000000u
static void t653(void)
{
    cur = 653;
    for (u32 f = 0; f < 64; f++) {
        if (f == 1 || f == 2 || f == 6 || f == 8 || f == 24 || f == 0x10) continue;
        u32 n = run_iso(CO | f);
        tcheck(f, n, 0);
    }
}
static void t655(void)
{
    cur = 655;
    static const u32 rs[3] = { 2, 6, 8 };
    for (int i = 0; i < 3; i++) tcheck(i, run_iso((0x10u << 26) | (rs[i] << 21)), 0);
}
static const u32 GARB[5] = { 0, 0x01E00000, 0x001FFFC0, 0x01FFFFC0, 0x00200000 };
#define TWI(idx) do { MTC0(0, idx); __asm__ volatile(".set noreorder\n\tnop\n\tnop\n\ttlbwi\n\tnop\n\tnop\n\t.set reorder"); } while (0)
static void prime(void)
{
    MTC0(5, 0); MTC0(2, (0x111u << 6) | 0x1B); MTC0(3, (0x222u << 6) | 0x1B); MTC0(10, (0x12345u << 13) | 0xAB);
    TWI(30);
}
static void sentinels(void) { MTC0(10, 0); MTC0(2, 0); MTC0(3, 0); MTC0(5, 0x6000); MTC0(0, 30); }
static void t658(void)
{
    cur = 658;
    for (int g = 0; g < 5; g++) {
        prime(); sentinels();
        __asm__ volatile(".set noreorder\n\tnop\n\tnop\n\ttlbr\n\tnop\n\tnop\n\t.set reorder");
        u32 h0 = MFC0(10), l00 = MFC0(2), l10 = MFC0(3), p0 = MFC0(5);
        sentinels();
        u32 n = run_iso(CO | 1 | GARB[g]);
        u32 h1 = MFC0(10), l01 = MFC0(2), l11 = MFC0(3), p1 = MFC0(5);
        tcheck(g * 8, n, 0); tcheck(g * 8 + 1, h1, h0); tcheck(g * 8 + 2, l01, l00); tcheck(g * 8 + 3, l11, l10); tcheck(g * 8 + 4, p1, p0);
    }
}
static void t659(void)
{
    cur = 659;
    for (int g = 0; g < 5; g++) {
        prime();
        MTC0(10, (0x12345u << 13) | 0xAB); MTC0(0, 0x1F);
        u32 n = run_iso(CO | 8 | GARB[g]);
        tcheck(g * 2, n, 0); tcheck(g * 2 + 1, MFC0(0), 30);
    }
}
static void t660(void)
{
    cur = 660;
    for (int g = 0; g < 5; g++) {
        u32 st = MFC0(12);
        MTC0(12, st | 2);
        u32 n = run_iso(CO | 24 | GARB[g]);
        u32 st2 = MFC0(12);
        MTC0(12, st);
        tcheck(g * 2, n, 0); tcheck(g * 2 + 1, (st2 >> 1) & 1, 0);
    }
}
static void t661_654(void)
{
    cur = 654;
    static const u32 w[] = { 0x13u << 26, 0x1Cu << 26, 0x1Du << 26, 0x1Eu << 26, 0x1Fu << 26, 0x33u << 26, 0x3Bu << 26, 0x01, 0x05, 0x0A, 0x0B, 0x0E, 0x15, 0x28, 0x29, 0x35, 0x37, 0x39, 0x3D,
        (1u << 26) | (4u << 16), (1u << 26) | (5u << 16), (1u << 26) | (0x1Fu << 16), (0x10u << 26) | (3u << 21), (0x10u << 26) | (7u << 21), (0x10u << 26) | (0xFu << 21), CO | 0x10 };
    for (unsigned i = 0; i < sizeof w / 4; i++) tcheck(i, run_iso(w[i]), 1);
    cur = 661;
    for (int g = 0; g < 5; g++) tcheck(g, run_iso((CO | 0x10) | GARB[g]), 1);
}
static void t662(void)
{
    cur = 662;
    static const u32 fn[4] = { 3, 9, 0x1F, 0x3F };
    for (int f = 0; f < 4; f++) for (int g = 0; g < 5; g++) tcheck(f * 5 + g, run_iso((CO | fn[f]) | GARB[g]), 0);
}

static void t657(void)
{
    cur = 657;
    static const u32 rs[] = { 3, 7, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x12, 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };
    u32 st = MFC0(12);
    for (unsigned i = 0; i < sizeof rs / 4; i++) {
        MTC0(12, st & ~0x20000000u);
        u32 n = run_iso((0x11u << 26) | (rs[i] << 21));
        u32 c = h_cause;
        MTC0(12, st);
        tcheck(i * 2, n, 1); tcheck(i * 2 + 1, c & 0x3000007C, 0x1000002C);
    }
    for (u32 fmt = 0x10; fmt <= 0x15; fmt++) { if (fmt == 0x12 || fmt == 0x13) continue;
        for (u32 f = 0; f < 64; f++) {
            MTC0(12, st & ~0x20000000u);
            u32 n = run_iso((0x11u << 26) | (fmt << 21) | f);
            u32 c = h_cause;
            MTC0(12, st);
            tcheck(64 + f, n, 1); tcheck(130 + f, c & 0x3000007C, 0x1000002C);
        } }
}
/* ---- 656: with COP1 usable, the reserved COP1 encodings are not the FPU's: the core raises Reserved Instruction (the runtime delivers it to the game
 * as the console's Floating-Point Exception with FCSR's Unimplemented Operation) -- the reserved sub-opcodes, BC1's reserved conditions, and the
 * undefined functions of the four formats */
static int cop1_defined(u32 fmt, u32 f)
{
    if (fmt == 0x10 || fmt == 0x11)
        return f <= 0x0F || (f >= 0x30 && f <= 0x3F) || f == 0x24 || f == 0x25 || (f == 0x20 && fmt != 0x10) || (f == 0x21 && fmt != 0x11);
    return f == 0x20 || f == 0x21;
}
static void t656(void)
{
    cur = 656;
    static const u32 rs[] = { 3, 7, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x12, 0x13, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };
    u32 st = MFC0(12), n = 0;
    MTC0(12, st | 0x20000000u);
    for (unsigned i = 0; i < sizeof rs / 4; i++, n++) { u32 c = run_iso((0x11u << 26) | (rs[i] << 21)); tcheck(n & 255, c, 1); tcheck(256 + (n & 255), (h_cause >> 2) & 31, 10); }
    for (u32 rt = 4; rt < 32; rt++, n++) { u32 c = run_iso((0x11u << 26) | (8u << 21) | (rt << 16)); tcheck(n & 255, c, 1); tcheck(256 + (n & 255), (h_cause >> 2) & 31, 10); }
    for (u32 k = 0; k < 4; k++) {
        static const u32 fm[4] = { 0x10, 0x11, 0x14, 0x15 };
        for (u32 f = 0; f < 64; f++) if (!cop1_defined(fm[k], f)) { u32 c = run_iso((0x11u << 26) | (fm[k] << 21) | f); tcheck(n & 255, c, 1); tcheck(256 + (n & 255), (h_cause >> 2) & 31, 10); n++; }
    }
    MTC0(12, st);
}
int main(void)
{
    for (u32 v = 0; v < 0x200; v += 0x180) {
        u32 a = (u32)(void *)h_entry;
        *K1(v) = 0x3C1A0000u | (a >> 16);
        *K1(v + 4) = 0x375A0000u | (a & 0xFFFF);
        *K1(v + 8) = 0x03400008u;
        *K1(v + 12) = 0;
    }
    for (u32 i = 0; i < 0x200; i++) *K1(0x000F2000 + 4 * i) = 0;
    t656(); t657(); t653(); t655(); t658(); t659(); t660(); t661_654(); t662();
    *K1(0x000F0F04) = nfail;
    *K1(0x000F0F00) = nfail ? 0xBAD00000u | nfail : 0x600DC0DEu;
    for (;;) ;
}
