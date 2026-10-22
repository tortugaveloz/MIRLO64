/* Game-CPU frame loop: multi-buffered RDP output, a GDL built every frame
 * and handed to the geometry core over the mailbox, vblank-synced flips.
 *
 *   frame_init();
 *   for (;;) {
 *       gdl_cur_t c;
 *       frame_begin(&c);                 // waits for last frame's geom to finish
 *       ... build the GDL with gdl_* helpers ...
 *       gdl_end(&c);
 *       frame_submit(&c);                // clear back buffer, kick geom, flip on vblank
 *   }
 */
#ifndef GAME_FRAME_H
#define GAME_FRAME_H

#include <stdint.h>
#include "gdl_build.h"

void frame_init(void);
/* 268 (default) or 320: the framebuffer's width, before frame_init() */
void frame_set_video(unsigned hres);
unsigned frame_hres(void);
void frame_begin(gdl_cur_t *c);
void frame_submit(gdl_cur_t *c);
/* Between frame_begin() and frame_submit(): clear this frame to 0xRRGGBB
 * instead of the default (black). */
void frame_set_clear_rgb(uint32_t rgb);

/* current back-buffer base (the render target of the frame in flight) */
uint32_t frame_back_buffer(void);

/* Pipelined submission (opt-in, default off). Off: frame_submit() kicks the
 * geometry core and waits for it, then for RDP, then flips -- every stage
 * idles while the others run. On: frame_submit() only waits for the geom core
 * to finish WALKING the previous list, kicks this one and returns, so the game
 * CPU builds frame k+2 while the geom core walks k+1 and RDP draws k.
 *
 * The clears and the end-of-frame SYNC FULL travel inside the GDL (GDL_RAW),
 * so the geom core puts them in RDP's single command FIFO in stream order;
 * the game CPU never writes that FIFO in this mode. A frame is complete when
 * RDP's sync_count counts its SYNC FULL (every write before it is in DRAM),
 * and frames are flipped to as they complete.
 *
 * Measured on a 762-triangle scene: serial 105 ms/frame, pipelined 93 ms;
 * with 33 ms of game-CPU work per frame, 138 -> 93 ms.
 *
 * frame_sync() drains the pipeline -- call it before reading a frame back,
 * and before changing modes. */
void frame_set_overlap(int on);
void frame_sync(void);
/* Called with the frame's buffer once its writes have landed and BEFORE it is
 * flipped to the display (pipelined mode only). For harnesses that assert on the
 * finished frame while the pipeline keeps running. */
extern void (*frame_preflip_hook)(uint32_t back);
/* Pipelined mode: cycles the game CPU spent in frame_submit() waiting for the
 * geom core to finish the previous list, and for the next frame's buffer to be
 * off the screen (flipped past AND latched). Accumulate; the caller zeroes. */
extern uint64_t frame_pipe_wait_geom_cyc, frame_pipe_wait_buf_cyc;
/* Cycles a caller spent translating its display list into the GDL (e.g. from
 * an N64 display list); the FPS line reports it as part of the game CPU's
 * time. */
extern uint64_t frame_emit_cyc;
uint64_t frame_uptime_cycles(void);
/* How long frame_submit()'s completion wait actually spun, for harnesses that
 * need to tell "the render is wrong" from "the wait returned too early". */
extern uint32_t frame_last_wspin, frame_last_wait_us;

#endif /* GAME_FRAME_H */
