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
    *K1(0x000F0F04) = nfail;
    *K1(0x000F0F00) = nfail ? 0xBAD00000u | nfail : 0x600DC0DEu;
    for (;;) ;
}
