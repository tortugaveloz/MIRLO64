/* Walk a real F3DEX2 (or, translated, Fast3D) `Gfx` display list and
 * translate it into a flat GDL for the geometry core (lang/c/geom/geom_gdl.h),
 * instead of doing the vertex math on the game CPU. The opcodes and field
 * packings are F3DEX2's (gbi.h under F3DEX_GBI_2).
 *
 * Decomp-header-free: a Gfx command is just two 32-bit words here, Vtx is a
 * raw 16-byte blob, Mtx is 16 s16 + 16 u16. So this compiles for host tests
 * as well as the target.
 */
#ifndef GAME_F3D_EMIT_H
#define GAME_F3D_EMIT_H

#include <stdint.h>
#include "gdl_build.h"

/* Mirlo-N64: the display lists are the unmodified game's, in the N64's RDRAM
 * (word-native, sim/n64/n64.h): w1 is an N64 address -- segmented or KSEG0 --
 * resolved through the segment table the lists set (gsSPSegment). */
typedef struct { uint32_t w0; uint32_t w1; } f3d_word_t;
extern uint8_t *f3d_n64_ram;        /* the RDRAM, as this CPU sees it */
extern uint32_t f3d_n64_ram_size;
extern uint32_t f3d_n64_sdram;      /* the RDRAM's address on RDP's bus */

/* The task's microcode (f3d_ucode_kind): its lists are walked as F3DEX2,
 * the GBI 1 families translated command by command. */
enum { F3D_UC_GBI2 = 0,     /* F3DEX2, F3DZEX2 (and their .NoN / .Rej / fifo builds) */
       F3D_UC_F3D = 1,      /* Fast3D, "RSP SW Version: 2.0x" (SM64, Pilotwings 64) */
       F3D_UC_F3DEX1 = 2,   /* F3DEX / F3DLX / F3DLP 0.9x-1.2x: GBI 1, a 32-vertex cache */
       F3D_UC_S2DEX = 3 };  /* S2DEX: sprites and backgrounds (only its RDP commands for now) */
extern int f3d_gbi1;
extern int f3d_texlog;          /* host tools: printf every GDL_TEXBIND with its source state */
/* Which microcode, from its data segment (the name string the microcode
 * carries): p[i ^ x] is byte i of the data, n its size. */
int f3d_ucode_kind(const uint8_t *p, uint32_t n, uint32_t x);

/* Reset the translator's tracked state (matrix target, geometry mode, tex). */
void f3d_emit_reset(void);

/* The display-list cache (call_dl): a static list's translation is kept and
 * reused while the state it starts in matches. The platform says what is
 * static and lends the arena (weak defaults: nothing is cached);
 * f3d_dlc_flush() forgets everything, for a platform that saw something a
 * translation read change. */
int   f3d_dlc_static(const void *p);
void *f3d_dlc_arena(uint32_t *bytes);
void  f3d_dlc_flush(void);
extern int f3d_dlc_off;
extern unsigned long f3d_dlc_hits, f3d_dlc_misses, f3d_dlc_never, f3d_dlc_words_saved, f3d_dlc_switches;

/* Translate the Gfx list at `dl` into GDL written through `out`. Nested lists
 * (G_DL / G_ENDDL) are flattened here on the game CPU. Does NOT emit a
 * trailing GDL_END -- the caller does that after any extra commands. */
void f3d_emit_display_list(const f3d_word_t *dl, gdl_cur_t *out);
/* After f3d_emit_display_list(): the display list painted a background
 * colour (a full-screen fill before any drawing) -- the caller's clear of
 * this frame should use it. 0xRRGGBB. */
extern int      f3d_clear_valid;
extern uint32_t f3d_clear_rgb;

/* w1 of a G_SPNOOP that makes the 2D after it keep its size on every screen
 * (to the end of the frame): the port's own overlays, e.g. the FPS counter,
 * which the title's squeezed 2D (f3d_keep_2d) cut a column off. The next
 * projection load takes it up. */
#define F3D_TAG_KEEP_2D 0x4D4B3244u   /* "MK2D" */

#endif /* GAME_F3D_EMIT_H */
