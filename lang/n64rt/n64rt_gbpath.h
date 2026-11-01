/* Where a Game Boy game's own save is, from the path the Pocket gives for its ROM (n64rt_gbvirt.c; lang/n64rt/test/gbpath_host.c tests it on the host).
 * The save is <ROM's name without its extension>.sav in the Saves folder of the ROM's platform: /Assets/gbc/common/x.gb -> /Saves/gbc/common/x.sav.
 * The platform is the folder gb or gbc in the ROM's path (/Assets/<platform>/...), else the extension's (.gbc: gbc, .gb: gb). */
#ifndef N64RT_GBPATH_H
#define N64RT_GBPATH_H

typedef unsigned int gbpath_u32;

static inline int gbpath_ci_eq(const char *a, const char *b)      /* b is lower case */
{
    for (; *b; a++, b++) { char c = *a; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != *b) return 0; }
    return !*a;
}

/* `path` (NUL-ended, at most 255 bytes) -> `out` (outsize bytes, at least 256 for any path): the save's path; the result is 1 if the save belongs in the gb
 * folder, 2 in gbc, 0 (and out empty) if the path says neither or is not a file's */
static inline int gbpath_sav(const char *path, char *out, gbpath_u32 outsize)
{
    gbpath_u32 len = 0, base = 0, dot = ~0u;
    out[0] = 0;
    for (; path[len] && len < 255u; len++) {
        if (path[len] == '/') { base = len + 1u; dot = ~0u; }
        else if (path[len] == '.') dot = len;
    }
    const gbpath_u32 end = dot != ~0u && dot > base ? dot : len;
    if (end == base) return 0;
    int kind = 0;
    for (gbpath_u32 i = 0; !kind && i + 4u <= len; i++) {       /* (a folder gb or gbc in the path: its platform) */
        if (path[i] != '/' || (path[i + 1] | 32) != 'g' || (path[i + 2] | 32) != 'b') continue;
        if (path[i + 3] == '/') kind = 1;
        else if ((path[i + 3] | 32) == 'c' && path[i + 4] == '/') kind = 2;
    }
    if (!kind && dot != ~0u) kind = gbpath_ci_eq(path + dot, ".gbc") ? 2 : gbpath_ci_eq(path + dot, ".gb") ? 1 : 0;      /* (else the extension) */
    if (!kind) return 0;
    const char *dir = kind == 2 ? "/Saves/gbc/common/" : "/Saves/gb/common/";
    gbpath_u32 o = 0;
    for (const char *q = dir; *q; ) out[o++] = *q++;
    if (o + (end - base) + 5u > outsize) { out[0] = 0; return 0; }
    for (gbpath_u32 i = base; i < end; i++) out[o++] = path[i];
    for (const char *q = ".sav"; *q; ) out[o++] = *q++;
    out[o] = 0;
    return kind;
}

#endif
