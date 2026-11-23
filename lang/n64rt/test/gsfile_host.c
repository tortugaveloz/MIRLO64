/* gsfile_host: the Game Shark file's path from a ROM's path (n64rt_gsfile.h) on the host.
 *   gcc -O1 -g -fsanitize=address,undefined -I.. -I../../mips/include gsfile_host.c -o /tmp/gsfile_host && /tmp/gsfile_host */
#include <stdio.h>
#include <string.h>
#include "n64rt_gsfile.h"
#include "n64rt_gsact.h"

static int fails;
static void t(const char *rom, int want, const char *path)
{
    char out[300];
    memset(out, 'x', sizeof out);
    const int r = n64rt_gsfile_path(rom, out, sizeof out);
    if (r != want || (want && strcmp(out, path)) || (!want && out[0])) { printf("FAIL %s -> %d '%s' (want %d '%s')\n", rom, r, out, want, path); fails++; }
}
int main(void)
{
    t("/Assets/n64/common/Star Game (U) [!].z64", 1, "/Saves/n64/common/Star Game (U) [!].gs");
    t("/Assets/n64/common/game.n64", 1, "/Saves/n64/common/game.gs");
    t("/Assets/n64/common/sub/dir.d/game.v64", 1, "/Saves/n64/common/game.gs");
    t("/Assets/n64/common/noext", 1, "/Saves/n64/common/noext.gs");
    t("/Assets/n64/common/a.b.c.z64", 1, "/Saves/n64/common/a.b.c.gs");
    t("/Assets/n64/common/.hidden", 1, "/Saves/n64/common/.hidden.gs");      /* (a leading dot is part of the name) */
    t("/Assets/n64/common/", 0, "");
    t("", 0, "");
    char big[400], out[300];
    memset(big, 'a', sizeof big); big[300] = 0;                         /* a name past 255 bytes is cut where the Pocket's path ends */
    if (n64rt_gsfile_path(big, out, sizeof out) != 1 || strlen(out) != 18 + 255 + 3) { printf("FAIL long path -> '%s'\n", out); fails++; }
    char small[20];
    if (n64rt_gsfile_path("/x/game.z64", small, sizeof small) != 0 || small[0]) { printf("FAIL small buffer\n"); fails++; }
    if (n64rt_gsfile_path("/x/g.z64", small, 0)) { printf("FAIL zero buffer\n"); fails++; }
    /* paths one after the other, each shorter than the one before (a game after a game, a Load Game after a Load Game): the path in the picker's list for the engine and the OPENFILE parameter struct
     * keep nothing of the earlier ones (a status file of an earlier build was named after "(U) [!].gs" left over from a longer name) */
    {
        static const char *roms[] = { "/Assets/n64/common/Star Game Of The Long Name (U) (V1.1) [!].z64", "/Assets/n64/common/PAL/Star Game (U) [!].z64", "/Assets/n64/common/Medium Name.z64", "/Assets/n64/common/Tiny.z64", "/x/a.z64" };
        static volatile char field[256];
        static volatile unsigned char ps[0x110 + 8];
        memset((void *)field, 'x', sizeof field); memset((void *)ps, 0xAA, sizeof ps);
        for (unsigned k = 0; k < sizeof roms / sizeof roms[0]; k++) {
            char gs[300], back[256];
            if (!n64rt_gsfile_path(roms[k], gs, sizeof gs)) { printf("FAIL path %s\n", roms[k]); fails++; continue; }
            n64rt_gsact_set_path(field, gs);
            const unsigned n = n64rt_gsact_get_path(back, field);
            if (n != strlen(gs) || strcmp(back, gs)) { printf("FAIL path %u: '%s' (want '%s')\n", k, back, gs); fails++; }
            for (unsigned i = (unsigned)strlen(gs); i < 256u; i++) if (field[i] || back[i]) { printf("FAIL path %u: byte %u after the NUL is %02x\n", k, i, (unsigned char)field[i]); fails++; break; }
            n64rt_open_param(ps, gs, 3u, 1234u);
            for (unsigned i = (unsigned)strlen(gs); i < 0x100u; i++) if (ps[i]) { printf("FAIL param %u: byte %u after the NUL is %02x\n", k, i, ps[i]); fails++; break; }
            if (strcmp((const char *)ps, gs) || *(volatile unsigned *)(void *)(ps + 0x100) != 3u || *(volatile unsigned *)(void *)(ps + 0x104) != 1234u || ps[0x108] || ps[0x10F] || ps[0x110] != 0xAA) { printf("FAIL param %u\n", k); fails++; }
        }
        /* the status file's name is that path with "log" after it, as the engine makes it: no tail */
        char back[256]; n64rt_gsact_set_path(field, "/Saves/n64/common/Short.gs");
        n64rt_gsact_get_path(back, field);
        if (strcmp(back, "/Saves/n64/common/Short.gs")) { printf("FAIL short after long\n"); fails++; }
    }
    printf(fails ? "gsfile_host: %d FAILED\n" : "gsfile_host: all passed\n", fails);
    return fails != 0;
}
