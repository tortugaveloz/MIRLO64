/* The cache groups of nemu64-test (tests/cache/{dcache,icache}.rs: the numbers 409-442 and 457-461 of its index), ported to C for the
 * bench, with the console's expected values. It runs on the RTL alone: the reference model has no caches, so the lockstep comparison
 * cannot hold here (tb TB_NOLOCK=1 -x skips it). A check's id is (test number << 8) + the check, so `check 1ac00` is test 0x1ac = 428.
 *
 *   ./tlb_rom.sh OUT.z64 cache_rom.c && TB_NOLOCK=1 obj_c/Vmips_core OUT.z64 -x
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
#define I_II   0x00
#define I_ILT  0x04
#define I_IST  0x08
#define I_HI   0x10
#define I_FILL 0x14
#define I_HWB  0x18
#define ST_INV 0u
#define ST_01  1u
#define ST_CLEAN 2u
#define ST_DIRTY 3u
#define TAGLO(pa, st) ((((pa) >> 12) << 8) | ((st) << 6))

/* ---- 409: cached vs uncached (the stores and loads of every width) */
static void t409(void)
{
    cur = 409;
    volatile u32 *c = K0(DATA), *u = K1(DATA);
    for (int i = 0; i < 4096 / 4; i++) u[i] = 0;
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t.set hardfloat\n\t"
        "ll $0, 0(%0)\n\t"
        "lui $8, 0x0011\n\tori $8, $8, 0x2233\n\tdsll $8, $8, 16\n\tori $8, $8, 0x4455\n\tdsll $8, $8, 16\n\tori $8, $8, 0x6677\n\t"
        "dmtc1 $8, $f0\n\t"
        "sb $8, 3(%0)\n\tsb $8, 16+1(%0)\n\tsh $8, 32+2(%0)\n\tsh $8, 48+0(%0)\n\tsw $8, 64+4(%0)\n\tsw $8, 80+0(%0)\n\tsd $8, 96+0(%0)\n\t"
        "sc $8, 112+4(%0)\n\t"
        "lui $8, 0x0011\n\tori $8, $8, 0x2233\n\tdsll $8, $8, 16\n\tori $8, $8, 0x4455\n\tdsll $8, $8, 16\n\tori $8, $8, 0x6677\n\t"
        "scd $8, 128+0(%0)\n\tswc1 $f0, 144+4(%0)\n\tsdc1 $f0, 160+0(%0)\n\t"
        "lui $8, 0xAABB\n\tori $8, $8, 0xCCDD\n\tdsll $8, $8, 16\n\tori $8, $8, 0xABCD\n\tdsll $8, $8, 16\n\tori $8, $8, 0xEFED\n\t"
        "dmtc1 $8, $f0\n\t"
        "sb $8, 3(%1)\n\tsb $8, 16+1(%1)\n\tsh $8, 32+2(%1)\n\tsh $8, 48+0(%1)\n\tsw $8, 64+4(%1)\n\tsw $8, 80+0(%1)\n\tsd $8, 96+0(%1)\n\t"
        "sc $8, 112+4(%1)\n\t"
        "lui $8, 0xAABB\n\tori $8, $8, 0xCCDD\n\tdsll $8, $8, 16\n\tori $8, $8, 0xABCD\n\tdsll $8, $8, 16\n\tori $8, $8, 0xEFED\n\t"
        "scd $8, 128+0(%1)\n\tswc1 $f0, 144+4(%1)\n\tsdc1 $f0, 160+0(%1)\n\t"
        ".set at\n\t.set reorder" :: "r"(c), "r"(u) : "$8", "memory");
    /* the loads, one batch per view, kept in memory at 0x00302000 + batch * 0x60 */
#define BATCH(base, out) \
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t.set hardfloat\n\t" \
        "lb $9, 3(%0)\n\tlbu $10, 16+1(%0)\n\tlh $11, 32+2(%0)\n\tlhu $12, 48+0(%0)\n\tlw $13, 64+4(%0)\n\tlwu $14, 80+0(%0)\n\tld $15, 96+0(%0)\n\t" \
        "ll $16, 112+4(%0)\n\tlld $17, 128+0(%0)\n\tdmtc1 $0, $f0\n\tlwc1 $f0, 144+4(%0)\n\tldc1 $f2, 160+0(%0)\n\t" \
        "sd $9, 0(%1)\n\tsd $10, 8(%1)\n\tsd $11, 16(%1)\n\tsd $12, 24(%1)\n\tsd $13, 32(%1)\n\tsd $14, 40(%1)\n\tsd $15, 48(%1)\n\tsd $16, 56(%1)\n\tsd $17, 64(%1)\n\t" \
        "sdc1 $f0, 72(%1)\n\tsdc1 $f2, 80(%1)\n\t.set at\n\t.set reorder" :: "r"(base), "r"(out) : "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$16", "$17", "memory")
    volatile u32 *o = K1(0x00302000);
    BATCH(c, o); BATCH(u, o + 24);
    CACHE(1, 0, c); CACHE(1, 16, c); CACHE(1, 32, c); CACHE(1, 48, c); CACHE(1, 64, c); CACHE(1, 80, c);
    CACHE(1, 96, c); CACHE(1, 112, c); CACHE(1, 128, c); CACHE(1, 144, c); CACHE(1, 160, c);
    BATCH(c, o + 48); BATCH(u, o + 72);
    static const u64 cv[11] = { 0x77, 0x77, 0x6677, 0x6677, 0x44556677ull, 0x44556677ull, 0x0011223344556677ull, 0x44556677ull, 0x0011223344556677ull, 0x44556677ull, 0x0011223344556677ull };
    static const u64 uv[11] = { 0xFFFFFFFFFFFFFFEDull, 0xED, 0xFFFFFFFFFFFFEFEDull, 0xEFED, 0xFFFFFFFFABCDEFEDull, 0xABCDEFED, 0xAABBCCDDABCDEFEDull, 0xFFFFFFFFABCDEFEDull, 0xAABBCCDDABCDEFEDull, 0xABCDEFED, 0xAABBCCDDABCDEFEDull };
    for (int b = 0; b < 4; b++)
        for (int k = 0; k < 11; k++) {
            volatile u32 *p = o + 24 * b + 2 * k;
            u64 got = ((u64)p[0] << 32) | p[1];
            tcheck(b * 16 + k, got, (b == 1) ? uv[k] : cv[k]);
        }
}

/* ---- 410-423: a cached store is not in memory until the line is written back */
#define WBTEST(idx, INSTR) do { cur = idx; volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *w = K1(0x00303000); \
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t" \
        "sw $0, 0(%1)\n\tsw $0, 4(%1)\n\tsw $0, 8(%1)\n\tsw $0, 12(%1)\n\tsw $0, 16(%1)\n\t" \
        "ori $8, $0, 0x1234\n\tsw $8, 0(%0)\n\tori $8, $0, 0x2345\n\tsw $8, 4(%0)\n\tori $8, $0, 0x3456\n\tsw $8, 8(%0)\n\t" \
        "ori $8, $0, 0x4567\n\tsw $8, 12(%0)\n\tori $8, $0, 0x5678\n\tsw $8, 16(%0)\n\t" \
        "lw $9, 0(%1)\n\tlw $10, 4(%1)\n\tlw $11, 8(%1)\n\tlw $12, 12(%1)\n\tlw $13, 16(%1)\n\t" \
        INSTR "\n\t" \
        "lw $14, 0(%1)\n\tlw $15, 4(%1)\n\tlw $16, 8(%1)\n\tlw $17, 12(%1)\n\tlw $18, 16(%1)\n\t" \
        "sw $9, 0(%2)\n\tsw $10, 4(%2)\n\tsw $11, 8(%2)\n\tsw $12, 12(%2)\n\tsw $13, 16(%2)\n\t" \
        "sw $14, 20(%2)\n\tsw $15, 24(%2)\n\tsw $16, 28(%2)\n\tsw $17, 32(%2)\n\tsw $18, 36(%2)\n\t.set at\n\t.set reorder" \
        :: "r"(c), "r"(u), "r"(w) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$16", "$17", "$18", "memory"); \
    for (int i = 0; i < 5; i++) tcheck(i, w[i], 0); \
    tcheck(5, w[5], 0x1234); tcheck(6, w[6], 0x2345); tcheck(7, w[7], 0x3456); tcheck(8, w[8], 0x4567); tcheck(9, w[9], 0); \
    } while (0)
static void t410_423(void)
{
    WBTEST(410, "lb $0, 8207(%0)");
    WBTEST(411, "lbu $0, 8207(%0)");
    WBTEST(412, "lh $0, 8206(%0)");
    WBTEST(413, "lhu $0, 8206(%0)");
    WBTEST(414, "lw $0, 8204(%0)");
    WBTEST(415, "lwu $0, 8204(%0)");
    WBTEST(416, "ld $0, 8200(%0)");
    WBTEST(417, "sb $0, 8207(%0)");
    WBTEST(418, "sh $0, 8206(%0)");
    WBTEST(419, "sw $0, 8204(%0)");
    WBTEST(420, "sd $0, 8200(%0)");
    WBTEST(421, "cache 1, 12(%0)");
    WBTEST(422, "cache 1, 12(%1)");
    WBTEST(423, "cache 1, 8200(%0)");
}

/* ---- 424: Index Load Tag */
static void t424(void)
{
    cur = 424;
    static const u32 offs[] = { 0, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 3072, 4096, 5120, 6144, 7168, 8192, 8196, 9216, 13312 };
    for (u32 k = 0; k < sizeof offs / sizeof offs[0]; k++) {
        u32 off = offs[k];
        volatile u32 *p = (volatile u32 *)(0x80000000u | (DATA + off));
        MTC0(28, 0xFFFFFFFFu);
        u32 ptag = (DATA + off) >> 12;
        (void)*p;
        __asm__ volatile("cache %0, 0(%1)" :: "i"(D_HWBI), "r"(p) : "memory");
        u32 t1 = LTAG(D_ILT, 0, p);
        tcheck(k * 4 + 0, t1, (ptag << 8) | (ST_INV << 6));
        (void)*p;
        u32 t2 = LTAG(D_ILT, 0, p);
        tcheck(k * 4 + 1, t2, (ptag << 8) | (ST_DIRTY << 6));
        *p = 0;
        u32 t3 = LTAG(D_ILT, 0, p);
        tcheck(k * 4 + 2, t3, (ptag << 8) | (ST_DIRTY << 6));
    }
}

/* ---- 425-429, 438?: the six lines test. R keeps the results: tag[6], r0[6], r1[6], r2[6] at 0x00303100 */
#define CITEST(idx, OP) do { cur = idx; volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *r = K1(0x00303100); \
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t" \
        "cache 1, 0(%0)\n\tcache 1, 16(%0)\n\tcache 1, 32(%0)\n\tcache 1, 48(%0)\n\tcache 1, 64(%0)\n\tcache 1, 80(%0)\n\t" \
        "sw $0, 0(%1)\n\tsw $0, 16(%1)\n\tsw $0, 32(%1)\n\tsw $0, 48(%1)\n\tsw $0, 64(%1)\n\tsw $0, 80(%1)\n\t" \
        "sw $0, 8192+0(%1)\n\tsw $0, 8192+16(%1)\n\tsw $0, 8192+32(%1)\n\tsw $0, 8192+48(%1)\n\tsw $0, 8192+64(%1)\n\tsw $0, 8192+80(%1)\n\t" \
        "ori $8, $0, 0x222\n\tsw $8, 0(%0)\n\tlw $8, 16(%0)\n\tsw $8, 48(%0)\n\tlw $8, 64(%0)\n\t" \
        "ori $8, $0, 0x111\n\tsw $8, 0(%1)\n\tsw $8, 16(%1)\n\tsw $8, 32(%1)\n\tsw $8, 48(%1)\n\tsw $8, 64(%1)\n\tsw $8, 80(%1)\n\t" \
        "cache " #OP ", 0(%0)\n\tcache " #OP ", 16(%0)\n\tcache " #OP ", 32(%0)\n\tcache " #OP ", 8192+48(%0)\n\tcache " #OP ", 8192+64(%0)\n\tcache " #OP ", 8192+80(%0)\n\t" \
        "cache 5, 0(%0)\n\tnop\n\tmfc0 $9, $28\n\tcache 5, 16(%0)\n\tnop\n\tmfc0 $10, $28\n\tcache 5, 32(%0)\n\tnop\n\tmfc0 $11, $28\n\t" \
        "cache 5, 48(%0)\n\tnop\n\tmfc0 $12, $28\n\tcache 5, 64(%0)\n\tnop\n\tmfc0 $13, $28\n\tcache 5, 80(%0)\n\tnop\n\tmfc0 $14, $28\n\t" \
        "sw $9, 0(%2)\n\tsw $10, 4(%2)\n\tsw $11, 8(%2)\n\tsw $12, 12(%2)\n\tsw $13, 16(%2)\n\tsw $14, 20(%2)\n\t" \
        "lw $9, 0(%1)\n\tlw $10, 16(%1)\n\tlw $11, 32(%1)\n\tlw $12, 48(%1)\n\tlw $13, 64(%1)\n\tlw $14, 80(%1)\n\t" \
        "sw $9, 24(%2)\n\tsw $10, 28(%2)\n\tsw $11, 32(%2)\n\tsw $12, 36(%2)\n\tsw $13, 40(%2)\n\tsw $14, 44(%2)\n\t" \
        "ori $8, $0, 1\n\tsw $8, 0(%1)\n\tsw $8, 16(%1)\n\tsw $8, 32(%1)\n\tsw $8, 48(%1)\n\tsw $8, 64(%1)\n\tsw $8, 80(%1)\n\t" \
        "sw $8, 8192+0(%1)\n\tsw $8, 8192+16(%1)\n\tsw $8, 8192+32(%1)\n\tsw $8, 8192+48(%1)\n\tsw $8, 8192+64(%1)\n\tsw $8, 8192+80(%1)\n\t" \
        "lw $0, 16384+0(%0)\n\tlw $0, 16384+16(%0)\n\tlw $0, 16384+32(%0)\n\tlw $0, 16384+48(%0)\n\tlw $0, 16384+64(%0)\n\tlw $0, 16384+80(%0)\n\t" \
        "lw $9, 0(%1)\n\tlw $10, 16(%1)\n\tlw $11, 32(%1)\n\tlw $12, 48(%1)\n\tlw $13, 64(%1)\n\tlw $14, 80(%1)\n\t" \
        "sw $9, 48(%2)\n\tsw $10, 52(%2)\n\tsw $11, 56(%2)\n\tsw $12, 60(%2)\n\tsw $13, 64(%2)\n\tsw $14, 68(%2)\n\t" \
        "lw $9, 8192+0(%1)\n\tlw $10, 8192+16(%1)\n\tlw $11, 8192+32(%1)\n\tlw $12, 8192+48(%1)\n\tlw $13, 8192+64(%1)\n\tlw $14, 8192+80(%1)\n\t" \
        "sw $9, 72(%2)\n\tsw $10, 76(%2)\n\tsw $11, 80(%2)\n\tsw $12, 84(%2)\n\tsw $13, 88(%2)\n\tsw $14, 92(%2)\n\t.set at\n\t.set reorder" \
        :: "r"(c), "r"(u), "r"(r) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "memory"); } while (0)
static void citest_check(u32 idx, const u32 *tag, u32 ntag, const u32 *r0, const u32 *r1, const u32 *r2)
{
    volatile u32 *r = K1(0x00303100);
    u32 base = DATA >> 4;
    cur = idx;
    for (u32 i = 0; i < ntag; i++) tcheck(i, r[i], tag[i] + base);
    for (u32 i = 0; i < 6; i++) tcheck(8 + i, r[6 + i], r0[i]);
    for (u32 i = 0; i < 6; i++) tcheck(16 + i, r[12 + i], r1[i]);
    for (u32 i = 0; i < 6; i++) tcheck(24 + i, r[18 + i], r2[i]);
}
static void t425_429(void)
{
    static const u32 ones[6] = { 1, 1, 1, 1, 1, 1 };
    { CITEST(425, 0x15); static const u32 tg[5] = { 0, 0, 0, 0xc0, 0xc0 }, r0[6] = { 0x222, 0x111, 0x111, 0x111, 0x111, 0x111 }, r1[6] = { 1, 1, 1, 0, 1, 1 };
      citest_check(425, tg, 5, r0, r1, ones); }
    { CITEST(426, 0x19); static const u32 tg[6] = { 0xc0, 0xc0, 0, 0xc0, 0xc0, 0 }, r0[6] = { 0x222, 0x111, 0x111, 0x111, 0x111, 0x111 }, r1[6] = { 1, 1, 1, 0, 1, 1 };
      citest_check(426, tg, 6, r0, r1, ones); }
    { CITEST(427, 0x11); static const u32 tg[6] = { 0, 0, 0, 0xc0, 0xc0, 0 }, r0[6] = { 0x111, 0x111, 0x111, 0x111, 0x111, 0x111 }, r1[6] = { 1, 1, 1, 0, 1, 1 };
      citest_check(427, tg, 6, r0, r1, ones); }
    { CITEST(428, 0x01); static const u32 tg[6] = { 0, 0, 0, 0x200, 0x200, 0 }, r0[6] = { 0x222, 0x111, 0x111, 0, 0x111, 0x111 };
      citest_check(428, tg, 6, r0, ones, ones); }
    { CITEST(429, 0x0D); static const u32 tg[6] = { 0xc0, 0xc0, 0xc0, 0x2c0, 0x2c0, 0x2c0 }, r0[6] = { 0x111, 0x111, 0x111, 0, 0x111, 0x111 }, r1[6] = { 0x222, 1, 1, 1, 1, 1 };
      citest_check(429, tg, 6, r0, r1, ones); }
}

/* ---- 430-433: a line written by hand */
static void manual(u32 idx, u32 st)
{
    cur = idx;
    volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *r = K1(0x00303200);
    MTC0(28, TAGLO(DATA, st));
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t"
        "ori $8, $0, 1\n\tori $9, $0, 2\n\t"
        "sw $8, 0(%0)\n\tsw $9, 0(%1)\n\tcache 9, 0(%0)\n\tlw $10, 0(%0)\n\tlw $11, 0(%1)\n\t"
        "sw $8, 16(%0)\n\tsw $9, 16(%1)\n\tcache 9, 16(%0)\n\tcache 0x19, 16(%0)\n\tlw $12, 16(%0)\n\tlw $13, 16(%1)\n\t"
        "sw $8, 32(%0)\n\tcache 0x15, 32(%0)\n\tlw $0, 32(%0)\n\tsw $9, 32(%1)\n\tcache 9, 32(%0)\n\tcache 0x19, 32(%0)\n\tlw $14, 32(%0)\n\tlw $15, 32(%1)\n\t"
        "sw $10, 0(%2)\n\tsw $11, 4(%2)\n\tsw $12, 8(%2)\n\tsw $13, 12(%2)\n\tsw $14, 16(%2)\n\tsw $15, 20(%2)\n\t.set at\n\t.set reorder"
        :: "r"(c), "r"(u), "r"(r) : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "memory");
    if (st == ST_INV || st == ST_01) { for (int i = 0; i < 6; i++) tcheck(i, r[i], 2); }
    else { tcheck(0, r[0], 1); tcheck(1, r[1], 2); tcheck(2, r[2], 1); tcheck(3, r[3], 1); tcheck(4, r[4], 1); tcheck(5, r[5], 2); }
}

/* ---- 434: a dirty line moved +8 KiB by Index Store Tag */
static void t434(void)
{
    cur = 434;
    volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *r = K1(0x00303300);
    u32 tdirty = TAGLO(DATA + 8192, ST_DIRTY), tinv = TAGLO(DATA + 8192, ST_INV);
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t"
        "sd $0, 8192+0(%1)\n\tsd $0, 8192+8(%1)\n\tsd $0, 8192+16(%1)\n\tsd $0, 8192+32(%1)\n\tsd $0, 8192+48(%1)\n\tsd $0, 8192+64(%1)\n\t"
        "cache 1, 0(%0)\n\tcache 1, 16(%0)\n\tcache 1, 32(%0)\n\t"
        "lui $8, 0x0123\n\tori $8, $8, 0x4567\n\tsw $8, 0(%1)\n\tsw $8, 16(%1)\n\tsw $8, 32(%1)\n\t"
        "ori $8, $0, 0x10\n\tsb $8, 5(%0)\n\tsb $8, 5+16(%0)\n\tlb $8, 5+32(%0)\n\t"
        "mtc0 %3, $28\n\tnop\n\tnop\n\tcache 9, 16(%0)\n\t"
        "mtc0 %4, $28\n\tnop\n\tnop\n\tcache 9, 0(%0)\n\tcache 9, 16(%0)\n\tcache 9, 32(%0)\n\t"
        "lb $8, 4(%0)\n\tlb $8, 4+16(%0)\n\tlb $8, 4+32(%0)\n\t"
        "lw $9, 8192+0(%1)\n\tlw $10, 8192+16(%1)\n\tlw $11, 8192+32(%1)\n\t"
        "sw $9, 0(%2)\n\tsw $10, 4(%2)\n\tsw $11, 8(%2)\n\t.set at\n\t.set reorder"
        :: "r"(c), "r"(u), "r"(r), "r"(tinv), "r"(tdirty) : "$8", "$9", "$10", "$11", "memory");
    tcheck(0, r[0], 0x01234567); tcheck(1, r[1], 0x01234567); tcheck(2, r[2], 0);
}

/* ---- 435: all the bits of Index Store Tag */
static void t435(void)
{
    cur = 435;
    volatile u32 *c = K0(DATA);
    CACHE(1, 0, c); CACHE(1, 16, c); CACHE(1, 32, c);
    MTC0(28, 0xFFFFFFFFu);
    CACHE(D_IST, 0, c);
    u32 t = LTAG(D_ILT, 0, c);
    MTC0(28, 0);
    CACHE(D_IST, 0, c);
    tcheck(0, t, 0x0FFFFFC0u);
}

/* ---- 436: a line made dirty by Index Store Tag is not written back */
static void t436(void)
{
    cur = 436;
    volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *r = K1(0x00303400);
    u32 td = TAGLO(DATA, ST_DIRTY);
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t"
        "cache 1, 0(%0)\n\tsw %5, 0(%1)\n\tlw $0, 0(%0)\n\tsw %6, 0(%0)\n\tcache 1, 0(%0)\n\tlw $9, 0(%1)\n\t"
        "cache 1, 16(%0)\n\tsw %7, 16(%1)\n\tlw $0, 16(%0)\n\tsw %8, 16(%1)\n\tmtc0 %3, $28\n\tnop\n\tnop\n\tcache 9, 16(%0)\n\tcache 1, 16(%0)\n\tlw $10, 16(%1)\n\t"
        "sw $9, 0(%2)\n\tsw $10, 4(%2)\n\t.set at\n\t.set reorder"
        :: "r"(c), "r"(u), "r"(r), "r"(td), "r"(0), "r"(0xAAAAAAAAu), "r"(0x11112222u), "r"(0xBBBBBBBBu), "r"(0x33334444u) : "$9", "$10", "memory");
    tcheck(0, r[0], 0x11112222u); tcheck(1, r[1], 0x33334444u);
}

/* ---- 437-439: the dirty flag outlives the line's validity */
#define KEEP(idx, OP) do { cur = idx; volatile u32 *c = K0(DATA), *u = K1(DATA); volatile u32 *r = K1(0x00303500); \
    u32 td = TAGLO(DATA, ST_DIRTY), ti = TAGLO(DATA, ST_INV); \
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t" \
        "sw $0, 8192+0(%0)\n\tsw $0, 8192+16(%0)\n\tsw $0, 8192+32(%0)\n\tsw $0, 8192+48(%0)\n\t" \
        "lui $8, 0x0123\n\tori $8, $8, 0x4567\n\t" \
        "mtc0 %4, $28\n\tlw $0, 0(%0)\n\tsw $8, 16(%0)\n\tlw $0, 32(%0)\n\tsw $8, 48(%0)\n\tcache 9, 32(%0)\n\tcache 9, 48(%0)\n\t" \
        "cache " #OP ", 0(%0)\n\tcache " #OP ", 16(%0)\n\tcache " #OP ", 32(%0)\n\tcache " #OP ", 48(%0)\n\t" \
        "sw $0, 0(%1)\n\tsw $0, 16(%1)\n\tsw $0, 32(%1)\n\tsw $0, 48(%1)\n\t" \
        "mtc0 %3, $28\n\tnop\n\tnop\n\tcache 9, 0(%0)\n\tcache 9, 16(%0)\n\tcache 9, 32(%0)\n\tcache 9, 48(%0)\n\t" \
        "lw $0, 8192+0(%0)\n\tlw $0, 8192+16(%0)\n\tlw $0, 8192+32(%0)\n\tlw $0, 8192+48(%0)\n\t" \
        "lw $9, 0(%0)\n\tlw $10, 16(%0)\n\tlw $11, 32(%0)\n\tlw $12, 48(%0)\n\t" \
        "sw $9, 0(%2)\n\tsw $10, 4(%2)\n\tsw $11, 8(%2)\n\tsw $12, 12(%2)\n\t.set at\n\t.set reorder" \
        :: "r"(c), "r"(u), "r"(r), "r"(td), "r"(ti) : "$8", "$9", "$10", "$11", "$12", "memory"); \
    tcheck(0, r[0], 0); tcheck(1, r[1], 0x01234567); tcheck(2, r[2], 0); tcheck(3, r[3], 0x01234567); } while (0)

/* ---- the instruction cache.  GEN is the generator's memory (16 KiB-aligned, 8 KiB), GENERATED the code the test runs: word 100 */
#define GEN 0x00340000u
#define GENERATED 100
#define LUI(rt, i)      (0x3C000000u | ((rt) << 16) | ((i) & 0xFFFF))
#define ORI(rt, rs, i)  (0x34000000u | ((rs) << 21) | ((rt) << 16) | ((i) & 0xFFFF))
#define SW(rt, off, rs) (0xAC000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFF))
#define LW(rt, off, rs) (0x8C000000u | ((rs) << 21) | ((rt) << 16) | ((off) & 0xFFFF))
#define JR(rs)          (0x00000008u | ((rs) << 21))
#define JALR(rd, rs)    (0x00000009u | ((rs) << 21) | ((rd) << 11))
#define BGEZAL(rs, off) (0x04110000u | ((rs) << 21) | ((off) & 0xFFFF))
#define MFC0I(rt, rd)   (0x40000000u | ((rt) << 16) | ((rd) << 11))
#define CACHEI(op, off, rs) (0xBC000000u | ((rs) << 21) | ((op) << 16) | ((off) & 0xFFFF))
#define NOP 0u
#define A0 4
#define A1 5
#define A2 6
#define V1 3
#define T0 8
#define T1 9
#define T2 10
#define T3 11
#define T4 12
#define T5 13
#define T6 14
#define T7 15
#define S0 16
#define RA 31
/* (a test of the instruction cache starts with an empty one: its memory is new in the console's test) */
static void icache_flush(void)
{
    for (u32 a = 0; a < 16384; a += 32) CACHE(I_II, 0, 0x80000000u + a);
}
static u32 gw;                                  /* the generator's write index (words) */
static void emit(u32 w) { K1(GEN)[gw++] = w; }
static void emit_write(int use_dc, int off, u32 value)
{
    emit(LUI(A1, value >> 16)); emit(ORI(A1, A1, value & 0xFFFF)); emit(SW(A1, off << 2, use_dc ? A0 : V1));
}
/* run the generator at GEN (cached) with $3 = the uncached GENERATED, $4 = the cached; the results are T0..T7, S0 at the end ($8..$16) -> regs[] */
static void run_gen(volatile u32 *regs)
{
    u32 g = 0x80000000u | GEN, gu = 0xA0000000u | (GEN + (GENERATED << 2)), gc = 0x80000000u | (GEN + (GENERATED << 2));
        __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t"
        "move $3, %1\n\tmove $4, %2\n\t"
        "daddiu $29, $29, -16\n\tsd $31, 0($29)\n\tjalr %0\n\tnop\n\tld $31, 0($29)\n\tdaddiu $29, $29, 16\n\t"
        "sw $9, 0(%3)\n\tsw $10, 4(%3)\n\tsw $11, 8(%3)\n\tsw $12, 12(%3)\n\tsw $13, 16(%3)\n\tsw $14, 20(%3)\n\tsw $15, 24(%3)\n\tsw $16, 28(%3)\n\t.set at\n\t.set reorder"
        :: "r"(g), "r"(gu), "r"(gc), "r"(regs) : "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$16", "memory");
}
static void t441_442(int second)
{
    cur = second ? 442 : 441;
    icache_flush();
    volatile u32 *regs = K1(0x00303600);
    gw = 0;
    for (u32 i = 0; i < 1024 + 4; i++) K1(GEN)[i] = 0;
    K1(GEN)[GENERATED + 4096] = JR(RA); K1(GEN)[GENERATED + 4097] = NOP;
    /* the cached memory for the instruction cache and the data cache aliases */
    emit(ORI(A2, RA, 0));
    emit_write(0, 0, LUI(T0, 0x1111));
    emit_write(0, 1, ORI(T0, T0, 0x1111));
    emit_write(0, 2, JR(RA));
    emit_write(0, 3, NOP);
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(T1, T0, 0));
    if (!second) emit_write(0, 0, LUI(T0, 0x2222)); else emit_write(0, 1, ORI(T0, T0, 0x2222));
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(T2, T0, 0));
    emit(BGEZAL(0, GENERATED + 4096 - gw - 1)); emit(NOP); emit(JALR(RA, A0)); emit(NOP); emit(ORI(T3, T0, 0));
    if (!second) emit_write(1, 0, LUI(T0, 0x3333)); else emit_write(1, 1, ORI(T0, T0, 0x3333));
    emit(BGEZAL(0, GENERATED + 4096 - gw - 1)); emit(NOP); emit(JALR(RA, A0)); emit(NOP); emit(ORI(T4, T0, 0));
    emit(LW(0, 8192, A0)); emit(NOP);
    emit(BGEZAL(0, GENERATED + 4096 - gw - 1)); emit(NOP); emit(JALR(RA, A0)); emit(NOP); emit(ORI(T5, T0, 0));
    emit(ORI(RA, A2, 0)); emit(JR(RA)); emit(NOP);
    run_gen(regs);
    /* regs: $9..$13 = T1..T5 = results 1..5 */
    static const u32 w1[5] = { 0x11111111u, 0x11111111u, 0x22221111u, 0x22221111u, 0x33331111u };
    static const u32 w2[5] = { 0x11111111u, 0x11111111u, 0x11112222u, 0x11112222u, 0x11113333u };
    for (int i = 0; i < 5; i++) tcheck(i, regs[i], second ? w2[i] : w1[i]);
}

/* ---- 457-460: the instruction cache's tags */
static void t_invalidate(u32 idx, u32 op, u32 want_after, u32 want_mis1, u32 want_mis2, u32 want_res4)
{
    cur = idx;
    icache_flush();
    volatile u32 *regs = K1(0x00303700);
    u32 ptag = (GEN + (GENERATED << 2)) >> 12, ptag_next = (GEN + (GENERATED << 2) + 16384) >> 12;
    (void)ptag_next;
    gw = 0;
    for (u32 i = 0; i < 128; i++) K1(GEN)[i] = 0;
    emit(ORI(A2, RA, 0));
    emit_write(0, 0, LUI(T0, 0x1111));
    emit_write(0, 1, ORI(T0, T0, 0x1111));
    emit_write(0, 2, JR(RA));
    emit_write(0, 3, NOP);
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(T1, T0, 0));                       /* T1 = result1 */
    emit_write(0, 1, ORI(T0, T0, 0x2222));
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(T2, T0, 0));                       /* T2 = result2 */
    emit(CACHEI(I_ILT, 0, V1)); emit(NOP); emit(NOP); emit(MFC0I(T4, 28)); emit(NOP); emit(NOP);
    emit(CACHEI(op, 0, V1));
    emit(CACHEI(I_ILT, 0, V1)); emit(NOP); emit(NOP); emit(MFC0I(T5, 28)); emit(NOP); emit(NOP);
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(T3, T0, 0));                       /* T3 = result3 */
    emit(CACHEI(op, 16384, V1));
    emit(CACHEI(I_ILT, 0, V1)); emit(NOP); emit(NOP); emit(MFC0I(T6, 28)); emit(NOP); emit(NOP);
    emit_write(0, 1, ORI(T0, T0, 0x3333));
    emit(CACHEI(op, 0, V1));
    emit(CACHEI(I_ILT, 0, V1)); emit(NOP); emit(NOP); emit(MFC0I(T7, 28)); emit(NOP); emit(NOP);
    emit_write(0, 1, ORI(T0, T0, 0x4444));
    emit(JALR(RA, A0)); emit(NOP); emit(ORI(S0, T0, 0));
    emit(ORI(RA, A2, 0)); emit(JR(RA)); emit(NOP);
    /* the cache lines: the memory for the code is 16 KiB-aligned so that +16384 is the same index */
    u32 g = 0x80000000u | GEN, gu = 0xA0000000u | (GEN + (GENERATED << 2)), gc = 0x80000000u | (GEN + (GENERATED << 2));
    __asm__ volatile(".set noat\n\t.set noreorder\n\t.set gp=64\n\t"
        "move $3, %1\n\tmove $4, %2\n\t"
        "daddiu $29, $29, -16\n\tsd $31, 0($29)\n\tjalr %0\n\tnop\n\tld $31, 0($29)\n\tdaddiu $29, $29, 16\n\t"
        "sw $9, 0(%3)\n\tsw $10, 4(%3)\n\tsw $11, 8(%3)\n\tsw $12, 12(%3)\n\tsw $13, 16(%3)\n\tsw $14, 20(%3)\n\tsw $15, 24(%3)\n\tsw $16, 28(%3)\n\t.set at\n\t.set reorder"
        :: "r"(g), "r"(gu), "r"(gc), "r"(regs) : "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$16", "memory");
    /* regs: [0]=$9 T1 result1, [1]=$10 T2 result2, [2]=$11 T3 result3, [3]=$12 T4 tag before, [4]=$13 T5 tag after, [5]=$14 T6 mismatch 1, [6]=$15 T7 mismatch 2, [7]=$16 S0 result 4 */
    tcheck(0, regs[0], 0x11111111u);
    tcheck(1, regs[1], 0x11111111u);
    tcheck(2, regs[3], (ptag << 8) | (ST_CLEAN << 6));
    tcheck(3, regs[2], 0x11112222u);
    tcheck(4, regs[4], want_after);
    tcheck(5, regs[5], want_mis1);
    tcheck(6, regs[6], want_mis2);
    tcheck(7, regs[7], want_res4);
}
static void t457_459(void)
{
    u32 pt = (GEN + (GENERATED << 2)) >> 12, ptn = (GEN + (GENERATED << 2) + 16384) >> 12;
    t_invalidate(457, I_II, (pt << 8) | (ST_INV << 6), (ptn << 8) | (ST_INV << 6), (pt << 8) | (ST_INV << 6), 0x11114444u);
    t_invalidate(458, I_HI, (pt << 8) | (ST_INV << 6), (pt << 8) | (ST_CLEAN << 6), (pt << 8) | (ST_INV << 6), 0x11114444u);
    t_invalidate(459, I_FILL, (pt << 8) | (ST_CLEAN << 6), (ptn << 8) | (ST_CLEAN << 6), (pt << 8) | (ST_CLEAN << 6), 0x11113333u);
}
static void t460(void)
{
    cur = 460;
    volatile u32 *regs = K1(0x00303800);
    u32 pt = (GEN + (GENERATED << 2)) >> 12, ptn = (GEN + (GENERATED << 2) + 16384) >> 12;
    u32 v[4] = { (pt << 8) | (ST_INV << 6), (ptn << 8) | (ST_01 << 6), (pt << 8) | (ST_CLEAN << 6), (ptn << 8) | (ST_DIRTY << 6) };
    u32 base = 0xA0000000u | (GEN + (GENERATED << 2));
    for (int i = 0; i < 4; i++) {
        MTC0(28, v[i]);
        CACHE(I_IST, 0, base);
        regs[i] = LTAG(I_ILT, 16384, base);
    }
    tcheck(0, regs[0], (pt << 8) | (ST_INV << 6));
    tcheck(1, regs[1], (ptn << 8) | (ST_INV << 6));
    tcheck(2, regs[2], (pt << 8) | (ST_CLEAN << 6));
    tcheck(3, regs[3], (ptn << 8) | (ST_CLEAN << 6));
}

#ifdef SOC
static void isv_text(const char *t)
{
    volatile u32 *buf = (volatile u32 *)0xB3FF0020u;
    u32 n = 0, w = 0;
    for (; t[n]; n++) {
        w |= (u32)(unsigned char)t[n] << (24 - 8 * (n & 3));
        if ((n & 3) == 3) { buf[n >> 2] = w; w = 0; }
    }
    if (n & 3) buf[n >> 2] = w;
    *(volatile u32 *)0xB3FF0014u = n;
}
static void isv_hex(u32 v)
{
    char b[9]; for (int i = 0; i < 8; i++) b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[8] = 0;
    isv_text(b);
}
#endif
int main(void)
{
#ifdef SOC
    MTC0(12, MFC0(12) | 0x30000000u);                  /* (CU1 and FR) */
#endif
    for (u32 v = 0; v < 0x200; v += 0x180) {
        u32 a = (u32)(void *)h_entry;
        *K1(v) = 0x3C1A0000u | (a >> 16);
        *K1(v + 4) = 0x375A0000u | (a & 0xFFFF);
        *K1(v + 8) = 0x03400008u;
        *K1(v + 12) = 0;
    }
    for (u32 i = 0; i < 0x200; i++) *K1(0x000F2000 + 4 * i) = 0;
#ifndef SEL
#define SEL 0xFFFF
#endif
    if (SEL & 1) t409();
    if (SEL & 2) t410_423();
    if (SEL & 4) { t424(); t425_429(); }
    if (SEL & 8) { manual(430, ST_INV); manual(431, ST_CLEAN); manual(432, ST_DIRTY); manual(433, ST_01); t434(); t435(); t436(); }
    if (SEL & 16) { KEEP(437, 0x11); KEEP(438, 0x15); KEEP(439, 0x01); }
    if (SEL & 32) { t441_442(0); t441_442(1); }
    if (SEL & 64) { t457_459(); t460(); }
    *K1(0x000F0F04) = nfail;
    *K1(0x000F0F00) = nfail ? 0xBAD00000u | nfail : 0x600DC0DEu;
#ifdef SOC
    /* in the SoC simulation (n64rt's ISV=1 runtime): the results as the IS-Viewer's text; the failed checks of the table */
    isv_text("cache_rom: ");
    isv_hex(nfail); isv_text(" checks failed\n");
    for (u32 i = 0; i < nfail && i < 32; i++) {
        volatile u32 *e = K1(0x000F0F08 + 20 * i);
        isv_text("c "); isv_hex(e[0]); isv_text(" got "); isv_hex(e[1]); isv_hex(e[2]); isv_text(" want "); isv_hex(e[3]); isv_hex(e[4]); isv_text("\n");
    }
    isv_text("cache_rom: done\n");
#endif
    for (;;) ;
}
