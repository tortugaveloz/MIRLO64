/* The D-cache against the other masters' writes into the RDRAM (sim/mips_soc; docs/mips.md "Caches and memory").
 *
 * The D-cache's lines are 32 bytes where the VR4300's are 16, so a buffer that is 16-byte aligned but not 32-byte aligned
 * shares its first or last line with 16 bytes of something else. Software drops the buffer's lines before a transfer into
 * it, as on the console; a load of the other half while the transfer is under way then reads the whole line back from
 * memory that does not have the transfer's words yet, and that line stays in the cache after the transfer. Each phase
 * reproduces that, and checks that the buffer reads what was transferred:
 *   1  the PI's engine (MONITOR.PIDST / PISRC / PILEN) copying 1 KiB into a buffer whose last 16 bytes share a line with
 *      the 16 bytes after it, which are loaded while the copy runs
 *   2  the RSP's DMA (SP_WR_LEN: DMEM to the RDRAM) of 4 KiB, laid out the same way
 *   3  the SI's RD64B (PIF RAM to the RDRAM) into a buffer whose first 16 bytes share a line with the 16 bytes before it,
 *      loaded while the read waits for the PIF (PEND bit 4, before MONITOR.SIDONE): Ocarina of Time's controller read
 * The buffers are read through KSEG0 (cached) and filled with an old pattern through KSEG1 before each transfer. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}

#define K0(phys) ((volatile uint32_t *)(uintptr_t)(0x80000000u + (phys)))     /* the RDRAM, cached */
#define K1(phys) ((volatile uint32_t *)(uintptr_t)(0xA0000000u + (phys)))     /* ... uncached */
#define SP  ((volatile uint32_t *)0x20000000u)       /* 0 MEM_ADDR 1 DRAM_ADDR 2 RD_LEN 3 WR_LEN .. 6 DMA_BUSY */
#define SI  ((volatile uint32_t *)0xA4800000u)       /* 0 DRAM_ADDR, 1 RD64B, 6 STATUS */
#define PIF ((volatile uint32_t *)0xBFC007C0u)       /* PIF RAM, KSEG1 */
#define SDRAM_RDRAM 0x43800000u                      /* the RDRAM's place in the SDRAM */

static void mctl(uint32_t v) { __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(v)); }
/* drop the D-cache's lines over [phys, phys + n), as osInvalDCache does (a hit op on each 16 bytes, the VR4300's line) */
static void inval(uint32_t phys, uint32_t n)
{
    for (uint32_t a = 0x80000000u + phys; a < 0x80000000u + phys + n; a += 16u) MIPS_CACHE_OP(0x11, a);
}
static void old_fill(uint32_t phys, uint32_t words, uint32_t tag)
{
    for (uint32_t i = 0; i < words; i++) K1(phys)[i] = tag | i;
}
/* the buffer's words through KSEG0 against what was transferred: the count of wrong words, the first reported */
static uint32_t compare(const char *what, uint32_t phys, const uint32_t *want_k1, uint32_t words)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t got = K0(phys)[i], want = want_k1[i];
        if (got != want && bad++ == 0) {
            puts_(what); puts_(": word at "); puthex(phys + 4u * i); puts_(" reads "); puthex(got);
            puts_(", transferred "); puthex(want); puts_("\n");
        }
    }
    return bad;
}

#define PI_SRC  0x42000000u                           /* the engine's source (SDRAM) */
#define PI_DST  0x00100010u                           /* 1 KiB, ending at 0x0010_0410 (16 mod 32) */
#define PI_LEN  1024u
#define SP_SRC  0x00110000u                           /* DMEM's 4 KiB come from here */
#define SP_DST  0x00120010u                           /* 4 KiB, ending at 0x0012_1010 */
#define SP_LEN  4096u
#define SI_DST  0x00130010u                           /* 64 bytes, from 16 mod 32 */

int main(void)
{
    puts_("dma_coh_test\n");
    monitor_pend_write(0x1F);
    monitor_mask_write(0u);                              /* nothing wakes the monitor */
    monitor_ctl_write(5u);                               /* OWN, IP2 is the MI's line */
    monitor_miclr_write(0x3Eu);
    mctl(0x300u);                                        /* the pass-through: the SI's registers and PIF RAM */

    /* 1: the PI's engine */
    {
        volatile uint32_t *src = (volatile uint32_t *)PI_SRC;
        for (uint32_t i = 0; i < PI_LEN / 4u; i++) src[i] = 0x11000000u | (i * 0x10001u);
        old_fill(PI_DST, PI_LEN / 4u + 4u, 0xA1000000u);   /* the buffer and the 16 bytes after it */
        uint32_t after = K0(PI_DST + PI_LEN)[0];
        inval(PI_DST, PI_LEN + 16u);
        monitor_pidst_write((SDRAM_RDRAM + PI_DST - 0x40000000u) >> 2);
        monitor_pisrc_write((PI_SRC - 0x40000000u) >> 2);
        monitor_pilen_write(PI_LEN / 4u);                 /* starts it */
        uint32_t a = K0(PI_DST + PI_LEN)[0];             /* the line of the buffer's last 16 bytes comes in */
        for (uint32_t t = 0; t < 4000000u && (monitor_pilen_read() >> 31); t++) { }
        check("PI: the engine is done", monitor_pilen_read() >> 31, 0u);
        check("PI: the word after the buffer, loaded during the copy", a, after);
        check("PI: the buffer's wrong words after the copy",
              compare("PI", PI_DST, (const uint32_t *)PI_SRC, PI_LEN / 4u), 0u);
    }

    /* 2: the RSP's DMA, DMEM to the RDRAM */
    {
        for (uint32_t i = 0; i < SP_LEN / 4u; i++) K1(SP_SRC)[i] = 0x22000000u | (i * 0x10003u);
        SP[0] = 0x000; SP[1] = SP_SRC; SP[2] = SP_LEN - 1u;          /* DMEM <- the source */
        for (uint32_t n = 0; n < 1000000u && SP[6]; n++) { }
        old_fill(SP_DST, SP_LEN / 4u + 4u, 0xA2000000u);
        uint32_t after = K0(SP_DST + SP_LEN)[0];
        inval(SP_DST, SP_LEN + 16u);
        SP[0] = 0x000; SP[1] = SP_DST; SP[3] = SP_LEN - 1u;          /* DMEM -> the buffer */
        uint32_t a = K0(SP_DST + SP_LEN)[0];
        for (uint32_t n = 0; n < 1000000u && SP[6]; n++) { }
        check("RSP: the DMA is done", SP[6], 0u);
        check("RSP: the word after the buffer, loaded during the DMA", a, after);
        check("RSP: the buffer's wrong words after the DMA",
              compare("RSP", SP_DST, (const uint32_t *)(uintptr_t)(0xA0000000u + SP_SRC), SP_LEN / 4u), 0u);
    }

    /* 3: the SI's RD64B, held while the PIF runs the block's commands */
    {
        for (uint32_t i = 0; i < 15; i++) PIF[i] = 0x33000000u | (i * 0x10007u);
        PIF[15] = 0x3300FF01u;                            /* bit 0 of the status byte: the block holds commands */
        old_fill(SI_DST - 16u, 4u + 16u, 0xA3000000u);   /* the 16 bytes before the buffer, and the buffer */
        uint32_t before = K0(SI_DST - 16u)[0];
        inval(SI_DST, 64u);
        SI[0] = SI_DST; SI[1] = 0x1FC007C0u;
        uint32_t k;
        for (k = 0; k < 100000u && !(monitor_pend_read() & 0x10u); k++) { }
        check("SI: the RD64B waits for the PIF (PEND bit 4)", (monitor_pend_read() >> 4) & 1u, 1u);
        uint32_t b = K0(SI_DST - 16u)[0];                /* the line of the buffer's first 16 bytes comes in */
        monitor_pend_write(0x10u);
        monitor_sidone_write(1);
        for (uint32_t n = 0; n < 1000000u && (SI[6] & 1u); n++) { }
        check("SI: the RD64B is done", SI[6] & 1u, 0u);
        SI[6] = 0;
        check("SI: the word before the buffer, loaded while the read waited", b, before);
        uint32_t pif[15];
        for (uint32_t i = 0; i < 15; i++) pif[i] = PIF[i];
        check("SI: the buffer's wrong words after the RD64B", compare("SI", SI_DST, pif, 15u), 0u);
    }

    puts_(fails ? "dma_coh_test: FAIL\n" : "dma_coh_test: PASS\n");
    putc_(4);
    for (;;);
}
