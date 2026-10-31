/* What the TLB tests (tlbmon_test, tlbgame_test, tlbpg_test, tlbreal_test; sim/mips_soc) have in common: output, COP0 and TLB
 * instructions, EntryLo, and the "game" side of mips_core's real TLB -- the game's own vectors in the RDRAM and code in
 * KSEG0, because an exception of code that is not the game's (this program's, in the SDRAM at 0x4000_0000) is the program's
 * own trap_handler, at 0x4000_0180, and one of the game's (a pc in the RDRAM's KSEG0 window, or on a mapped page) is the
 * game's, at 0x8000_0000 (a refill with EXL clear) or 0x8000_0180.
 *
 * gv_refill / gv_general: stubs for those two vectors. Each notes in the log (0x8001_0000: +0 the refill vector's count, +4
 * the general vector's) that it was taken and jumps to 0x4000_0180, where init_asm.S saves the registers, calls the program's
 * trap_handler() and returns with an eret -- so a test's handler is C and sees Cause, BadVAddr, Context, EntryHi, EPC and
 * Status as the game's would. */
#ifndef TLBTEST_H
#define TLBTEST_H
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static __attribute__((unused)) void putdec(uint32_t v) { char b[12]; int n = 0; do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}
static void done(const char *name)
{
    puts_(name); puts_(fails ? ": FAILED\n" : ": PASS\n");
    putc_(4);
    for (;;);
}

#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_) :: "memory"); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)) : "memory")
#define K0(pa) ((volatile uint32_t *)(0x80000000u | (pa)))
#define K1(pa) ((volatile uint32_t *)(0xA0000000u | (pa)))
#define TLBWI() __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop" ::: "memory")
#define TLBWR() __asm__ volatile(".set push\n.set mips3\ntlbwr\n.set pop" ::: "memory")
#define TLBR()  __asm__ volatile(".set push\n.set mips3\ntlbr\n.set pop" ::: "memory")
#define TLBP()  __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop" ::: "memory")

/* EntryLo for a physical address: C, and V / D / G as asked */
enum { LV = 2, LD = 4, LG = 1, LC_CACHED = 3 << 3, LC_UNCACHED = 2 << 3 };
#define LO(pa, f) ((((uint32_t)(pa) >> 12) << 6) | LC_CACHED | (f))
#define LOC(pa, c, f) ((((uint32_t)(pa) >> 12) << 6) | ((c) << 3) | (f))

/* the capability word (COP0 23): 'T' 'L' 'B', bit 0 MON, bit 1 a real TLB, bit 2 cached fetches from mapped pages */
static int has_real_tlb(void) { return (MFC0(23) >> 8) == 0x544C42u && (MFC0(23) & 2u); }

static void tlb_wi(uint32_t idx, uint32_t pm, uint32_t hi, uint32_t lo0, uint32_t lo1)
{
    MTC0(0, idx); MTC0(5, pm); MTC0(10, hi); MTC0(2, lo0); MTC0(3, lo1);
    TLBWI();
    __asm__ volatile("nop; nop");               /* (a TLB write is not in the delay slot of the call that follows) */
}
static __attribute__((unused)) void tlb_unmap_all(void)
{
    for (uint32_t i = 0; i < 32; i++) tlb_wi(i, 0, 0x80000000u + (i << 13), 0, 0);
}
static __attribute__((unused)) uint32_t tlb_probe(uint32_t hi)
{
    MTC0(10, hi); TLBP();
    __asm__ volatile("nop; nop");
    return MFC0(0);
}

/* ---- the game's vectors and the log */
#define LOGBASE 0x10000u
#define LOGW(o) (*K0(LOGBASE + (o)))
extern char gv_refill[], gv_refill_end[], gv_general[], gv_general_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl gv_refill\n"
"gv_refill:\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0($k0)\n"
"    lui   $k0, 0x4000\n"
"    ori   $k0, $k0, 0x180\n"
"    jr    $k0\n"
"    nop\n"
"    .globl gv_refill_end\n"
"gv_refill_end:\n"
"    .globl gv_general\n"
"gv_general:\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 4($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 4($k0)\n"
"    lui   $k0, 0x4000\n"
"    ori   $k0, $k0, 0x180\n"
"    jr    $k0\n"
"    nop\n"
"    .globl gv_general_end\n"
"gv_general_end:\n"
);
static __attribute__((unused)) void copy_k1(uint32_t dst, const char *a, const char *b)
{
    for (const uint32_t *p = (const uint32_t *)a; p < (const uint32_t *)b; p++, dst += 4) *K1(dst) = *p;
}
static __attribute__((unused)) void install_game_vectors(void)
{
    copy_k1(0x0, gv_refill, gv_refill_end);
    copy_k1(0x180, gv_general, gv_general_end);
    LOGW(0) = 0; LOGW(4) = 0;
    flush_cpu_dcache(); flush_cpu_icache();
}
#define REFILLS LOGW(0)
#define GENERALS LOGW(4)

/* ---- code of the game's: small routines copied into the RDRAM, so that the pc is the game's (KSEG0). They are position
 * independent. Each is called by jalr from this program (the SDRAM) and returns with jr ra.
 *   gc_load   a0 = address          v0 = lw 0(a0)
 *   gc_store  a0 = address, a1      sw a1, 0(a0)
 *   gc_slot   a0 = address          v0 = 0; b 1f; lw v0, 0(a0) in the delay slot; v1 += 100 (skipped); 1: v1 += 1 -> v1
 *   gc_exl    a0 = address          sets Status.EXL, then lw: the handler returns to the label (the EPC the test sets) */
extern char gc_start[], gc_end[], gc_load[], gc_store[], gc_slot[], gc_exl[], gc_exl_land[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl gc_start\n"
"gc_start:\n"
"    .globl gc_load\n"
"gc_load:\n"
"    lw    $v0, 0($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gc_store\n"
"gc_store:\n"
"    sw    $a1, 0($a0)\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gc_slot\n"
"gc_slot:\n"
"    move  $v1, $zero\n"
"    move  $v0, $zero\n"
"    b     1f\n"
"    lw    $v0, 0($a0)\n"
"    addiu $v1, $v1, 100\n"
"1:  addiu $v1, $v1, 1\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gc_exl\n"
"gc_exl:\n"
"    mfc0  $t0, $12\n"
"    ori   $t0, $t0, 2\n"
"    mtc0  $t0, $12\n"
"    lw    $v0, 0($a0)\n"
"    addiu $v1, $v1, 100\n"
"    .globl gc_exl_land\n"
"gc_exl_land:\n"
"    addiu $v1, $v1, 1\n"
"    jr    $ra\n"
"    nop\n"
"    .globl gc_end\n"
"gc_end:\n"
);
#define GC_BASE 0x7F0000u                       /* where they go: the end of the RDRAM */
typedef uint32_t (*gcall1_t)(uint32_t);
typedef uint32_t (*gcall2_t)(uint32_t, uint32_t);
#define GC(sym) ((uint32_t)(0x80000000u | (GC_BASE + ((uint32_t)(uintptr_t)(sym) - (uint32_t)(uintptr_t)gc_start))))
static __attribute__((unused)) void install_game_code(void)
{
    copy_k1(GC_BASE, gc_start, gc_end);
    flush_cpu_dcache(); flush_cpu_icache();
}
static __attribute__((unused)) uint32_t gload(uint32_t va) { return ((gcall1_t)GC(gc_load))(va); }
static __attribute__((unused)) void gstore(uint32_t va, uint32_t v) { ((gcall2_t)GC(gc_store))(va, v); }
static __attribute__((unused)) uint32_t gslot(uint32_t va, uint32_t *v0) { uint32_t v1; register uint32_t a0 __asm__("a0") = va; register uint32_t r0 __asm__("v0"), r1 __asm__("v1");
    __asm__ volatile("jalr %3" : "=r"(r0), "=r"(r1), "+r"(a0) : "r"(GC(gc_slot)) : "ra", "memory", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "at");
    v1 = r1; *v0 = r0; return v1; }
#endif
