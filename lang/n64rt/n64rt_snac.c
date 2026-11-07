/* The SNAC port as the console's PIF: real Joybus devices on the Analogizer's four controller ports (rtl/analogizer/an_snac_pif.sv; docs/analogizer.md).
 *
 * The hardware is a Joybus master with four ports behind it, a 128-byte buffer, and a scan that polls each port's status every 8 ms. This file is the
 * runtime's side of it:
 *   - the switch. The menu's "Analogizer" (INTERACT2 bits 3:0, interact.json id 253): Off (0) leaves the Joybus off, any other value -- a video mode,
 *     or 10, "N64 SNAC", which is no picture -- turns it on. That is all the policy there is, and it is looked at at every run of the PIF's commands (and
 *     at the core's start), so changing the menu while a game runs turns the controllers on or off at the game's next poll. The pins are another matter:
 *     the hardware gives them to the Joybus only while the adapter owns the cartridge port (INTERACT2 bit 31, decided in n64rt_pak.c from the cartridge's
 *     header); with a Game Boy cartridge in the slot they stay the cartridge's -- bank 0 is its /WR, /RD and /CS -- whatever the menu says, and the
 *     scan finds nobody there.
 *   - presence. A port has a device when the scan's last status command was answered; that is read at every run of the PIF's commands.
 *   - a transaction: the game's own command bytes go to the device and its reply, with the device's own CRC byte, comes back as it is
 *     (n64rt_io.c, pif_run). The line time is the real one; the time the runtime waited for it is taken off the console's time for the commands
 *     (SIDONE), so the game is held no longer than the console's PIF would hold it.
 * Where a physical device is present on a port it is that channel; otherwise the channel is the virtual one (the Pocket's pad, the virtual paks). */
#include <generated/csr.h>
#include <generated/soc.h>
#include "n64rt.h"

#define SNAC_ID       0x534E0001u
#define AN_OWN        0x80000000u              /* INTERACT2 bit 31: the adapter owns the cartridge port (n64rt_pak.c) */
#define AN_MENU       0x0000000Fu              /* INTERACT2 bits 3:0: the menu's "Analogizer" (0 Off) */
#define TFIRST_US     64u                      /* the console's wait for a reply's first edge, as the hardware resets it */
#define SCAN_SWEEP_US 26000u                   /* the scan visits a port every 2 ms (four ports in 8 ms): three sweeps are two answers in a row for sure */

static u8  s_probed, s_hw, s_on, s_pres;
static u64 s_wait;                             /* SoC clocks spent in snac_xfer since n64rt_snac_wait_take */
static u64 s_ok[4];                            /* when a transaction on each port was last answered */
#define HOLD_MS 300u                           /* a port the scan lost still counts as there this long after an answered transaction */

static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static void wait_us(u32 us) { const u64 e = now() + (u64)us * (CONFIG_CLOCK_FREQUENCY / 1000000u); while (now() < e) { } }

int n64rt_snac_hw(void)
{
    if (!s_probed) { s_probed = 1; s_hw = snac_id_read() == SNAC_ID; }
    return s_hw;
}

/* follow the menu, and read who is there */
void n64rt_snac_sync(void)
{
    if (!n64rt_snac_hw()) return;
    const u32 i2 = apf_interact_interact2_read();
    const int want = (i2 & AN_MENU) != 0;
    if (want != s_on) {
        s_on = (u8)want;
        snac_ctrl_write(want ? 3u | TFIRST_US << 8 : 0u);
        if (want) wait_us(SCAN_SWEEP_US);        /* (the first sweep of the scan: who is there is known before the game asks) */
        else s_pres = 0;
        n64rt_log("n64rt: SNAC: the Joybus is %s%s\n", want ? "on" : "off", want && !(i2 & AN_OWN) ? " (the pins are a cartridge's: nobody is scanned)" : "");
    }
    if (!s_on) return;
    const u32 st = snac_stat_read();
    s_pres = (u8)(st & 15u);
    /* The scan's hysteresis (three misses in a row) is shorter than a frame: one missed status command would send the game's poll to the
     * virtual channel -- the Pocket's pad, with no input and a pak of its own -- for that poll. A port whose last real transaction was
     * answered a moment ago is still there: if it has gone, its transactions time out and the game sees the wire's "no response". */
    {
        const u64 t = now(), hold = (u64)HOLD_MS * (CONFIG_CLOCK_FREQUENCY / 1000u);
        for (int c = 0; c < 4; c++) if (!((s_pres >> c) & 1u) && s_ok[c] && t - s_ok[c] < hold) s_pres |= (u8)(1u << c);
    }
    if (st & 0xF0u) {
        n64rt_log("n64rt: SNAC: controller port(s) %x changed, present %x\n", (unsigned)(st >> 4) & 15u, (unsigned)s_pres);
        snac_stat_write(st & 0xF0u);             /* (write 1s: clears them) */
    }
}

int n64rt_snac_present(int ch) { return s_on && ch >= 0 && ch < 4 && ((s_pres >> ch) & 1u); }

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
    if (n && !(r & 4) && ch >= 0 && ch < 4) s_ok[ch] = now();
    s_wait += now() - t0;
    return r;
}

u32 n64rt_snac_wait_take(void)
{
    const u64 w = s_wait;
    s_wait = 0;
    return w > 0x3FFFFu ? 0x3FFFFu : (u32)w;
}
