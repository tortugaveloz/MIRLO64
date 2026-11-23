/* Where the Game Shark core's file of codes for a game is, from the path the Pocket gives for the game's ROM (n64rt_cheat.c; lang/n64rt/test/gsfile_host.c
 * tests it on the host). The file is <the ROM's name without its extension>.gs in /Saves/n64/common/: /Assets/n64/common/x.z64 -> /Saves/n64/common/x.gs.
 * Its lines are the format of n64rt_gs.h: `# a cheat's name` comments and the codes to run (`80123456 0063`), only the ones the user has on. */
#ifndef N64RT_GSFILE_H
#define N64RT_GSFILE_H

/* `rom`: the ROM's absolute path (NUL-ended, at most 255 bytes) -> `out` (`outsize` bytes; 280 are enough for any path): the file's path. The result is 1, or 0
 * (and out empty) if the path has no file name or the result does not fit */
static inline int n64rt_gsfile_path(const char *rom, char *out, unsigned outsize)
{
    unsigned len = 0, base = 0, dot = ~0u;
    if (outsize) out[0] = 0;
    for (; rom[len] && len < 255u; len++) {
        if (rom[len] == '/') { base = len + 1u; dot = ~0u; }
        else if (rom[len] == '.') dot = len;
    }
    const unsigned end = dot != ~0u && dot > base ? dot : len;
    static const char dir[] = "/Saves/n64/common/", ext[] = ".gs";
    const unsigned dl = sizeof dir - 1u, el = sizeof ext - 1u;
    if (end == base || dl + (end - base) + el + 1u > outsize) return 0;
    unsigned o = 0;
    for (unsigned i = 0; i < dl; i++) out[o++] = dir[i];
    for (unsigned i = base; i < end; i++) out[o++] = rom[i];
    for (unsigned i = 0; i < el; i++) out[o++] = ext[i];
    out[o] = 0;
    return 1;
}

/* The parameter struct of the Pocket's OPENFILE (docs/mips.md): the path (NUL-ended, at most 255 bytes) in the first 256 bytes, then the flags and the size as 32-bit words at 0x100 and 0x104, and nothing else:
 * all 0x110 bytes are cleared first, so that no tail of an earlier, longer path stays in the struct (n64rt_gspick.c, n64rt_cheat.c). `ps` is the struct's memory (the CPU's words: big-endian on the target). */
static inline void n64rt_open_param(volatile unsigned char *ps, const char *path, unsigned flags, unsigned size)
{
    for (unsigned i = 0; i < 0x110u; i++) ps[i] = 0;
    for (unsigned i = 0; i < 255u && path[i]; i++) ps[i] = (unsigned char)path[i];
    *(volatile unsigned *)(void *)(ps + 0x100u) = flags;
    *(volatile unsigned *)(void *)(ps + 0x104u) = size;
}

#endif
