/* GDL_F3D (geom_gdl.h): the geom core processes an F3DEX2 display list
 * itself, as the N64's RSP does, instead of the game CPU translating it.
 *
 * One pass, the RSP's way. The RSP brings the list into its DMEM a piece at a
 * time and hands the RDP its commands from there (the XBUS); here the
 * translator (f3d_emit.h's interface, linked from F3D_EMIT=<f3d_emit.c>)
 * writes its GDL into a small buffer in this core's own RAM, and every few
 * hundred words, at a command boundary (f3d_cmd_boundary), that piece is
 * walked (geom_walk_chunk) -- T&L, triangle setup, RDP -- and the buffer is
 * used again. Nothing of the frame's translation goes through SDRAM: an
 * earlier version wrote it all to SDRAM and walked it after, and was slower
 * than the game CPU translating.
 *
 * The translator works in fixed point only (no float on this core). Its
 * translation cache keeps the translations of the static lists (the game's
 * read-only data, a range the task gives) in an arena the task gives too, and
 * the walk calls them from there (GDL_DL); its big tables live in SDRAM
 * (.sdram_bss, geom_mips.ld), not in the core's 16 KiB of RAM.
 *
 * The task's words (gdl_build.h gdl_f3d(), gdl_f3d_cfg()):
 *   [0] the list's root
 *   [1..3] an N64 game's translator (F3D_N64RAM, n64rt): the microcode's
 *          kind | the state log << 8 | the colour image's width << 16; where
 *          the N64's RDRAM is (SDRAM); its size
 *   [4] [5] the static range [lo, hi)  [6] [7] the cache's arena, its bytes */
#include <stdint.h>
#include <string.h>
#include "geom_gdl.h"
#include "gdl_build.h"
#include "f3d_emit.h"

void geom_walk_chunk(const uint32_t *pc);

static uintptr_t s_static_lo, s_static_hi, s_arena;
static uint32_t  s_arena_bytes;

int f3d_dlc_static(const void *p) { return (uintptr_t)p >= s_static_lo && (uintptr_t)p < s_static_hi; }
void *f3d_dlc_arena(uint32_t *bytes) { *bytes = s_arena_bytes; return (void *)s_arena; }

/* the translator's diagnostics have nowhere to go on this core */
int printf(const char *fmt, ...) { (void)fmt; return 0; }

extern char __sdram_bss_start[], __sdram_bss_end[];

/* The piece being translated, in this core's RAM. A piece is walked once it
 * passes CHUNK_FLUSH words, at the next command boundary: no single command
 * translates to more than CHUNK_WORDS - CHUNK_FLUSH - 1 words (a G_VTX of 32
 * vertices is 129, with the state before a draw it stays well under 256). */
#define CHUNK_WORDS 768u
#define CHUNK_FLUSH 256u
static uint32_t  s_chunk[CHUNK_WORDS];
static gdl_cur_t s_cur;

static void chunk_walk(gdl_cur_t *c)
{
    if (c->p == s_chunk) return;
    if (c->p > c->end) c->p = c->end;         /* (cannot happen: CHUNK_FLUSH's margin) */
    *c->p = GDL_HDR(GDL_END, 0);
    geom_walk_chunk(s_chunk);
    c->p = s_chunk;
}

/* the translator, at a command once out->p has reached f3d_chunk_mark:
 * everything before out->p is complete (its cache translates into its arena,
 * past the mark, with a cursor of its own: not ours) */
uint32_t *f3d_chunk_mark = s_chunk + CHUNK_FLUSH;
void f3d_cmd_boundary(gdl_cur_t *out)
{
    if (out == &s_cur && out->p - s_chunk >= (int)CHUNK_FLUSH) chunk_walk(out);
}

void geom_f3d_task(const uint32_t *a, uint32_t n)
{
    static int s_init;
    if (n < GDL_F3D_WORDS) return;
    if (!s_init) {                            /* the SDRAM tables start zeroed, as .bss */
        memset(__sdram_bss_start, 0, (size_t)(__sdram_bss_end - __sdram_bss_start));
        s_init = 1;
    }
#ifdef F3D_N64RAM
    {
        extern int f3d_gbi1, f3d_fb_hres, f3d_statelog;
        f3d_gbi1 = (int)(a[1] & 0xFFu);
        f3d_statelog = (int)((a[1] >> 8) & 1u);
        f3d_fb_hres = (int)(a[1] >> 16);
        f3d_n64_ram = (uint8_t *)(uintptr_t)a[2];
        f3d_n64_sdram = a[2];
        f3d_n64_ram_size = a[3];
    }
#endif
    s_static_lo = a[4]; s_static_hi = a[5];
    s_arena = a[6]; s_arena_bytes = a[7];
    gdl_begin(&s_cur, s_chunk, CHUNK_WORDS - 1u);   /* (a word kept for the GDL_END) */
    f3d_emit_reset();
    f3d_emit_display_list((const f3d_word_t *)(uintptr_t)a[0], &s_cur);
    chunk_walk(&s_cur);
}
