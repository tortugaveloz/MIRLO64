/* Geometry-core firmware.
 *
 * Boot: run a VPU self-test, then loop: wait for the game CPU's doorbell (the
 * message word is the address, in shared RAM, of a GDL display list), walk it
 * (transform + triangle setup via the VPU, N64 triangle commands to RDP),
 * signal done.
 *
 * BRING-UP: heartbeat progress codes are written to the geom->game mailbox
 * message CSR (mailbox_geom_msg, a plain CSRStorage -- writing it without a
 * kick just updates the register, the game CPU can poll it any time). The
 * game CPU prints mailbox_geom_msg while it waits, so a hang shows exactly
 * which stage the geom core reached. Trap faults show as 0xFA17_xxxx (see
 * start.S).
 */
#include <stdint.h>
#include "geom_vecmath.h"
#include "mailbox.h"
#include "geom_pipeline.h"
#include "geom_dcache.h"

#define HB(code) mmio_w(CSR_MAILBOX_GEOM_MSG_ADDR, (uint32_t)(code))

/* Cheap boot-time sanity check on the fixed-point scalar path (decode ->
 * multiply -> add -> encode) -- catches a broken toolchain/build before the
 * main loop starts trusting it. Used to also matvec-check an identity
 * matrix through vpu_matvec()/vpu_load_matrix(), but that path (software
 * S15.16, geom_vecmath.c) is already exhaustively host-verified
 * (test/test_geom_fixed.c, 500k+ trials, plus the full Fase battery) and
 * vpu_matvec() had no other caller -- dropping it here let gc-sections
 * remove it (and the identity-matrix constant) entirely, real ROM-budget
 * bytes this firmware's 16KB scratchpad genuinely needed. The CFU's own
 * hardware path (geom_vpar.h, the parallel vertex-transform accelerator)
 * is the one actually worth a real bring-up self-test if that's ever
 * needed -- it's untested by geom_vecmath.c's host coverage since it talks
 * to real Vpu4DFixed.v hardware, not software. */
/* The old vpu_selftest() checked 2*3+1 == 7 through vpu_fmul/vpu_fadd, the
 * float-on-integer software routines of the FPU-less era. Nothing uses them
 * any more, the check said nothing about the CFU, and it alone pulled ~800
 * bytes of IEEE conversion code into the 16 KB ROM. The heartbeat keeps its
 * old "ok" value (0x00D0) so nothing reading it changes. */

extern uint32_t geom_tris_emitted;

int main(void)
{
    HB(0xB0000010);                       /* reached main() */


    geom_reset();
    HB(0xB0000030);                        /* geom_reset done */

    for (;;) {
        HB(0xB0000040);                    /* waiting for doorbell */
        uint32_t dl_base = mbox_wait_kick();
        HB(0xB0000050);                    /* got doorbell, acked */

        /* The geom core's D$ is not coherent with the game CPU's writes to
         * the GDL in SDRAM. Clean + invalidate so the walk reads fresh data.
         * (geom_dcache.h) */
        geom_dcache_flush();
        HB(0xB0000060u | (dl_base & 0xFFFFu)); /* past the D$ flush */

        geom_tris_emitted = 0;
        geom_run_display_list((const uint32_t *)(uintptr_t)dl_base);
        HB(0xB0000070);                    /* DL walk complete */

        /* Report triangles actually streamed to RDP this frame (tagged
         * 0xB1xxxxxx) instead of the constant selftest word, so the game CPU
         * can tell an empty frame caused by the geom side (0 triangles) from
         * one caused by the rasterizer (triangles emitted, nothing drawn). */
        mbox_signal_done(0xB1000000u | (geom_tris_emitted & 0xFFFFFFu));
        HB(0xB0000080);                    /* signalled done */
    }
    return 0;
}
