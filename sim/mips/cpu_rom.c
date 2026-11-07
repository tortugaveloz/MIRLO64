/* A directed CPU program with checks of its own, for the lockstep bench (sim/mips/build.sh; tb -x rom.z64). Where tlb_rom.c only
 * reaches cases and leaves the verdict to the model, this program knows what each one must give -- the values the VR4300 gives,
 * as a conformance suite for the CPU found them -- and reports every check that differs at 0x000F0F00 (tb -x prints them). The
 * model (sim/n64) and the RTL must also agree on every retired instruction, as ever.
 *
 * Each section is one group of behaviours; `tlb_rom.sh OUT.z64 cpu_rom.c` builds it.
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_) :: "memory"); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)) : "memory")
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))

volatile u32 h_cause, h_epc, h_badv, h_count, h_ret, h_status;
volatile u32 sink;

/* ---- the checks: id, the value got, the value expected (64 bits) */
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
/* the handler: what the exception left in the registers, then on past the instruction (h_ret: to there instead; a delay slot's
 * branch and the slot are both stepped over) */
"    .globl h_entry\n"
"h_entry:\n"
"    mfc0  $k0, $13\n"
"    lui   $k1, %hi(h_cause)\n"
"    sw    $k0, %lo(h_cause)($k1)\n"
"    mfc0  $k0, $14\n"
"    sw    $k0, %lo(h_epc)($k1)\n"
"    mfc0  $k0, $8\n"
"    sw    $k0, %lo(h_badv)($k1)\n"
"    mfc0  $k0, $12\n"
"    sw    $k0, %lo(h_status)($k1)\n"
"    lw    $k0, %lo(h_count)($k1)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, %lo(h_count)($k1)\n"
"    lw    $k0, %lo(h_cause)($k1)\n"
"    lw    $k1, %lo(h_ret)($k1)\n"
"    bnez  $k1, 2f\n"
"    nop\n"
"    mfc0  $k1, $14\n"
"    bgez  $k0, 3f\n"                    /* (Cause.BD clear) */
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


/* ---- COP1 control registers */
static void sec_fcr(void)
{
    u32 v;
    __asm__ volatile("cfc1 %0, $0\n\tnop\n\tnop" : "=r"(v));
    check(0x0301, v, 0xA00);                                           /* the console's FPU revision */
    __asm__ volatile("ctc1 %0, $0\n\tnop\n\tnop" :: "r"(0x123u));
    __asm__ volatile("cfc1 %0, $0\n\tnop\n\tnop" : "=r"(v));
    check(0x0302, v, 0xA00);                                           /* (not writable) */
    __asm__ volatile("cfc1 %0, $1\n\tnop\n\tnop" : "=r"(v));
    check(0x0303, v, 0);
}

/* ---- Index and Status: what is writable */
static void sec_cop0_masks(void)
{
    u32 prev = MFC0(0);
    MTC0(0, 0xFFFFFFFFu);
    check(0x0401, MFC0(0), 0x8000003F);                                /* P is writable, with the six bits of the index */
    MTC0(0, 0);
    check(0x0402, MFC0(0), 0);
    MTC0(0, prev);

    u32 st = MFC0(12);
    MTC0(12, st | 0x00080000u);                                        /* bit 19 reads 0 */
    check(0x0411, MFC0(12), st & ~0x00080000u);
    MTC0(12, st);
    check(0x0412, MFC0(12), st);
}

/* ---- COP0 CO instructions of an undefined function 0x20 - 0x3F do nothing (no Reserved Instruction), whatever their other fields: a
 * test suite runs three of them, an emulator's detection and log calls, before it has a handler. The defined ones' neighbours stay RI. */
static void sec_cop0_co(void)
{
    u32 r8 = 0x12345678u;
    h_count = 0; h_ret = 0;
    __asm__ volatile(".set noat\n\t.set noreorder\n\tmove $8, %0\n\t.word 0x42800060\n\t.word 0x42848065\n\t.word 0x420000ac\n\t.word 0x4200006c\n\t"
                     ".word 0x42000020\n\t.word 0x4200003f\n\t.word 0x43f0ffe5\n\tmove %0, $8\n\t.set at\n\t.set reorder" : "+r"(r8) :: "$8", "memory");
    check(0x0601, h_count, 0);
    check(0x0602, r8, 0x12345678);
    __asm__ volatile(".word 0x42000007" ::: "memory");                /* function 7: a Reserved Instruction */
    check(0x0603, h_count, 1);
    check(0x0604, h_cause & 0x8000007C, 0x28);
}
/* ---- Integer Overflow: add, addi and sub raise it (Cause code 12, nothing written) when the signed 32-bit result does not fit; addu, addiu
 * and subu never do */
static void sec_overflow(void)
{
    u32 r, a = 0x7FFFFFFFu, b = 1, c = 0x80000000u;
    h_count = 0; h_ret = 0; r = 0x1234;
    __asm__ volatile("add %0, %1, %2" : "+r"(r) : "r"(a), "r"(b) : "memory");
    check(0x0701, h_count, 1);
    check(0x0702, h_cause & 0x8000007C, 12 << 2);
    check(0x0703, r, 0x1234);                                          /* (the destination keeps its value) */
    h_count = 0; r = 0x1234;
    __asm__ volatile("sub %0, %1, %2" : "+r"(r) : "r"(c), "r"(b) : "memory");
    check(0x0711, h_count, 1);
    check(0x0712, r, 0x1234);
    h_count = 0; r = 0x1234;
    __asm__ volatile("addi %0, %1, 1" : "+r"(r) : "r"(a) : "memory");
    check(0x0721, h_count, 1);
    check(0x0722, r, 0x1234);
    h_count = 0;
    __asm__ volatile("addu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b) : "memory");
    check(0x0731, h_count, 0);
    check(0x0732, r, 0xFFFFFFFF80000000ull & 0xFFFFFFFFu);
    __asm__ volatile("subu %0, %1, %2" : "=r"(r) : "r"(c), "r"(b) : "memory");
    check(0x0733, h_count, 0);
    __asm__ volatile("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(0u) : "memory");     /* no overflow: written */
    check(0x0741, h_count, 0);
    check(0x0742, r, 0x7FFFFFFF);
}
/* ---- dadd, dsub and daddi raise it the same way on the 64-bit result; the unsigned forms never do. OPD runs one such instruction on
 * the 64-bit values (ah:al, bh:bl) in registers 8 and 9, the destination ($10) preset to (rh:rl) and read back */
#define OPD(ins, ah, al, bh, bl, rh, rl) __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\t" \
    "dsll32 $8, %2, 0\n\tdsll32 $11, %3, 0\n\tdsrl32 $11, $11, 0\n\tor $8, $8, $11\n\t" \
    "dsll32 $9, %4, 0\n\tdsll32 $11, %5, 0\n\tdsrl32 $11, $11, 0\n\tor $9, $9, $11\n\t" \
    "dsll32 $10, %0, 0\n\tdsll32 $11, %1, 0\n\tdsrl32 $11, $11, 0\n\tor $10, $10, $11\n\t" \
    ins "\n\tnop\n\tdsra32 %0, $10, 0\n\tsll %1, $10, 0\n\t.set pop" \
    : "+r"(rh), "+r"(rl) : "r"(ah), "r"(al), "r"(bh), "r"(bl) : "$8", "$9", "$10", "$11", "memory")
static void sec_overflow64(void)
{
    u32 rh, rl;
    const u32 H = 0x7FFFFFFFu, L = 0x80000000u, M = 0xFFFFFFFFu;
#define OVF(id, ins, ah, al, bh, bl) \
    h_count = 0; rh = 0x1111; rl = 0x2222; OPD(ins, ah, al, bh, bl, rh, rl); \
    check(id, h_count, 1); check(id + 1, h_cause & 0x8000007C, 12 << 2); check(id + 2, ((u64)rh << 32) | rl, 0x111100002222ull)
#define NOV(id, ins, ah, al, bh, bl, wh, wl) \
    h_count = 0; rh = 0x1111; rl = 0x2222; OPD(ins, ah, al, bh, bl, rh, rl); \
    check(id, h_count, 0); check(id + 1, ((u64)rh << 32) | rl, ((u64)(wh) << 32) | (wl))
    h_ret = 0;
    OVF(0x0800, "dadd $10, $8, $9", H, M, 0, 1);                          /* the largest + 1 */
    OVF(0x0810, "dadd $10, $8, $9", L, 0, M, M);                          /* the smallest + -1 */
    OVF(0x0820, "dadd $10, $8, $9", H, M, H, M);
    OVF(0x0830, "dadd $10, $8, $9", L, 0, L, 0);
    OVF(0x0840, "dsub $10, $8, $9", L, 0, 0, 1);                          /* the smallest - 1 */
    OVF(0x0850, "dsub $10, $8, $9", H, M, M, M);                          /* the largest - -1 */
    OVF(0x0860, "dsub $10, $8, $9", 0, 0, L, 0);                          /* 0 - the smallest */
    OVF(0x0870, "daddi $10, $8, 1", H, M, 0, 0);
    OVF(0x0880, "daddi $10, $8, 0x7FFF", H, M, 0, 0);
    OVF(0x0890, "daddi $10, $8, -1", L, 0, 0, 0);
    OVF(0x08A0, "dadd $10, $8, $9", H, M, 0, 1);                          /* (again: nothing was left behind) */
    NOV(0x0900, "dadd $10, $8, $9", H, M, M, M, H, M - 1);                /* the largest + -1 */
    NOV(0x0910, "dadd $10, $8, $9", 0, H, 0, 1, 0, L);                    /* a 32-bit overflow is not a 64-bit one */
    NOV(0x0920, "dadd $10, $8, $9", M, M, 0, 1, 0, 0);
    NOV(0x0930, "dadd $10, $8, $9", L, 0, H, M, M, M);                    /* the smallest + the largest */
    NOV(0x0940, "dsub $10, $8, $9", 0, L, 0, 1, 0, H);
    NOV(0x0950, "dsub $10, $8, $9", H, M, H, M, 0, 0);
    NOV(0x0960, "dsub $10, $8, $9", L, 0, L, 0, 0, 0);
    NOV(0x0970, "dsub $10, $8, $9", 0, 0, 0, 1, M, M);
    NOV(0x0980, "daddi $10, $8, -1", H, M, 0, 0, H, M - 1);
    NOV(0x0990, "daddi $10, $8, 1", 0, H, 0, 0, 0, L);
    NOV(0x09A0, "daddu $10, $8, $9", H, M, 0, 1, L, 0);                   /* the unsigned forms wrap */
    NOV(0x09B0, "daddu $10, $8, $9", L, 0, M, M, H, M);
    NOV(0x09C0, "dsubu $10, $8, $9", L, 0, 0, 1, H, M);
    NOV(0x09D0, "dsubu $10, $8, $9", H, M, M, M, L, 0);
    NOV(0x09E0, "daddiu $10, $8, 1", H, M, 0, 0, L, 0);
    /* EPC, and in a branch's delay slot Cause.BD and the branch's address */
    {
        u32 pc;
        h_count = 0; rh = 0x1111; rl = 0x2222;
        __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\tla %2, 1f\n\t"
            "dsll32 $8, %3, 0\n\tdsll32 $11, %4, 0\n\tdsrl32 $11, $11, 0\n\tor $8, $8, $11\n\tli $9, 1\n\t"
            "1: dadd $10, $8, $9\n\tnop\n\t.set pop" : "+r"(rh), "+r"(rl), "=&r"(pc) : "r"(H), "r"(M) : "$8", "$9", "$10", "$11", "memory");
        check(0x0A00, h_count, 1);
        check(0x0A01, h_epc, pc);
        check(0x0A02, h_cause & 0x8000007C, 12 << 2);
        h_count = 0;
        __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\t"
            "dsll32 $8, %3, 0\n\tdsll32 $11, %4, 0\n\tdsrl32 $11, $11, 0\n\tor $8, $8, $11\n\tli $9, 1\n\tli $10, 0x2222\n\t"
            "la %2, 1f\n\t1: beq $0, $0, 2f\n\tdadd $10, $8, $9\n\t2: nop\n\tsll %0, $10, 0\n\t.set pop"
            : "=r"(rl), "+r"(rh), "=&r"(pc) : "r"(H), "r"(M) : "$8", "$9", "$10", "$11", "memory");
        check(0x0A10, h_count, 1);
        check(0x0A11, h_epc, pc);
        check(0x0A12, h_cause & 0x8000007C, 0x80000000u | (12 << 2));
        check(0x0A13, h_cause >> 31, 1);                                  /* (BD) */
        check(0x0A14, rl, 0x2222);
    }
#undef OVF
#undef NOV
}
/* ---- dmfc0 of LLAddr and TagLo is zero-extended (32 bits); mfc0 gives the word */
#define DMFC0(r, hi, lo) __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\tdmfc0 $8, $" #r "\n\tnop\n\tnop\n\tdsra32 %0, $8, 0\n\tsll %1, $8, 0\n\t.set pop" : "=r"(hi), "=r"(lo) :: "$8")
static void sec_dmfc0(void)
{
    u32 hi, lo;
    MTC0(17, 0x89ABCDEFu);
    DMFC0(17, hi, lo);
    check(0x0801, ((u64)hi << 32) | lo, 0x89ABCDEFull);
    MTC0(28, 0x89ABCDEFu);
    DMFC0(28, hi, lo);
    check(0x0802, ((u64)hi << 32) | lo, 0x89ABCDEFull);
    u32 w;
    __asm__ volatile("mfc0 %0, $28\n\tnop\n\tnop" : "=r"(w));
    check(0x0803, w, 0x89ABCDEFu);                                     /* (mfc0: the word) */
    MTC0(12, MFC0(12));
}
/* ---- the 64-bit registers: EPC, ErrorEPC and Context keep all 64 bits of a dmtc0 (Context's low word has its mask); an mtc0 writes the sign extension */
#define DMTC0(r, v) do { u32 vh_ = (u32)((u64)(v) >> 32), vl_ = (u32)(v); \
    __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\tdsll32 $8, %0, 0\n\tdsll32 $9, %1, 0\n\tdsrl32 $9, $9, 0\n\tor $8, $8, $9\n\t" \
                     "dmtc0 $8, $" #r "\n\tnop\n\tnop\n\t.set pop" :: "r"(vh_), "r"(vl_) : "$8", "$9", "memory"); } while (0)
static void sec_cp0_64(void)
{
    u32 hi, lo;
    u32 st = MFC0(12);
    MTC0(12, st & ~0x2u);                                              /* (EXL clear: EPC is the game's to write) */
    DMTC0(14, 0xF765432189ABCDEFull);
    DMFC0(14, hi, lo);
    check(0x0901, ((u64)hi << 32) | lo, 0xF765432189ABCDEFull);
    MTC0(14, 0x89ABCDEFu);                                             /* mtc0: the word, sign-extended */
    DMFC0(14, hi, lo);
    check(0x0902, ((u64)hi << 32) | lo, 0xFFFFFFFF89ABCDEFull);
    DMTC0(30, 0xF765432189ABCDEFull);
    DMFC0(30, hi, lo);
    check(0x0911, ((u64)hi << 32) | lo, 0xF765432189ABCDEFull);
    u32 c0 = MFC0(4);
    DMTC0(4, 0xF765432189ABCDEFull);
    DMFC0(4, hi, lo);
    check(0x0921, hi, 0xF7654321u);                                    /* (PTEBase's upper word) */
    check(0x0922, lo & 0xFF800000u, 0x89800000u);
    MTC0(4, c0);
    MTC0(12, st);
}
/* ---- CACHE with an address that is not a word's raises Address Error (AdEL); a word's does not */
static void sec_cache_align(void)
{
    u32 a = 0x80100000u;
    h_count = 0; h_ret = 0;
    __asm__ volatile("cache 0x15, 4(%0)" :: "r"(a) : "memory");
    check(0x0A01, h_count, 0);
    __asm__ volatile("cache 0x15, 2(%0)" :: "r"(a) : "memory");
    check(0x0A02, h_count, 1);
    check(0x0A03, h_cause & 0x8000007C, 4 << 2);
    check(0x0A04, h_badv, 0x80100002u);
}
int main(void)
{
    for (u32 v = 0; v < 0x200; v += 0x180) {
        u32 a = (u32)(void *)h_entry;
        *K1(v) = 0x3C1A0000u | (a >> 16);                      /* lui k0, hi */
        *K1(v + 4) = 0x375A0000u | (a & 0xFFFF);               /* ori k0, k0, lo */
        *K1(v + 8) = 0x03400008u;                              /* jr k0 */
        *K1(v + 12) = 0;
    }
    sec_fcr();
    sec_cop0_masks();
    sec_cop0_co();
    sec_overflow();
    sec_overflow64();
    sec_dmfc0();
    sec_cp0_64();
    sec_cache_align();
    *K1(0x000F0F04) = nfail;
    *K1(0x000F0F00) = nfail ? 0xBAD00000u | nfail : 0x600DC0DEu;
    for (;;) ;
}
