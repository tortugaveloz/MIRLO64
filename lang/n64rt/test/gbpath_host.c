/* The save's path of a Game Boy game from its ROM's path (../n64rt_gbpath.h) on the host:
 *   gcc -O1 -g -fsanitize=address,undefined -I.. gbpath_host.c -o /tmp/gbpath_host && /tmp/gbpath_host */
#include <stdio.h>
#include <string.h>
#include "n64rt_gbpath.h"

static int fails;
static void t(const char *path, int kind, const char *want)
{
    char out[256];
    int k = gbpath_sav(path, out, sizeof out);
    if (k != kind || strcmp(out, want)) { printf("FAIL %s: kind %d \"%s\", want %d \"%s\"\n", path, k, out, kind, want); fails++; }
}
int main(void)
{
    t("/Assets/gbc/common/game.gb", 2, "/Saves/gbc/common/game.sav");           /* the folder wins over the extension */
    t("/Assets/gbc/common/game.gbc", 2, "/Saves/gbc/common/game.sav");
    t("/Assets/gb/common/game.gb", 1, "/Saves/gb/common/game.sav");
    t("/Assets/GBC/common/Game Name.GBC", 2, "/Saves/gbc/common/Game Name.sav");
    t("/Assets/n64/common/game.gb", 1, "/Saves/gb/common/game.sav");           /* no platform folder: the extension */
    t("/Assets/n64/common/game.gbc", 2, "/Saves/gbc/common/game.sav");
    t("/Assets/n64/common/game.v1.gb", 1, "/Saves/gb/common/game.v1.sav");     /* only the last dot is the extension */
    t("/Assets/n64/common/game.rom", 0, "");                                     /* neither */
    t("/Assets/n64/common/", 0, "");                                             /* no file */
    t("/Assets/n64/common/.gb", 1, "/Saves/gb/common/.gb.sav");                  /* (a name that is all dot: the whole is the name) */
    t("", 0, "");
    t("game.gb", 1, "/Saves/gb/common/game.sav");
    t("/Assets/gbx/common/game.gb", 1, "/Saves/gb/common/game.sav");             /* gbx is not a platform: the extension */
    char longp[300], out[256], small[40];
    strcpy(longp, "/Assets/gbc/common/");
    memset(longp + 19, 'a', 230); strcpy(longp + 249, ".gb");
    int k = gbpath_sav(longp, out, sizeof out);
    if (k != 2 || strlen(out) != 18 + 230 + 4) { printf("FAIL long name: %d\n", k); fails++; }       /* a long name that fits */
    k = gbpath_sav(longp, small, sizeof small);
    if (k != 0 || small[0]) { printf("FAIL name too long for the buffer: %d\n", k); fails++; }       /* does not fit: no save */
    k = gbpath_sav("/Assets/gbc/common/12345678901234.gb", small, sizeof small);                       /* 18 + 14 + 4 + NUL = 37 */
    if (k != 2 || strlen(small) != 36) { printf("FAIL name that just fits: %d\n", k); fails++; }
    printf(fails ? "gbpath_host: %d failures\n" : "gbpath_host: ok\n", fails);
    return fails != 0;
}
