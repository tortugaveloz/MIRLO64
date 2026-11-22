/* gsfile_host: the Game Shark file's path from a ROM's path (n64rt_gsfile.h) on the host.
 *   gcc -O1 -g -fsanitize=address,undefined -I.. gsfile_host.c -o /tmp/gsfile_host && /tmp/gsfile_host */
#include <stdio.h>
#include <string.h>
#include "n64rt_gsfile.h"

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
    printf(fails ? "gsfile_host: %d FAILED\n" : "gsfile_host: all passed\n", fails);
    return fails != 0;
}
