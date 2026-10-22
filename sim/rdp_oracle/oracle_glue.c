/* The oracle's RDP core in one translation unit, driven one command at a time.
 * Its source is not in this repository: setup.sh clones it into
 * build/oracle/ and the Makefile passes that directory with -I. Everything it
 * needs from a host (messages, worker threads) is stubbed here; it runs single
 * threaded, its commands come from a DMEM-style buffer (the XBUS path), so its
 * own RDRAM holds only the images and the textures. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void msg_error(const char *err, ...)   { va_list a; va_start(a, err); fputs("oracle error: ", stderr); vfprintf(stderr, err, a); fputc('\n', stderr); va_end(a); }
void msg_warning(const char *err, ...) { va_list a; va_start(a, err); fputs("oracle warning: ", stderr); vfprintf(stderr, err, a); fputc('\n', stderr); va_end(a); }
void msg_debug(const char *err, ...)   { (void)err; }
void parallel_init(uint32_t num, bool busy) { (void)num; (void)busy; }
void parallel_run(void task(uint32_t)) { task(0); }
uint32_t parallel_num_workers(void) { return 1; }
void parallel_close(void) { }

/* the LOD hooks (oracle_hooks.patch): the pixel a 2-cycle LOD is computed for, and what it gave */
void orc_lod_hook(int kind, int x, int y, int s0, int t0, int s1, int t1, int s2, int t2,
                  int clamp, int lod, int mag, int dis, int lf, int tile1, int tile2);
static int orc_x, orc_y;
#define ORACLE_AT(x_, y_) (orc_x = (x_), orc_y = (y_))
#define ORACLE_LOD_HOOK(kind, s0, t0, s1, t1, s2, t2, clamp, lod, mag, dis, lf, tile1, tile2) \
    orc_lod_hook(kind, orc_x, orc_y, s0, t0, s1, t1, s2, t2, clamp, lod, mag, dis, lf, tile1, tile2)

#include "n64video.c"

static void mi_cb(void) { }
static uint32_t dp_regs[DP_NUM_REG], vi_regs[VI_NUM_REG], mi_intr;
static uint32_t *dp_ptr[DP_NUM_REG], *vi_ptr[VI_NUM_REG];
static uint32_t dmem_buf[1024];
static uint32_t dp_pos;                     /* bytes fed so far (the DMEM index wraps at 4 KiB) */

/* rdram: 8 MiB as the oracle keeps it (32-bit words in host order) */
void orc_init(uint8_t *rdram)
{
    struct n64video_config c;
    n64video_config_init(&c);
    for (int i = 0; i < DP_NUM_REG; i++) dp_ptr[i] = &dp_regs[i];
    for (int i = 0; i < VI_NUM_REG; i++) vi_ptr[i] = &vi_regs[i];
    c.gfx.rdram = rdram;
    c.gfx.rdram_size = RDRAM_MAX_SIZE;
    c.gfx.dmem = (uint8_t *)dmem_buf;
    c.gfx.dp_reg = dp_ptr;
    c.gfx.vi_reg = vi_ptr;
    c.gfx.mi_intr_reg = &mi_intr;
    c.gfx.mi_intr_cb = mi_cb;
    c.parallel = false;
    c.num_workers = 1;
    memset(dp_regs, 0, sizeof dp_regs);
    dp_regs[DP_STATUS] = DP_STATUS_XBUS_DMA;
    dp_pos = 0;
    n64video_init(&c);
}

/* one complete command, n 32-bit words */
void orc_cmd(const uint32_t *w, int n)
{
    dp_regs[DP_START] = dp_regs[DP_CURRENT] = dp_pos;
    for (int i = 0; i < n; i++) dmem_buf[((dp_pos >> 2) + (uint32_t)i) & 0x3FF] = w[i];
    dp_pos += 4u * (uint32_t)n;
    dp_regs[DP_END] = dp_pos;
    n64video_process_list();
}

/* the two hidden bits of the 16-bit word at RDRAM byte a, as the oracle reads them (a word never written: from its bit 0) */
unsigned orc_hidden(uint32_t a)
{
    uint16_t v; uint8_t h;
    rdram_read_pair16(&v, &h, a >> 1);
    return h & 3u;
}
void orc_set_hidden(uint32_t a, unsigned v) { rdram_hidden[(a >> 1) & (RDRAM_MAX_SIZE / 2 - 1)] = (uint8_t)(v & 3u); }
