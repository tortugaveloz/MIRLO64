/* padtest: what the Pocket and its dock send as controllers, on the screen -- for finding out what a pad (the Switch Online
 * N64 controller, say) arrives as. It is a program of its own, not a game: no ROM is looked at.
 *
 *   P1 K 30000010 BTN 8000        the pad's KEY word (the top digit is KEY_TYPE: 0 nothing, 1 the Pocket's own controls,
 *      J 80 80 80 80  S+80+00      2 a dock pad without analog, 3 with analog, 4 keyboard, 5 mouse; the low 16 bits
 *                                  the buttons), BTN what the N64 game would get of it with the core's settings now
 *                                  (the N64 controller word), J the four stick bytes LX LY RX RY (0x80 is the centre,
 *                                  hex), S the N64 stick it makes (-80..+80)
 *   S0 / S1                       the two words of the core's settings (interact.json)
 * Bits of the key word, as the Analogue docs have them: 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 X, 7 Y, 8 L1, 9 R1, 10 L2,
 * 11 R2, 12 L3, 13 R3, 14 select, 15 start -- write down which bit each button of the pad lights.
 * The same on the JTAG UART when a pad's words change (tools/jtag/sfl_term.py).
 *
 * On a Pocket: this file built (make padtest in lang/mips/test -> build/padtest.bin) goes where the core's runtime goes, as
 * Assets/n64/common/n64rt.sys (keep the real one aside); start the core with any game file (it is loaded and ignored). In the
 * simulation: ROM=<any file> tb_soc build/padtest.bin -c 100 -o <dir> -e 10, PAD / PAD2 / PAD3 / PAD4 for the pads
 * (sim/mips_soc/tb_soc.cpp) and INTERACT=<address>:<value> for the settings. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <notice.h>
#include "../../n64rt/n64rt_pad.h"

static void putc_(char c)
{
    for (unsigned i = 0; i < 20000u && uart_txfull_read(); i++);     /* (bounded: with no host reading the JTAG UART TXFULL never clears) */
    if (!uart_txfull_read()) uart_rxtx_write((uint8_t)c);
}
static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }
void trap_handler(void) { puts_("TRAP\n"); for (;;) ; }
static uint64_t now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }

static void read_pad(int i, uint32_t *k, uint32_t *j)
{
    switch (i) {
    case 0:  *k = apf_input_cont1_key_read(); *j = apf_input_cont1_joy_read(); break;
    case 1:  *k = apf_input_cont2_key_read(); *j = apf_input_cont2_joy_read(); break;
    case 2:  *k = apf_input_cont3_key_read(); *j = apf_input_cont3_joy_read(); break;
    default: *k = apf_input_cont4_key_read(); *j = apf_input_cont4_joy_read(); break;
    }
}

int main(void)
{
    puts_("padtest\n");
    static char text[400], last[400];
    uint64_t t_draw = 0, t_log = 0;
    uint32_t lk[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }, lj[4] = { 0, 0, 0, 0 };
    for (;;) {
        const uint32_t s0 = apf_interact_interact0_read(), s1 = apf_interact_interact1_read();
        const uint64_t t = now();
        int n = snprintf(text, sizeof text, "PADTEST          T %05lu\nS0 %08lX S1 %08lX\n", (unsigned long)(t / CONFIG_CLOCK_FREQUENCY),
                         (unsigned long)s0, (unsigned long)s1);
        int changed = 0;
        for (int i = 0; i < 4; i++) {
            uint32_t k, j;
            uint16_t b; int8_t x, y;
            read_pad(i, &k, &j);
            n64rt_pad_map(k, j, s0, s1, &b, &x, &y);
            n += snprintf(text + n, sizeof text - (size_t)n, "P%d K %08lX BTN %04X\n  J %02X %02X %02X %02X  S%+03d%+03d\n", i + 1, (unsigned long)k, b,
                          (unsigned)(j & 255u), (unsigned)((j >> 8) & 255u), (unsigned)((j >> 16) & 255u), (unsigned)(j >> 24), x, y);
            if (k != lk[i] || j != lj[i]) changed = 1;
        }
        n += snprintf(text + n, sizeof text - (size_t)n, "TYPE: 0 NONE 1 POCKET\n2 DOCK 3 DOCK WITH STICK\n");
        if (changed && t - t_log > CONFIG_CLOCK_FREQUENCY / 10u) {
            t_log = t;
            for (int i = 0; i < 4; i++) {
                uint32_t k, j;
                read_pad(i, &k, &j);
                if (k == lk[i] && j == lj[i]) continue;
                lk[i] = k; lj[i] = j;
                char l[64];
                snprintf(l, sizeof l, "P%d K=%08lX J=%08lX\n", i + 1, (unsigned long)k, (unsigned long)j);
                puts_(l);
            }
        }
        if (strcmp(text, last) != 0 && t - t_draw > CONFIG_CLOCK_FREQUENCY / 20u) {
            t_draw = t;
            memcpy(last, text, sizeof last);
            notice(LAYOUT_RDRAM, text);
        }
    }
}
