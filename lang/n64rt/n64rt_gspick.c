/* The GameShark picker: with "GameShark" chosen in the core's "Expansion Pak/Gameshark" setting, the runtime shows the game's cheats before the game starts (loader/main.c calls
 * n64rt_gspick() once the ROM is known, at the program's start and at the core's "Restart Game", which runs main() again) and lets the player turn them on and off with the controller
 * as the N64's (n64rt_gspad.h: the D-pad or a stick, L/Z/R, A, C-down for all off). Start saves the choice to the game's own file, /Saves/n64/common/<the ROM's name>.gs (n64rt_gsfile.h:
 * the choice remembered, which this picker reads when it opens and the Game Shark core, a program of its own on this hardware, reads and writes in the same format, n64rt_gsdb.c) and
 * hands the chosen codes to the cheat engine (n64rt_gsact.h: the list in memory, parsed as the engine's reader parses a file; nothing is read from the card when the game starts), which runs
 * them in the game; B saves the same and starts the game with no cheats. The file is written only if the choice changed.
 *
 * The game is found in the cheat database -- the file gameshark.gsdz (the Game Shark database project's format 2: an index sorted by the ROM header's two CRC words and country byte, and each
 * game's block of cheats as one zlib stream; about 0.7 MB for some 800 games), by what the ROM's header says. The Pocket loads it into SDRAM with the core's files at every launch of the core, as a
 * data slot with a file name (data.json's slot LAYOUT_GSDB_SLOT, like the save-type database's), so the picker reads it in place: it searches the index in memory (gsdz/gsdz.c), inflates the game's block
 * (gsdz/tinf: some 2.4 KB of code, 1.4 KB of stack, no window) and takes no command of the Pocket for it. A file that is not there is a size of 0 in the Pocket's table: "database not loaded", and the game
 * starts with no cheats. The player's choice for the game is the .gs file's: a game that has one already shows its cheats on, and the picker writes the whole
 * list back (the lines of the file that no cheat of the database explains are kept).
 *
 * The screen has margins of 3 pixels at the sides and 8 lines above and below (a grid of 39 x 17 characters); a list row that does not fit in the window scrolls smoothly
 * while the cursor is on it (n64rt_gsscroll.h); the other rows are cut at the window's end.
 *
 * The picture is the CSR framebuffer path of the scan-out (the one the runtime's own messages use, loader/main.c stop()): 320 x 240 pixels of RGBA5551 in
 * SDRAM that nothing else uses (layout.h), a text screen of 39 x 17 characters of 8 x 13. When the picker returns the picture is black and the scan-out stays on until the runtime sets it up for
 * the game (n64rt_io_init), whose own picture is shown from its first frame on; the window registers are put back as they were. The file moves with the Pocket's target commands, as the
 * runtime's other files do (n64rt_cheat.c, n64rt_gbvirt.c): the open of the game's slot 0 path (GETFILE), the open of the .gs into the writable slot
 * LAYOUT_GSIO_SLOT, a read, a write; after a write the file is read back (the slot opened again), its codes counted.
 *
 * The runtime's .data and .bss hold nothing of it but a few words: the tables are in the SDRAM above. A restart of the game starts main() again with all of it new. */
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include <system.h>
#include "n64rt.h"
#include "n64rt_gsdb.h"
#include "gsdz/gsdz.h"
#include "n64rt_gsfile.h"
#include "n64rt_gs.h"
#include "n64rt_gsact.h"
#include "n64rt_gsfont.h"
#include "n64rt_gspad.h"
#include "n64rt_gsscroll.h"

int n64rt_cheat_rom_path(char *path);                   /* n64rt_cheat.c: the game ROM's path, as the Pocket has it */

#define IO       ((volatile u8 *)(uintptr_t)LAYOUT_GSIO)
#define PARAM    ((volatile u8 *)(uintptr_t)LAYOUT_GS_PARAM)
#define TXT      ((char *)(uintptr_t)LAYOUT_GSP_TXT)
#define DB       ((const u8 *)(uintptr_t)LAYOUT_GSDB)           /* the database the Pocket loaded (data slot LAYOUT_GSDB_SLOT) */
#define BLOB     ((u8 *)(uintptr_t)LAYOUT_GSP_BLOB)
#define FB       ((volatile u16 *)(uintptr_t)LAYOUT_GSP_FB)
typedef struct { gs_game_t game; gs_state_t st; } gsp_t;
#define P        ((gsp_t *)(uintptr_t)LAYOUT_GSP_GAME)
_Static_assert(sizeof(gsp_t) <= 0x8000u, "the game's cheats fit their area");
_Static_assert(LAYOUT_GSIO + LAYOUT_GSIO_MAX <= LAYOUT_GSP_TXT && LAYOUT_GSP_TXT + LAYOUT_GSIO_MAX <= LAYOUT_GSP_GAME && LAYOUT_GSP_GAME + 0x8000u <= LAYOUT_GSP_WORK && LAYOUT_GSP_WORK + 0x24000u <= LAYOUT_GSP_BLOB &&
               LAYOUT_GSP_BLOB + LAYOUT_GSP_BLOB_MAX <= LAYOUT_GSP_SCRATCH_END && LAYOUT_GSP_SCRATCH_END <= LAYOUT_RDRAM + 0x100000u, "the picker's scratch in the RDRAM does not overlap");
_Static_assert(LAYOUT_GSP_FB >= LAYOUT_STATE + 0x970000u && LAYOUT_GSP_FB + 320u * 240u * 2u <= LAYOUT_GSP_ACTIVE && LAYOUT_GSP_ACTIVE + 0x4200u <= LAYOUT_GBSAVE,
               "the picture and the codes' list are in the save states' tail (a state is at most 0x97_0000 bytes)");
_Static_assert(LAYOUT_GSDB + LAYOUT_GSDB_MAX <= 0x43200000u && LAYOUT_GSDB >= LAYOUT_SAVEDB + LAYOUT_SAVEDB_MAX, "the database is in the hole between the save-type database and 0x4320_0000");

/* The time a target command is given: a Pocket that is well answers in tens of milliseconds (the GETFILE in 20 ms: n64rt_cheat.c), so half a second is generous for a command
 * that opens or reads, and an error is shown at once instead of the picker waiting; a write to the card is given 2 s (n64rt_gbvirt.c gives its writes 2 s too). One try, no more. */
#define CMD_MS       500u
#define CMD_WRITE_MS 2000u
#define HZ ((u32)CONFIG_CLOCK_FREQUENCY)
static u64 now(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
static void settle(void) { for (volatile int i = 0; i < 256; i++) { } }

/* ---- the file --------------------------------------------------------------------------------------------------------------------------------------- */
/* a target command: 0 done, the result code (1 to 7) if the Pocket failed it (3: not found, 4: refused), -1 if it did not answer. Two seconds: the card's own
 * time for a file it creates or writes is the Pocket's */
static int command(void (*issue)(u32), u32 ms)
{
    (void)apf_bridge_status_read();                         /* (a read clears it) */
    issue(1u);
    const u64 end = now() + (u64)(HZ / 1000u) * ms;
    while (!apf_bridge_status_read()) if (now() > end) return -1;
    return (int)(apf_bridge_command_result_code_read() & 7u);
}
/* open a file into a slot: flags bit 0 creates it if it is not there, bit 1 sets its size to `size`. 0 opened, 1 created, 3 not there, 4 refused, -1 no answer */
static int open_file(u32 slot, const char *path, u32 flags, u32 size)
{
    n64rt_open_param(PARAM, path, flags, size);
    flush_cpu_dcache_range((const void *)PARAM, 0x110u);
    apf_bridge_slot_id_write(slot);
    apf_bridge_ram_data_address_write(LAYOUT_GS_PARAM);
    settle();
    const int r = command(apf_bridge_request_openfile_write, CMD_MS);
    settle();
    return r;
}
/* `n` bytes of the slot's open file from `off` into the buffer (0 done) */
static int read_at(u32 slot, u32 buf, u32 off, u32 n)
{
    apf_bridge_slot_id_write(slot);
    apf_bridge_data_offset_write(off);
    apf_bridge_transfer_length_write(n);
    apf_bridge_ram_data_address_write(buf);
    settle();
    const int r = command(apf_bridge_request_read_write, CMD_MS);
    if (r == 0) flush_cpu_dcache_range((const void *)(uintptr_t)buf, n);
    return r;
}
static int write_open(u32 n)
{
    apf_bridge_slot_id_write(LAYOUT_GSIO_SLOT);
    flush_cpu_dcache_range((const void *)IO, n);
    apf_bridge_data_offset_write(0);
    apf_bridge_transfer_length_write(n);
    apf_bridge_ram_data_address_write(LAYOUT_GSIO);
    settle();
    return command(apf_bridge_request_write_write, CMD_WRITE_MS);
}

/* ---- the screen ------------------------------------------------------------------------------------------------------------------------------------- */
#define W 320u
#define H 240u
#define MARGIN_X 3u                     /* the picture's margins: 3 pixels at each side, 8 lines above and below */
#define MARGIN_Y 8u
#define COLS 39u                        /* the text grid in what is left: 314 x 224 pixels of 8 x 13 characters (the 39th column ends at the 315th pixel, the 17th row at line 229) */
#define ROWS 17u
#define WINPX (COLS * GS_FONT_W)
#define RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 3) << 6) | (((b) >> 3) << 1) | 1u))
#define C_BG   RGB(8, 16, 56)
#define C_FG   RGB(230, 230, 240)
#define C_DIM  RGB(120, 130, 170)
#define C_HEAD RGB(255, 200, 40)
#define C_ON   RGB(80, 230, 120)
#define C_SEL_BG RGB(40, 70, 170)
#define C_ERR  RGB(255, 90, 90)

/* the rows of text, each 13 pixel lines: row r is the lines MARGIN_Y + 13 r on */
static void flush_rows(u32 row, u32 n) { flush_cpu_dcache_range((const void *)(FB + (MARGIN_Y + row * GS_FONT_ROWS) * W), n * GS_FONT_ROWS * W * 2u); }
/* a string at (col, row) into `width` columns, moved `off` pixels to the left (a window on it: the part that is out is cut, a blank follows the end), in the colours */
static void textpx(u32 col, u32 row, const char *s, u16 fg, u16 bg, u32 width, u32 off)
{
    u32 n = 0;
    while (s[n]) n++;
    const u32 x0 = MARGIN_X + col * GS_FONT_W, y0 = MARGIN_Y + row * GS_FONT_ROWS, wpx = width * GS_FONT_W;
    for (u32 y = 0; y < GS_FONT_ROWS; y++) {
        volatile u16 *d = FB + (y0 + y) * W + x0;
        for (u32 x = 0; x < wpx; ) {
            const u32 px = x + off, ci = px / GS_FONT_W, sh = px % GS_FONT_W;
            const char ch = ci < n ? s[ci] : ' ';
            const unsigned char g = gs_font[(unsigned char)ch >= 32u && (unsigned char)ch < 127u ? (unsigned char)ch - 32u : 0u][y];
            for (u32 k = sh; k < GS_FONT_W && x < wpx; k++, x++) d[x] = (g >> (7u - k)) & 1u ? fg : bg;
        }
    }
}
/* a string at (col, row), padded with blanks to `width` columns and cut there */
static void textn(u32 col, u32 row, const char *s, u16 fg, u16 bg, u32 width) { textpx(col, row, s, fg, bg, width, 0); }
static unsigned utoa(u32 v, char *out)
{
    char t[12]; unsigned n = 0, i;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
    return n;
}

/* the scan-out's registers as the runtime found them, to put back */
static struct { u32 mode, base, width, scale, hwin, win, rowwords, vtg, dma; } s_vid;
static void video_save(void)
{
    s_vid.mode = video_framebuffer_vtg_mode_read(); s_vid.base = video_framebuffer_dma_base_read(); s_vid.width = video_framebuffer_width_read();
    s_vid.scale = video_framebuffer_scale_read(); s_vid.hwin = video_framebuffer_hwindow_read(); s_vid.win = video_framebuffer_window_read();
    s_vid.rowwords = video_framebuffer_rowwords_read(); s_vid.vtg = video_framebuffer_vtg_enable_read(); s_vid.dma = video_framebuffer_dma_enable_read();
}
/* The way out: the picture goes black and stays on (the scan-out is not switched off and its timing is not changed: the Pocket would show a picture that is lost for a moment), the
 * framebuffer is the picker's, all black, until the runtime sets the scan-out for the game (n64rt_io_init: its own base, mode and width; its picture is the game's from
 * the game's first VI frame on). The window registers are put back to what the runtime had. */
static void video_restore(void)
{
    for (u32 i = 0; i < W * H; i++) FB[i] = RGB(0, 0, 0);
    flush_cpu_dcache_range((const void *)FB, W * H * 2u);
    video_framebuffer_scale_write(s_vid.scale); video_framebuffer_hwindow_write(s_vid.hwin); video_framebuffer_window_write(s_vid.win);
    video_framebuffer_rowwords_write(s_vid.rowwords); video_framebuffer_width_write(s_vid.width);
}
static void video_on(void)
{
    for (u32 i = 0; i < W * H; i++) FB[i] = C_BG;
    flush_cpu_dcache_range((const void *)FB, W * H * 2u);
    video_framebuffer_vtg_mode_write(1);
    video_framebuffer_dma_base_write(LAYOUT_GSP_FB);
    video_framebuffer_vtg_enable_write(1);
    video_framebuffer_dma_enable_write(1);
    video_framebuffer_scale_write(0x200u | 0x400u << 16);                  /* 320 x 240: X_SCALE 0.5, Y_SCALE 1.0, in the 640-clock, 237-line window */
    video_framebuffer_hwindow_write(0u | 640u << 16);
    video_framebuffer_window_write(0u << 8 | 237u << 16);
    video_framebuffer_rowwords_write((((640u - 1u) * 0x200u >> 10) + 8u) / 2u + 1u);
    video_framebuffer_width_write(W);
}

/* ---- the state -------------------------------------------------------------------------------------------------------------------------------------- */
enum { S_NODB, S_NOCHEATS, S_LIST };
static int      s_mode;
static int      s_dbwhy;                     /* why the database was not read: the Pocket's answer, -1 none, -3 empty, -4 over the slot's size_maximum */
static char     s_gs_path[300], s_msg[24];
static u16      s_msg_col;
static gs_rom_t s_rom;
static u32      s_cur, s_top;
static int      s_dirty, s_failed, s_have_file;
static int      s_saved;                     /* the file was written and read back right: the exit shows it a moment */
static u32      s_prev_len;                  /* the file's length now: a write is never shorter (below) */

static void message(const char *m, u16 col) { unsigned n = 0; while (m[n] && n < sizeof s_msg - 1u) { s_msg[n] = m[n]; n++; } s_msg[n] = 0; s_msg_col = col; }
static unsigned ncheats_on(void) { unsigned n = 0; for (u32 j = 0; j < P->game.ncheats; j++) n += P->st.sel[j] != 0; return n; }

/* the rows of the screen: 0 the title, the status and the number of cheats on, 1 the game, 2 .. 13 the list, 14 .. 16 the help (Start and B, the D-pad and the stick, the other buttons) */
#define LIST_ROW0 2u
#define LIST_ROWS 12u
static u64 s_t0;
static u32 s_off;                 /* when the cursor came to its row (the uptime), and the scroll of that row's text now */
static unsigned long ms_since(u64 t0) { return (unsigned long)((now() - t0) / (HZ / 1000u)); }
/* a list row's text: "[x] name <option>" */
static u32 row_text(u32 j, char *buf, u32 size)
{
    const gs_cheat_t *c = &P->game.cheat[j];
    const int on = P->st.sel[j] != 0;
    buf[0] = '['; buf[1] = on ? 'x' : ' '; buf[2] = ']'; buf[3] = ' ';
    u32 l = 4 + gs_cheat_name(c, buf + 4, size - 4);
    if (on && c->optcode >= 0) {
        char lab[72];
        gs_cheat_opt(c, (u32)P->st.sel[j] - 1u, lab, sizeof lab, 0);
        if (l + 4u < size) { buf[l++] = ' '; buf[l++] = '<'; for (u32 i = 0; lab[i] && l + 2u < size; i++) buf[l++] = lab[i]; buf[l++] = '>'; buf[l] = 0; }
    }
    return l;
}
/* the list's row r (0 ..): the cursor's with the scroll `off`, the others cut at the window's end */
static void draw_list_row(u32 r, u32 off)
{
    const u32 j = s_top + r;
    if (j >= P->game.ncheats) { textn(0, LIST_ROW0 + r, "", C_FG, C_BG, COLS); return; }
    char buf[400];
    row_text(j, buf, sizeof buf);
    textpx(0, LIST_ROW0 + r, buf, P->st.sel[j] ? C_ON : C_FG, j == s_cur ? C_SEL_BG : C_BG, COLS, j == s_cur ? off : 0u);
}
/* the scroll of the cursor's row, from the time (n64rt_gsscroll.h); redrawn alone when its offset moves */
static void scroll_tick(void)
{
    if (s_mode != S_LIST) return;
    char buf[400];
    const u32 off = n64rt_gsscroll((u32)ms_since(s_t0), row_text(s_cur, buf, sizeof buf) * GS_FONT_W, WINPX);
    if (off == s_off) return;
    s_off = off;
    draw_list_row(s_cur - s_top, off);
    flush_rows(LIST_ROW0 + s_cur - s_top, 1);
}
static void draw(void)
{
    char buf[96];
    gs_game_t *g = &P->game;
    textn(0, 0, "MIRLO GAME SHARK", C_HEAD, C_BG, 16);                           /* (ours: not the original product's name) */
    textn(17, 0, s_mode == S_LIST ? s_msg : "", s_msg_col, C_BG, 16);
    if (s_mode == S_LIST) {
        u32 l = utoa(ncheats_on(), buf);
        buf[l++] = ' '; buf[l++] = 'o'; buf[l++] = 'n'; buf[l] = 0;
        textn(COLS - 6u, 0, buf, C_ON, C_BG, 6);
    } else textn(COLS - 6u, 0, "", C_FG, C_BG, 6);
    if (s_mode != S_LIST) {
        for (u32 r = 1; r < ROWS - 1u; r++) textn(0, r, "", C_FG, C_BG, COLS);
        textn(0, 1, s_rom.title, C_FG, C_BG, COLS);
        if (s_mode == S_NODB) {
            if (s_dbwhy == 1) textn(0, 4, "The cheat database is not loaded.", C_ERR, C_BG, COLS);
            else if (s_dbwhy == 2) textn(0, 4, "The cheat database is not valid.", C_ERR, C_BG, COLS);
            else if (s_dbwhy == 3) textn(0, 4, "The cheat database is too big.", C_ERR, C_BG, COLS);
            else textn(0, 4, "The game's cheats could not be read.", C_ERR, C_BG, COLS);
            textn(0, 5, "(Assets/n64/common/gameshark.gsdz)", C_DIM, C_BG, COLS);
        } else {
            textn(0, 4, "No codes for this game in the database.", C_FG, C_BG, COLS);
            u32 l = 0; static const char hx[] = "0123456789ABCDEF";
            for (int i = 28; i >= 0; i -= 4) buf[l++] = hx[(s_rom.crc1 >> i) & 15u];
            buf[l++] = '-';
            for (int i = 28; i >= 0; i -= 4) buf[l++] = hx[(s_rom.crc2 >> i) & 15u];
            buf[l] = 0;
            textn(0, 6, buf, C_DIM, C_BG, COLS);
        }
        textn(0, ROWS - 1u, "Starts by itself in 3 s, or A/B/Start", C_DIM, C_BG, COLS);
    } else {
        { u32 i = 0; for (; i < g->name[0] && i < COLS; i++) buf[i] = (char)g->name[1 + i]; buf[i] = 0; }
        textn(0, 1, buf, C_FG, C_BG, COLS);
        for (u32 r = 0; r < LIST_ROWS; r++) draw_list_row(r, s_off);
        textn(0, 14, "Start: play w/ cheats B: play no cheats", C_DIM, C_BG, COLS);      /* (both save the choice; Start runs its cheats, B none) */
        textn(0, 15, "D-pad/stick: move    < > : option", C_DIM, C_BG, COLS);
        textn(0, 16, "A: on/off  L/Z R: page  C-down: all off", C_DIM, C_BG, COLS);
    }
    flush_cpu_dcache_range((const void *)FB, W * H * 2u);
}

/* ---- the game --------------------------------------------------------------------------------------------------------------------------------------- */
/* the choice to the game's file; 1 if it is there now */
static int save_file(void)
{
    s_saved = 0;
    { u32 pl = 0; while (s_gs_path[pl]) pl++;                                  /* never a create on a path that is not the .gs of a game */
      if (pl == 0u || pl >= 250u) { message(pl ? "file name long" : "no file path", C_ERR); return 0; } }
    const u32 n = gs_file_write(&P->game, &P->st, (char *)(uintptr_t)LAYOUT_GSIO, LAYOUT_GSIO_MAX);
    if (!n) { message("file too big", C_ERR); return 0; }
    /* A file that is longer than what is written is not shortened by every Pocket that the set-size flag is given to (what a real one does is not known): the
     * write is as long as the file was, the rest comment lines -- which the runtime's reader (n64rt_gs.c) and this one (gs_file_parse) ignore -- so that no old cheat is
     * left in a tail */
    u32 len = n < s_prev_len ? s_prev_len : n;
    if (len > LAYOUT_GSIO_MAX) len = LAYOUT_GSIO_MAX;
    for (u32 o = n; o < len; ) {
        const u32 left = len - o, line = left >= 80u ? 80u : left;
        char *d = (char *)(uintptr_t)LAYOUT_GSIO + o;
        if (line == 1u) d[0] = '\n';
        else { d[0] = '#'; for (u32 i = 1; i + 1u < line; i++) d[i] = '-'; d[line - 1u] = '\n'; }
        o += line;
    }
    int r = open_file(LAYOUT_GSIO_SLOT, s_gs_path, 3u, len);
    if (r == 0 || r == 1) r = write_open(len);
    if (r == 0) {
        s_prev_len = len;
        printf("n64rt: GameShark: wrote %s (%lu bytes, %lu of padding)\n", s_gs_path, (unsigned long)len, (unsigned long)(len - n));
        /* The file is read back as the picker will read it next time (the same slot, opened again), and its codes counted: the file holds what the player chose */
        unsigned want = 0;
        for (u32 j = 0; j < P->game.ncheats; j++) if (P->st.sel[j]) want += P->game.cheat[j].ncodes;
        u32 fs = 0; unsigned got = 0;
        int rr = open_file(LAYOUT_GSIO_SLOT, s_gs_path, 0u, 0u);
        if (rr == 0) {
            fs = apf_bridge_file_size_read();
            if (fs > LAYOUT_GSIO_MAX) fs = LAYOUT_GSIO_MAX;
            rr = fs ? read_at(LAYOUT_GSIO_SLOT, LAYOUT_GSIO, 0, fs) : -2;
            if (rr == 0) got = n64rt_gs_parse((const char *)(uintptr_t)LAYOUT_GSIO, fs, (n64rt_gs_code_t *)(void *)GSACT->code, GSACT_MAX);        /* (not into BLOB: the game's cheats point into it) */
        }
        printf("n64rt: GameShark: read back from the file: %d, %lu bytes, %u codes (%u wanted)\n", rr, (unsigned long)fs, got, want);
        if (rr != 0 || got != want) {
            char m[24] = "read back: ";
            unsigned l = 11; l += utoa(got, m + l); m[l++] = '/'; l += utoa(want, m + l); m[l] = 0;
            message(rr != 0 ? "read-back failed" : m, C_ERR);
            return 0;
        }
        char m[24]; unsigned l = utoa(got, m); const char *w = got == 1 ? " code saved" : " codes saved"; for (u32 i = 0; w[i]; i++) m[l++] = w[i]; m[l] = 0;
        message(m, C_ON);
        s_saved = 1;
        return 1;
    }
    printf("n64rt: GameShark: could not write %s (%d)\n", s_gs_path, r);
    char m[24] = "save failed ";
    m[12] = r < 0 ? 'N' : (char)('0' + r); m[13] = 0;
    message(m, C_ERR);
    return 0;
}

/* The codes to run in the game, for the cheat engine (n64rt_gsact.h): what Start has chosen -- the list the file would hold, parsed as the engine's own reader parses a file --, or
 * nothing (B, a game with no cheats, no database) */
static unsigned activate(int on)
{
    unsigned c = 0;
    GSACT->magic = 0; GSACT->n = 0;
    if (on && s_mode == S_LIST) {
        const u32 n = gs_file_write(&P->game, &P->st, (char *)(uintptr_t)LAYOUT_GSIO, LAYOUT_GSIO_MAX);
        if (n) c = n64rt_gs_parse((const char *)(uintptr_t)LAYOUT_GSIO, n, (n64rt_gs_code_t *)(void *)GSACT->code, GSACT_MAX);
        n64rt_gsact_set_path(GSACT->path, s_gs_path);
        GSACT->n = c;
        GSACT->magic = GSACT_MAGIC;
    }
    flush_cpu_dcache_range((const void *)GSACT, sizeof(n64rt_gsact_t));
    printf("n64rt: GameShark: %u codes handed to the cheat engine\n", c);
    return c;
}

/* ---- the database in memory ---------------------------------------------------------------------------------------------------------------------- */
/* the ROM's header (the 0x40 bytes of a .z64) -> the game's cheats and the file's choice */
static void load_game(const u8 *head)
{
    s_dirty = 0; s_failed = 0; s_cur = s_top = 0; s_msg[0] = 0;
    memset(P, 0, sizeof *P);
    s_mode = S_NOCHEATS;
    if (!gs_rom_header(head, 0x40u, &s_rom)) return;
    /* The database is in memory, as the Pocket loaded it with the core's files (the slot has a file name, and no file is a size of 0 in the Pocket's table: the save-type database's way).
     * Nothing is asked of the Pocket for it. */
    apf_bridge_slot_id_write(LAYOUT_GSDB_SLOT);
    for (volatile int i = 0; i < 2000; i++) { }                                         /* (the slot's size: read in the bridge's clock) */
    const u32 size = apf_bridge_file_size_read();
    gsdz_t db;
    flush_cpu_dcache_range((const void *)DB, size < LAYOUT_GSDB_MAX ? size : LAYOUT_GSDB_MAX);
    s_dbwhy = size == 0u ? 1 : size > LAYOUT_GSDB_MAX ? 3 : gsdz_open(&db, DB, size) != GSDZ_OK ? 2 : 0;
    if (s_dbwhy) { s_mode = S_NODB; printf("n64rt: GameShark: no database (%d: size %lu)\n", s_dbwhy, (unsigned long)size); return; }
    const int len = gsdz_get(&db, s_rom.crc1, s_rom.crc2, s_rom.country, BLOB, LAYOUT_GSP_BLOB_MAX, 1);
    if (len == GSDZ_ERR_NOTFOUND) return;                                               /* (no cheats for this game) */
    if (len < 0) { s_dbwhy = 4; s_mode = S_NODB; printf("n64rt: GameShark: the game's block: %d\n", len); return; }
    printf("n64rt: GameShark: the game's block of the database: %d bytes\n", len);
    if (!gs_game_parse(BLOB, (uint32_t)len, &P->game) || !P->game.ncheats) return;
    s_mode = S_LIST;
    char rp[256];
    s_gs_path[0] = 0;
    if (n64rt_cheat_rom_path(rp)) n64rt_gsfile_path(rp, s_gs_path, sizeof s_gs_path);
    gs_file_parse(&P->game, "", 0u, &P->st);
    s_have_file = 0; s_prev_len = 0;
    if (!s_gs_path[0]) { message("no file path", C_ERR); return; }
    const int r = open_file(LAYOUT_GSIO_SLOT, s_gs_path, 0u, 0u);
    if (r == 0) {
        u32 fs = apf_bridge_file_size_read();
        if (fs > LAYOUT_GSIO_MAX) { message("file too big", C_ERR); return; }               /* (more than the picker writes: left alone) */
        if (fs && read_at(LAYOUT_GSIO_SLOT, LAYOUT_GSIO, 0, fs) == 0) {
            for (u32 i = 0; i < fs; i++) TXT[i] = (char)IO[i];                       /* (a copy: the parse keeps lines of it) */
            gs_file_parse(&P->game, TXT, fs, &P->st);
            s_have_file = 1; s_prev_len = fs;
            printf("n64rt: GameShark: read %s (%lu bytes, %u cheats on)\n", s_gs_path, (unsigned long)fs, ncheats_on());
        }
    } else if (r != 3) { char m[24] = "file: error "; m[12] = r < 0 ? 'N' : (char)('0' + r); m[13] = 0; message(m, C_ERR); }      /* (3: no file yet; -1: the Pocket did not answer) */
}

/* ---- the buttons ------------------------------------------------------------------------------------------------------------------------------------ */
static u32 inputs(void)
{
    static u32 prev;                                                      /* (what was held a moment ago: the stick's hysteresis) */
    return prev = n64rt_gspad(apf_input_cont1_key_read(), apf_input_cont1_joy_read(), apf_interact_interact0_read(), apf_interact_interact1_read(), prev);
}
static u32 s_held[16];
/* a press, with repeat while held: 1 on the frame it counts */
static int pressed(u32 keys, u32 mask, int repeat)
{
    int idx = 0;
    while (!((mask >> idx) & 1u)) idx++;
    if (!(keys & mask)) { s_held[idx] = 0; return 0; }
    const u32 h = s_held[idx]++;
    return h == 0 || (repeat && h >= 20u && (h - 20u) % 4u == 0);
}
static int keys_step(u32 keys)
{
    gs_game_t *g = &P->game;
    if (s_mode != S_LIST) return 0;
    const u32 rows = LIST_ROWS, nc = g->ncheats;
    int ch = 0;
    if (pressed(keys, GP_UP, 1)) { s_cur = s_cur ? s_cur - 1u : nc - 1u; ch = 2; }
    if (pressed(keys, GP_DOWN, 1)) { s_cur = s_cur + 1u < nc ? s_cur + 1u : 0u; ch = 2; }
    if (pressed(keys, GP_PREV, 1)) { s_cur = s_cur > rows ? s_cur - rows : 0u; ch = 2; }
    if (pressed(keys, GP_NEXT, 1)) { s_cur = s_cur + rows < nc ? s_cur + rows : nc - 1u; ch = 2; }
    if (ch) { if (s_cur < s_top) s_top = s_cur; if (s_cur >= s_top + rows) s_top = s_cur - rows + 1u; s_msg[0] = 0; }
    const gs_cheat_t *c = &g->cheat[s_cur];
    const u32 no = gs_cheat_nopts(c);
    if (pressed(keys, GP_A, 0)) { P->st.sel[s_cur] = P->st.sel[s_cur] ? 0u : 1u; ch = 1; }
    if (no && (pressed(keys, GP_LEFT, 1) | pressed(keys, GP_RIGHT, 1))) {
        const int right = (keys & GP_RIGHT) != 0;
        u32 k = P->st.sel[s_cur] ? (u32)P->st.sel[s_cur] - 1u : 0u;
        k = right ? (k + 1u < no ? k + 1u : 0u) : (k ? k - 1u : no - 1u);
        P->st.sel[s_cur] = (u8)(1u + k);
        ch = 1;
    }
    if (pressed(keys, GP_OFF, 0)) { memset(&P->st.sel, 0, sizeof P->st.sel); ch = 1; }
    if (ch == 1) { s_dirty = 1; s_failed = 0; s_msg[0] = 0; }
    if (ch) { s_t0 = now(); s_off = 0; }                                 /* (the cursor moved, or the row's text changed: its scroll starts again) */
    return ch;
}

/* ---- the picker ------------------------------------------------------------------------------------------------------------------------------------- */
/* `head`: the ROM's first 0x40 bytes, a .z64's. Returns when the game is to start. */
u64 n64rt_picker_cycles;
void n64rt_gspick(const u8 *head)
{
    const u64 t_in = now();
    printf("n64rt: GameShark: the picker\n");
    apf_bridge_savestate_write(0);              /* (no game is running: no save state until it does, n64rt_state.c; after a restart the flag is still the last game's) */
    video_save();
    memset(s_held, 0, sizeof s_held);
    load_game(head);
    video_on();
    s_t0 = now(); s_off = 0;
    draw();
    u32 idle = 0;
    int started = 0;
    u64 next = now();
    for (;;) {
        const u32 keys = inputs();
        if (s_mode != S_LIST && ++idle >= 180u) break;                                  /* (no cheats for this game: on in 3 s) */
        if (s_mode != S_LIST && (pressed(keys, GP_START, 0) | pressed(keys, GP_A, 0) | pressed(keys, GP_B, 0))) break;
        const int start = pressed(keys, GP_START, 0), back = pressed(keys, GP_B, 0);
        if (s_mode == S_LIST && (start || back)) {                                      /* Start: the choice is saved and its codes run; B: it is saved and no code runs */
            if (s_dirty && !s_failed && !save_file()) {                                  /* (the same key again plays without the file) */
                s_failed = 1; message("again: play", C_ERR); draw();
            } else { started = start; break; }
        } else if (keys_step(keys)) draw();
        else scroll_tick();
        next += HZ / 60u;
        while (now() < next) { }
    }
    activate(started);
    message("Starting", C_ON);                                                    /* (Start and B alike: what runs is on the list, not in a counter) */
    s_saved = 0;
    draw();
    for (u32 n = 0; n < 30u || (n < 90u && (inputs() & (GP_START | GP_A | GP_B))); n++) { next += HZ / 60u; while (now() < next) { } }    /* (the message is read; the game does not see the press) */
    video_restore();
    for (u32 i = 0; i < (LAYOUT_GSP_SCRATCH_END - LAYOUT_GSIO) / 4u; i++) ((volatile u32 *)(uintptr_t)LAYOUT_GSIO)[i] = 0;        /* (the RDRAM as it was: the picker's tables and buffers were there) */
    flush_cpu_dcache_range((const void *)(uintptr_t)LAYOUT_GSIO, LAYOUT_GSP_SCRATCH_END - LAYOUT_GSIO);
    apf_bridge_slot_id_write(0);
    n64rt_picker_cycles = now() - t_in;
}
