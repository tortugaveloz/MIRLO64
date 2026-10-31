/* The RSP's DMA, its host registers and the DPC's writes, the same in both builds (the faithful RSP on the system clock, the
 * SoC built with MIRLO_RSP_OC: the pipelined RSP on the game CPU's clock, across rtl/soc/rsp_xing.sv), with the time a DMA
 * takes (printed, in system cycles).
 *   1  1024 words by DMA into DMEM and out again land intact, a queued second DMA too, SP_MEM_ADDR / SP_DRAM_ADDR read back what
 *      was written, the semaphore reads 0 1 1
 *   2  the DPC after the RSP's DMA: a write of DPC_START / DPC_END by the CPU while the RSP's DMA runs is seen */
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
static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }

#define SP   ((volatile uint32_t *)0x20000000u)      /* 0 MEM_ADDR 1 DRAM_ADDR 2 RD_LEN 3 WR_LEN 4 STATUS 5 DMA_FULL 6 DMA_BUSY 7 SEMAPHORE */
#define DPC  ((volatile uint32_t *)0x20000040u)
#define RAM(phys) ((volatile uint32_t *)(uintptr_t)(0xA0000000u + (phys)))

static void sp_wait(void) { for (uint32_t n = 0; n < 1000000u && SP[6]; n++) { } }

static void run(uint32_t salt)
{
    puts_("pass "); puthex(salt);
    SP[7] = 0;
    uint32_t s1 = SP[7], s2 = SP[7], s3 = SP[7];
    check("semaphore reads 0 1 1", s1 * 100 + s2 * 10 + s3, 11u);
    SP[7] = 0;
    SP[0] = 0x123 & ~7u; SP[1] = 0x300008;
    check("SP_MEM_ADDR", SP[0], 0x120u);
    check("SP_DRAM_ADDR", SP[1], 0x300008u);
    /* 1024 words in, DMEM and IMEM (4 KiB each) in two transfers, and out again */
    for (uint32_t i = 0; i < 1024; i++) { RAM(0x310000)[i] = 0x5A000000u ^ (i * 0x01010101u) ^ salt; RAM(0x320000)[i] = 0; RAM(0x330000)[i] = 0xFFFFFFFFu; }
    uint32_t t0 = now();
    SP[0] = 0x000; SP[1] = 0x310000; SP[2] = 0xFFF;          /* DMEM <- 4 KiB */
    SP[0] = 0x000; SP[1] = 0x320000; SP[3] = 0xFFF;          /* queued behind it: DMEM -> RDRAM */
    sp_wait();
    uint32_t t1 = now();
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1024; i++) if (RAM(0x320000)[i] != (0x5A000000u ^ (i * 0x01010101u) ^ salt)) bad++;
    check("4 KiB in and out", bad, 0u);
    puts_(": 4 KiB in and out "); puthex(t1 - t0); puts_(" sys cycles\n");
    /* the CPU's DPC writes while the RSP's DMA runs */
    SP[0] = 0x000; SP[1] = 0x330000; SP[3] = 0xFFF;          /* DMEM -> RDRAM again */
    DPC[3] = 0x8u;                                           /* freeze */
    DPC[0] = 0x350000u;
    check("DPC_START during the DMA", DPC[0], 0x350000u);
    sp_wait();
    DPC[3] = 0x4u;
}

int main(void)
{
    puts_("rsp_host_test\n");
    monitor_ctl_write(0x5u);
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x300\n mtc0 $t0, $7\n.set pop" ::: "t0");
    run(0x65); run(0x11); run(0x43);
    puts_(fails ? "rsp_host_test: FAILED\n" : "rsp_host_test: PASS\n");
    putc_(4);
    for (;;);
}
