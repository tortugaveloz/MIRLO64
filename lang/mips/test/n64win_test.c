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
    /* ldl, ldr, sdl, sdr (a core with them: MCTL bit 27) are the game's accesses as well: RDRAM goes through at any byte, the
     * RCP's registers are the same address error (the load's code 4, the store's 5) */
    uint32_t mctl; __asm__ volatile("mfc0 %0, $7" : "=r"(mctl));
    if (mctl >> 27 & 1) {
        code[8]  = 0x00A01021u; code[9]  = 0x68820000u; code[10] = 0x03E00008u; code[11] = 0x00021000u;   /* v0 = a1; ldl v0, 0(a0); (sll v0, v0, 0 in the slot: v0 a 32-bit value) */
        code[12] = 0x00A01021u; code[13] = 0x6C820000u; code[14] = 0x03E00008u; code[15] = 0x00021000u;   /* v0 = a1; ldr v0, 0(a0); sll v0, v0, 0 */
        code[16] = 0xB0850000u; code[17] = 0x03E00008u; code[18] = 0x00001025u;                          /* sdl a1, 0(a0) */
        code[20] = 0xB4850000u; code[21] = 0x03E00008u; code[22] = 0x00001025u;                          /* sdr a1, 0(a0) */
        fn_t ldl = (fn_t)0x80001020u, ldr = (fn_t)0x80001030u, sdl = (fn_t)0x80001040u, sdr = (fn_t)0x80001050u;
        uint32_t t0 = s_traps;
        rd[4] = 0x01234567u; rd[5] = 0x89ABCDEFu;                                                   /* the doubleword at 0x80000210 */
        check("ldl rdram", ldl(0x80000213u, 0), 0xEF000000u);                                       /* byte 3 on: the last four bytes' first, at the top */
        check("ldr rdram", ldr(0x80000213u, 0), 0x01234567u);                                       /* bytes 0 to 3, at the bottom */
        sdl(0x80000215u, 0xAABBCCDDu);                                                              /* a1 is 0xFFFFFFFF_AABBCCDD: its top three bytes to bytes 5 to 7 */
        check("sdl rdram", rd[5], 0x89FFFFFFu);
        check("sdl rdram, the other word", rd[4], 0x01234567u);
        sdr(0x80000212u, 0x12345678u);                                                              /* its low three bytes to bytes 0 to 2 */
        check("sdr rdram", rd[4], 0x34567867u);
        check("sdr rdram, the other word", rd[5], 0x89FFFFFFu);
        check("no trap 3", s_traps, t0);
        (void)ldl(0xA4040010u, 0);
        check("ldl trap", s_traps, t0 + 1);
        check("ldl code", s_cause >> 2 & 31, 4);
        check("ldl badv", s_badv, 0xA4040010u);
        check("ldl epc", s_epc, 0x80001024u);
        sdr(0xA4400013u, 1);
        check("sdr trap", s_traps, t0 + 2);
        check("sdr code", s_cause >> 2 & 31, 5);
        check("sdr badv", s_badv, 0xA4400013u);
        check("sdr epc", s_epc, 0x80001050u);
        check("sdl trap, the cartridge", (sdl(0xB0000004u, 1), s_traps), t0 + 3);
    }
    /* native code still reaches the device window */
    uint32_t traps_now = s_traps;
    check("native regs", ctrl_scratch_read(), 0x12345678u);
    check("native trap free", s_traps, traps_now);
    puts_(fails ? "n64win_test: FAILED\n" : "n64win_test: PASS\n");
    putc_(4);
    for (;;);
}
