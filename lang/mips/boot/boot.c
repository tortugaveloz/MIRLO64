/* Portions derived from LiteX (BSD 2-Clause, (c) Enjoy-Digital & LiteX developers; see NOTICE) */
/* MIRLO's boot ROM on the MIPS SoC, after start.S: the game the Pocket
 * loaded, or a serial boot (SFL) over the JTAG UART -- the protocol
 * tools/jtag/sfl_term.py and tools/jtag/jtag_run.py speak. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <notice.h>

#define MAIN_RAM 0x40000000u

static void putc_(char c)
{
    /* bounded: with no host reading the JTAG UART, TXFULL never clears */
    for (unsigned i = 0; i < 400000u && uart_txfull_read(); i++);
    if (!uart_txfull_read()) uart_rxtx_write((uint8_t)c);
}
static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }

static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }

static void invalidate_caches(void)
{
    for (uint32_t a = MAIN_RAM; a < MAIN_RAM + 16384u; a += 32) __asm__ volatile(".set push\n.set mips3\ncache 0x00, 0(%0)\n.set pop" :: "r"(a));
    for (uint32_t a = MAIN_RAM; a < MAIN_RAM + 8192u; a += 16) __asm__ volatile(".set push\n.set mips3\ncache 0x01, 0(%0)\n.set pop" :: "r"(a));
}

/* A program is started only if it is built for this core's memory layout
 * (layout.h: the mark at its start). An older program's runtime keeps the
 * ROM and the saves elsewhere: it would write over what the Pocket loaded
 * from the save files, which the Pocket writes back when the core quits. Not
 * started, it leaves the SDRAM as the Pocket loaded it (the text on the
 * screen is in the RDRAM's place, where no slot is). */
static void jump(uint32_t addr)
{
    if (*(volatile uint32_t *)(addr + LAYOUT_MARK_OFS) != LAYOUT_MARK) {
        puts_("The program at 0x"); puthex(addr);
        puts_(" is not built for this core's memory layout: not started.\n");
        notice(LAYOUT_RDRAM, "The program file on the card is for an older Mirlo core and was not started.\n\n"
                             "Put the one that goes with this core there.\n\nThe saves are as they were.");
        return;
    }
    puts_("Executing booted program at 0x"); puthex(addr); puts_("\n");
    invalidate_caches();
    ((void (*)(void))addr)();
    for (;;);
}

void boot_exception(uint32_t cause, uint32_t epc, uint32_t badvaddr)
{
    puts_("\nboot ROM: exception, cause "); puthex(cause); puts_(" epc "); puthex(epc);
    puts_(" badvaddr "); puthex(badvaddr); puts_("\n");
    for (;;);
}

/* ---- SFL (serial firmware loader) */
#define SFL_MAGIC_REQ "sL5DdSMmkekro\n"
#define SFL_MAGIC_ACK "z6IHG7cYDID6o\n"
#define SFL_CMD_ABORT 0x00
#define SFL_CMD_LOAD  0x01
#define SFL_CMD_JUMP  0x02
#define SFL_ACK_SUCCESS  'K'
#define SFL_ACK_CRCERROR 'C'
#define SFL_ACK_UNKNOWN  'U'
#define SFL_ACK_ERROR    'E'

static unsigned short crc16(const unsigned char *p, int len)
{
    unsigned short crc = 0;
    while (len-- > 0) {
        crc ^= (unsigned short)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (unsigned short)(crc << 1 ^ 0x1021) : (unsigned short)(crc << 1);
    }
    return crc;
}
static int rx_ready(void) { return !uart_rxempty_read(); }
static unsigned char rx(void) { return (unsigned char)uart_rxtx_read(); }
static void tx_raw(char c) { putc_(c); }

/* the magic answered within `cycles`? */
static int check_ack(uint32_t cycles)
{
    static const char str[] = SFL_MAGIC_ACK;
    int rec = 0;
    uint32_t t0 = now();
    while (now() - t0 < cycles) {
        if (!rx_ready()) continue;
        char c = (char)rx();
        if (c == str[rec]) { if (++rec == 14) return 1; }
        else rec = c == str[0];
    }
    return 0;
}

static uint32_t be32(const unsigned char *d) { return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]; }

static void serialboot(void)
{
    puts_("Booting from serial...\n");
    for (;;) {                                      /* until a host answers */
        for (const char *c = SFL_MAGIC_REQ; *c; c++) tx_raw(*c);
        if (check_ack(CONFIG_CLOCK_FREQUENCY / 2)) break;
    }
    struct { unsigned char len, crc[2], cmd, payload[255]; } f;
    for (;;) {
        int i = 0, timeout = 1;
        uint32_t t0 = 0;
        while (i == 0 || now() - t0 < 10u * CONFIG_CLOCK_FREQUENCY) {
            if (!rx_ready()) continue;
            unsigned char b = rx();
            if (i == 0) { t0 = now(); f.len = b; }
            else if (i == 1) f.crc[0] = b;
            else if (i == 2) f.crc[1] = b;
            else if (i == 3) f.cmd = b;
            else f.payload[i - 4] = b;
            i++;
            if (i >= 4 && i == f.len + 4) { timeout = 0; break; }
        }
        if (timeout) { tx_raw(SFL_ACK_ERROR); continue; }
        if (crc16(&f.cmd, f.len + 1) != (unsigned short)(f.crc[0] << 8 | f.crc[1])) { tx_raw(SFL_ACK_CRCERROR); continue; }
        switch (f.cmd) {
        case SFL_CMD_ABORT:
            tx_raw(SFL_ACK_SUCCESS);
            return;
        case SFL_CMD_LOAD: {
            unsigned char *dst = (unsigned char *)be32(f.payload);
            for (int k = 0; k < f.len - 4; k++) dst[k] = f.payload[4 + k];
            tx_raw(SFL_ACK_SUCCESS);
            break;
        }
        case SFL_CMD_JUMP:
            tx_raw(SFL_ACK_SUCCESS);
            jump(be32(f.payload));
            return;                                 /* (not started: the host may send another) */
        default:
            tx_raw(SFL_ACK_UNKNOWN);
            break;
        }
    }
}

void boot_main(int loaded)
{
    puts_("\nMIRLO (MIPS) boot ROM\n");
    if (loaded) {
        puts_("Booting slot 0 (loaded by the Pocket)\n");
        jump(MAIN_RAM);
    } else puts_("No game loaded by the Pocket\n");
    for (;;) serialboot();
}
