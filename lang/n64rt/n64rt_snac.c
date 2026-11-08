/* The SNAC port as the console's PIF: real Joybus devices on the Analogizer's four controller ports (rtl/analogizer/an_snac_pif.sv; docs/analogizer.md).
 *
 * The hardware is a Joybus master with four ports behind it and a 128-byte buffer. This file is the runtime's side of it:
 *   - the switch. The menu's "Analogizer controllers" (INTERACT2 bits 13:10, interact.json id 249) says which of the four players are the SNAC port's:
 *     bit n set, player n + 1 is the real controller on SNAC port n + 1; clear, the Dock's (the Pocket's pad, the virtual paks). 0 (the default) leaves
 *     the Joybus off. It is looked at at every run of the PIF's commands (and at the core's start), so changing the menu while a game runs moves a
 *     player at the game's next poll. The pins are another matter: the hardware gives them to the Joybus only while the adapter owns the cartridge
 *     port (INTERACT2 bit 31, decided in n64rt_pak.c from the cartridge's header); with a Game Boy cartridge in the slot they stay the cartridge's --
 *     bank 0 is its /WR, /RD and /CS -- whatever the menu says, and the players stay the Dock's.
 *   - no scan, no detection. The hardware scan is OFF and the runtime does not look for devices: the menu says where they are. A device that sits on a
 *     port beside the game's own commands has a command start 12 us after the pad's reply to the scan whenever the game's command waits for a scan in
 *     progress, and a pad that close to its last reply can miss the command (the game then sees no-response and pauses); with the wire's traffic the
 *     game's alone that does not happen. A SNAC port with nothing plugged in answers nothing, as a console's does.
 *   - a transaction: the game's own command bytes go to the device and its reply, with the device's own CRC byte, comes back as it is
 *     (n64rt_io.c, pif_run). The line time is the real one; the time the runtime waited for it is taken off the console's time for the commands
 *     (SIDONE), so the game is held no longer than the console's PIF would hold it.
 * Where a player is the SNAC port's it is that channel; otherwise the channel is the virtual one (the Pocket's pad, the virtual paks). */
#include <generated/csr.h>
#include <generated/soc.h>
#include "n64rt.h"

#define SNAC_ID       0x534E0001u
#define AN_OWN        0x80000000u              /* INTERACT2 bit 31: the adapter owns the cartridge port (n64rt_pak.c) */
#define AN_PORTS(w)   (((w) >> 10) & 15u)      /* INTERACT2 bits 13:10: the menu's "Analogizer controllers", bit n = player n + 1 is the SNAC port's */
#define TFIRST_US     64u                      /* the console's wait for a reply's first edge, as the hardware resets it */

static u8  s_probed, s_hw, s_on, s_ports;       /* s_ports: the players that are the SNAC port's now */
static u64 s_wait;                             /* SoC clocks spent in snac_xfer since n64rt_snac_wait_take */

static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }

int n64rt_snac_hw(void)
{
    if (!s_probed) { s_probed = 1; s_hw = snac_id_read() == SNAC_ID; }
    return s_hw;
}

/* follow the menu */
void n64rt_snac_sync(void)
{
    if (!n64rt_snac_hw()) return;
    const u32 i2 = apf_interact_interact2_read();
    const u8 menu = (u8)AN_PORTS(i2);
    const int want = menu != 0;
    if (want != s_on) {
        s_on = (u8)want;
        snac_ctrl_write(want ? 1u | TFIRST_US << 8 : 0u);       /* (the Joybus on, the hardware scan off) */
        n64rt_log("n64rt: SNAC: the Joybus is %s%s\n", want ? "on" : "off", want && !(i2 & AN_OWN) ? " (the pins are a cartridge's: the players stay the Dock's)" : "");
    }
    const u8 ports = (u8)((s_on && (i2 & AN_OWN)) ? menu : 0u);
    if (ports != s_ports) { s_ports = ports; n64rt_log("n64rt: SNAC: players %x are the SNAC port's\n", (unsigned)ports); }
}

int n64rt_snac_present(int ch) { return ch >= 0 && ch < 4 && ((s_ports >> ch) & 1u); }

/* One command to port `ch`: tx[0..txlen) out, up to rxlen bytes back into rx; *got is how many came. Returns 0 clean, else bit 0 nothing answered,
 * bit 1 fewer bytes than asked or the wire never went quiet, bit 2 the hardware did not finish. */
int n64rt_snac_xfer(int ch, const u8 *tx, u32 txlen, u8 *rx, u32 rxlen, u32 *got)
{
    const u64 t0 = now();
    snac_ptr_write(0);
    for (u32 i = 0; i < txlen; i++) snac_data_write(tx[i]);
    snac_xfer_write((u32)ch << 16 | rxlen << 8 | txlen);
    u32 s;
    const u64 limit = t0 + (u64)(CONFIG_CLOCK_FREQUENCY / 1000u) * 12u;       /* (63 bytes each way is 4 ms) */
    while ((s = snac_xfer_read()) >> 31) if (now() > limit) break;
    int r = 0;
    u32 n = s & 63u;
    if (s >> 31) { r = 4; n = 0; }
    else {
        if (s & (1u << 30)) r |= 1;
        if (s & (1u << 29)) r |= 2;
    }
    if (n > rxlen) n = rxlen;
    snac_ptr_write(0);
    for (u32 i = 0; i < n; i++) rx[i] = (u8)snac_data_read();
    *got = n;
    s_wait += now() - t0;
    return r;
}

u32 n64rt_snac_wait_take(void)
{
    const u64 w = s_wait;
    s_wait = 0;
    return w > 0x3FFFFu ? 0x3FFFFu : (u32)w;
}
