/* The cartridge bus' write latch (lang/n64rt/n64rt_io.c cart_latch_*), as nemu64-test's "cart-writing" groups (466-477) check it; for the SoC
 * simulation: the verdict is in the RDRAM at 0x100000 (uncached stores), which MEMWATCH=0x43900000,0x43900400 prints as the SDRAM sees it (the game's RDRAM
 * is at 0x4380_0000): word 0 the number of failed checks, then (id, got, want) from word 4, the Count over the decay loops from word 0x40, and word 1 =
 * 0xD0DECAF0 when it is all done.
 *
 *   ./tlb_rom.sh OUT.z64 pi_rom.c && (cd dir && nust64 --elf OUT.elf)       (the ROM is OUT.z64 again)
 *   ROM=OUT.z64 MEMWATCH=0x43900000,0x43900400 sim/mips_soc/obj_soc/tb_soc lang/n64rt/build/n64rt.bin -c 400 -o dir -e 1000
 */
typedef unsigned int u32;
typedef unsigned long long u64;
#define CART(off) ((volatile u32 *)(0xB0000000u + (off)))

static u32 nfail; static u32 ids[32], gots[32], wants[32];
static void check(u32 id, u32 got, u32 want)
{
    if (got == want) return;
    if (nfail < 32) { ids[nfail] = id; gots[nfail] = got; wants[nfail] = want; }
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
);
static u32 count(void) { u32 c; __asm__ volatile("mfc0 %0, $9" : "=r"(c)); return c; }
/* the loop of nemu's decay test: n iterations of addi, bgtz, nop between the write (a word, a byte, a half or a doubleword) and the read */
#define WAITR(name, STORE) static u32 name(u32 n, volatile u32 *x, u32 *dt) \
{ \
    u32 r, t0; \
    __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\t.set gp=64\n\t" \
        "lui $4, 0xBADC\n\tori $4, $4, 0x0FFE\n\t" \
        "mfc0 %2, $9\n\t" \
        STORE " $4, 0(%1)\n\t" \
        "move $4, %3\n\t" \
        "2: addi $4, $4, -1\n\tbgtz $4, 2b\n\tnop\n\t" \
        "lw %0, 0(%1)\n\t.set pop" \
        : "=r"(r), "+r"(x), "=&r"(t0) : "r"(n) : "$4", "memory"); \
    *dt = count() - t0; \
    return r; \
}
WAITR(wait_sw, "sw") WAITR(wait_sb, "sb") WAITR(wait_sh, "sh") WAITR(wait_sd, "sd")
int main(void)
{
    volatile u32 *x = CART(0x2000);
    u32 w0 = *x, w1 = *CART(0x2004);
    /* Write32, Read32 (same location): the first read gives the written word, the next ones the ROM's */
    *x = 0xBADC0FFEu; *x = 0xDECAFu;
    check(1, *x, 0xBADC0FFEu); check(2, *x, w0); check(3, *x, w0);
    /* a different location, nearby; the beginning of the ROM; the end; outside it (the PIF ROM: no latch) */
    *CART(0x2004) = 0xBADC0FFEu; *x = 0xDECAFu;
    check(4, *x, 0xBADC0FFEu); check(5, *x, w0);
    *CART(0) = 0xBADC0FFEu; *x = 0xDECAFu;
    check(6, *x, 0xBADC0FFEu); check(7, *x, w0);
    *(volatile u32 *)0xBFBFFFFCu = 0xBADC0FFEu; *x = 0xDECAFu;
    check(8, *x, 0xBADC0FFEu); check(9, *x, w0);
    *(volatile u32 *)0xBFC00000u = 0xBADC0FFEu; *x = 0xDECAFu;
    check(10, *x, 0xDECAFu); check(11, *x, w0);
    /* narrower reads of the latch: the upper bits */
    *CART(0) = 0xBADC0FFEu; *x = 0xDECAFu;
    check(12, *(volatile unsigned char *)x, 0xBA); check(13, *(volatile unsigned char *)x, w0 >> 24);
    *CART(0) = 0xBADC0FFEu; *x = 0xDECAFu;
    check(14, *(volatile unsigned short *)x, 0xBADC); check(15, *(volatile unsigned short *)x, w0 >> 16);
    /* narrower writes: zeros in the lanes below / the register shifted to the lane */
    *(volatile unsigned char *)CART(0) = 0xBA;  *x = 0xDECAFu;
    check(16, *x, 0xBA000000u); check(17, *x, w0);
    *(volatile unsigned short *)CART(0) = 0xBADC; *x = 0xDECAFu;
    check(18, *x, 0xBADC0000u); check(19, *x, w0);
    __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\tlui $2, 0xB000\n\tlui $3, 0x1234\n\tori $3, $3, 0x56BA\n\tsb $3, 1($2)\n\t.set pop" ::: "$2", "$3", "memory");
    *x = 0xDECAFu;
    check(20, *x, 0x56BA0000u); check(21, *x, w0);
    /* sd: the upper word only */
    __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\t.set gp=64\n\tlui $2, 0xB000\n\tlui $3, 0x9876\n\tori $3, $3, 0x5432\n\tdsll $3, $3, 32\n\tlui $5, 0x1AF1\n\tor $3, $3, $5\n\tsd $3, 0($2)\n\t.set pop" ::: "$2", "$3", "$5", "memory");
    *x = 0xDECAFu;
    check(22, *x, 0x98765432u); check(23, *x, w0);
    /* PI_STATUS' IO busy (bit 1) while it is held */
    {
        volatile u32 *st = (volatile u32 *)0xA4600010u;
        *CART(0) = 0xBADC0FFEu;
        u32 s1 = *st;
        (void)*x;
        u32 s2 = *st;
        check(40, s1 & 2, 2); check(41, s2 & 2, 0);
        __asm__ volatile(".set push\n\t.set noat\n\t.set noreorder\n\t.set gp=64\n\tlui $2, 0xB000\n\tlui $3, 0x0101\n\tdsll32 $3, $3, 0\n\tlui $5, 0x2323\n\tor $3, $3, $5\n\tsd $3, 0($2)\n\t.set pop" ::: "$2", "$3", "$5", "memory");
        s1 = *st;
        (void)*x;
        s2 = *st;
        check(42, s1 & 2, 2); check(43, s2 & 2, 0);
    }
    /* the decay: a few iterations is still there, a hundred is gone */
    static u32 (*const waits[4])(u32, volatile u32 *, u32 *) = { wait_sw, wait_sb, wait_sh, wait_sd };
    volatile u32 *R = (volatile u32 *)0xA0100000u;
    for (u32 k = 0; k < 4; k++) {
        u32 dt0, dt1, dt2;
        u32 r0 = waits[k](0, x, &dt0), r1 = waits[k](10, x, &dt1), r2 = waits[k](110, x, &dt2);
        const u32 want = k == 1 ? 0xBA000000u : k == 2 ? 0x0FFE0000u : 0xBADC0FFEu;      /* (the lane of a byte / half at address 0: the register's low bits there) */
        (void)want;
        check(24 + 3 * k, r0 != w0, 1); check(25 + 3 * k, r1 != w0, 1); check(26 + 3 * k, r2, w0);
        R[0x40 + 3 * k] = dt0; R[0x41 + 3 * k] = dt1; R[0x42 + 3 * k] = dt2;
    }
    for (u32 i = 0; i < nfail && i < 32; i++) { R[4 + 3 * i] = ids[i]; R[5 + 3 * i] = gots[i]; R[6 + 3 * i] = wants[i]; }
    R[0] = nfail;
    R[1] = 0xD0DECAF0u;
    (void)w1;
    for (;;) ;
}
