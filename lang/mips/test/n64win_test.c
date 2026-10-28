/* The N64 window of MIRLO's MIPS game CPU (mips_core's N64WIN, sim/mips_soc):
 * code fetched from KSEG0's window (0x8000_0000 + 8 MiB) is an N64 game's.
 * Its accesses to RDRAM go through; its accesses to anything else -- the
 * RCP's registers here -- take an address error that Mirlo's runtime can
 * emulate (BadVAddr: the address; EPC: the instruction). Native code at
 * 0x4000_0000 reaches every address as before. */
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

static volatile uint32_t s_traps, s_cause, s_badv, s_epc;
#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
void trap_handler(void)
{
    s_traps++;
    s_cause = MFC0(13); s_badv = MFC0(8); s_epc = MFC0(14);
    __asm__ volatile("mtc0 %0, $14" :: "r"(s_epc + 4));   /* skip the access */
}

typedef uint32_t (*fn_t)(uint32_t a, uint32_t v);

int main(void)
{
    puts_("n64win_test\n");
    /* the game's code: lw v0, 0(a0) / jr ra / nop, and sw a1, 0(a0) / jr ra / move v0, zero */
    volatile uint32_t *code = (volatile uint32_t *)0xA0001000u;    /* written uncached */
    code[0] = 0x8C820000u; code[1] = 0x03E00008u; code[2] = 0x00000000u;
    code[4] = 0xAC850000u; code[5] = 0x03E00008u; code[6] = 0x00001025u;
    fn_t ld = (fn_t)0x80001000u, st = (fn_t)0x80001010u;
    volatile uint32_t *rd = (volatile uint32_t *)0x80000200u;
    rd[0] = 0x600DCAFEu;
    check("rdram load", ld(0x80000200u, 0), 0x600DCAFEu);
    check("no trap", s_traps, 0);
    st(0x80000204u, 0x12345678u);
    check("rdram store", rd[1], 0x12345678u);
    flush_cpu_dcache();
    check("kseg1 load", ld(0xA0000204u, 0), 0x12345678u);
    check("no trap 2", s_traps, 0);
    (void)ld(0xA4040010u, 0);                                       /* SP_STATUS */
    check("load trap", s_traps, 1);
    check("load code", s_cause >> 2 & 31, 4);
    check("load badv", s_badv, 0xA4040010u);
    check("load epc", s_epc, 0x80001000u);
    st(0xA4400010u, 0x1u);                                          /* VI_CURRENT */
    check("store trap", s_traps, 2);
    check("store code", s_cause >> 2 & 31, 5);
    check("store badv", s_badv, 0xA4400010u);
    check("store epc", s_epc, 0x80001010u);
    (void)ld(0xB0000000u, 0);                                       /* the cartridge */
    check("cart trap", s_traps, 3);
    /* native code still reaches the device window */
    check("native regs", ctrl_scratch_read(), 0x12345678u);
    check("native trap free", s_traps, 3);
    puts_(fails ? "n64win_test: FAILED\n" : "n64win_test: PASS\n");
    putc_(4);
    for (;;);
}
