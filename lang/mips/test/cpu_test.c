/* A bare self-check of MIRLO's MIPS game CPU on the SoC (sim/mips_soc):
 * big-endian (the N64's order) loads and stores of every width (lb/lh/lw, lwl/lwr,
 * swl/swr through memcpy of unaligned data), the D-cache against uncached
 * device reads, the FPU, the divider, the registers. No libc. Prints PASS or
 * the failures on the UART, then a 0x04 (the bench stops). */
#include <stdint.h>
#include <generated/csr.h>
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

static volatile uint8_t buf[64] __attribute__((aligned(8)));
struct __attribute__((packed)) un { uint8_t a; uint32_t w; uint16_t h; };
static volatile struct un u;
static volatile float fa = 1.5f, fb = -2.25f;
static volatile uint32_t da = 1000000007u, db = 97u;

int main(void)
{
    puts_("cpu_test\n");
    /* byte order: a word's bytes, a halfword's */
    volatile uint32_t *w = (volatile uint32_t *)buf;
    w[0] = 0x44332211u;
    check("lbu 0", buf[0], 0x44); check("lbu 3", buf[3], 0x11);
    check("lhu 0", ((volatile uint16_t *)buf)[0], 0x4433); check("lhu 2", ((volatile uint16_t *)buf)[1], 0x2211);
    check("lb sign", (uint32_t)(int32_t)((volatile int8_t *)buf)[0], 0x44);
    buf[1] = 0xAA; check("sb 1", w[0], 0x44AA2211u);
    ((volatile uint16_t *)buf)[1] = 0xBEEF; check("sh 2", w[0], 0x44AABEEFu);
    ((volatile int8_t *)buf)[4] = -2; check("lb neg", (uint32_t)(int32_t)((volatile int8_t *)buf)[4], 0xFFFFFFFEu);
    /* unaligned (lwl/lwr, swl/swr) */
    u.a = 1; u.w = 0xCAFEF00Du; u.h = 0x1234;
    check("unaligned w", u.w, 0xCAFEF00Du); check("unaligned h", u.h, 0x1234);
    uint8_t *p = (uint8_t *)&u;
    check("unaligned bytes", (uint32_t)p[1] << 24 | p[2] << 16 | p[3] << 8 | (uint32_t)p[4], 0xCAFEF00Du);
    /* the D-cache: a cached write then a read through the device window of the same SDRAM? (no alias:
     * check write-through by invalidating and reading back) */
    w[2] = 0x5A5A1234u; flush_cpu_dcache(); check("write-through", w[2], 0x5A5A1234u);
    /* the FPU (single) and doubles in software are not linked here: floats only */
    float f = fa * fb + 0.5f;
    check("fmul/fadd", *(uint32_t *)&f, 0xC0380000u);          /* -2.875 */
    float q = fa / fb;
    check("fdiv", *(uint32_t *)&q, 0xBF2AAAABu);                /* -0.6666667 */
    check("cvt", (uint32_t)(int32_t)(fb * 4.0f), (uint32_t)-9);
    /* the divider */
    check("divu", da / db, 10309278u); check("remu", da % db, 1000000007u - 10309278u * 97u);
    /* the registers */
    check("scratch", ctrl_scratch_read(), 0x12345678u);
    ctrl_scratch_write(0xA5A55A5Au); check("scratch w", ctrl_scratch_read(), 0xA5A55A5Au);
    timer0_uptime_latch_write(1); uint64_t t0 = timer0_uptime_cycles_read();
    timer0_uptime_latch_write(1); uint64_t t1 = timer0_uptime_cycles_read();
    check("uptime runs", t1 > t0, 1);
    check("bus errors", ctrl_bus_errors_read(), 0);
    /* nothing there: just above the SDRAM, physical (0x2000_0000..0x4FFF_FFFF is not the TLB's), and with bit 30
     * set, so none of the RCP's blocks or the cartridge's, which ignore bits 31:29, answer it */
    (void)*(volatile uint32_t *)0x44000000u;
    check("bus error counted", ctrl_bus_errors_read(), 1);
    /* the N64 window: KSEG0 (cached), KSEG1 and SDRAM 0x4380_0000 are one RDRAM */
    volatile uint32_t *k0 = (volatile uint32_t *)0x80000100u, *k1 = (volatile uint32_t *)0xA0000100u,
                      *sd = (volatile uint32_t *)0x43800100u;
    k0[0] = 0x01234567u; check("kseg1 sees kseg0", k1[0], 0x01234567u); check("sdram sees kseg0", sd[0], 0x01234567u);
    sd[1] = 0x89ABCDEFu; check("kseg0 sees sdram", k0[1], 0x89ABCDEFu);
    k1[2] = 0x55AA55AAu; flush_cpu_dcache(); check("kseg0 sees kseg1", k0[2], 0x55AA55AAu);
    ((volatile uint8_t *)k0)[13] = 0x77; check("kseg0 sb", sd[3] >> 16 & 0xFF, 0x77);
    volatile uint32_t *top = (volatile uint32_t *)0x807FFFFCu;
    *top = 0xFACEB00Cu; check("window top", *(volatile uint32_t *)0x43FFFFFCu, 0xFACEB00Cu);
    puts_(fails ? "cpu_test: FAILED\n" : "cpu_test: PASS\n");
    putc_(4);
    for (;;);
}
