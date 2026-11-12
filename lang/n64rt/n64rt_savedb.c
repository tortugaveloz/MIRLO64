/* The save-type database's lookup (n64rt_savedb.h). */
#include "n64rt_savedb.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int blank(char c) { return c == ' ' || c == '\t'; }

/* the `n` characters at `s` are the word `w` (lower case), in any case */
static int is_word(const char *s, unsigned n, const char *w)
{
    unsigned i = 0;
    for (; i < n && w[i]; i++) if (lower((unsigned char)s[i]) != w[i]) return 0;
    return i == n && !w[i];
}

/* a "rev=0,1" field: does `rev` appear (-1: malformed) */
static int rev_has(const char *s, unsigned n, unsigned rev)
{
    int has = 0;
    unsigned i = 4;                         /* after "rev=" */
    if (i >= n) return -1;
    while (i < n) {
        unsigned v = 0, d = 0;
        for (; i < n && s[i] >= '0' && s[i] <= '9'; i++, d++) { v = v * 10u + (unsigned)(s[i] - '0'); if (v > 100000u) v = 100000u; }
        if (!d) return -1;
        if (v == rev) has = 1;
        if (i < n) { if (s[i] != ',') return -1; i++; if (i >= n) return -1; }
    }
    return has;
}

int n64rt_savedb_lookup(const char *db, unsigned len, const unsigned char id[4], unsigned rev)
{
    unsigned i = 0;
    while (i < len) {
        unsigned e = i;                     /* the line is db[i .. e): up to a LF, a CR, a NUL or a "#" */
        while (e < len && db[e] != '\n' && db[e] != '\r' && db[e] != 0 && db[e] != '#') e++;
        const unsigned end = e;
        while (e < len && db[e] != '\n' && db[e] != '\r' && db[e] != 0) e++;        /* (the comment, to the line's end) */
        unsigned p = i;
        i = e + 1;
        /* word 1: the ID */
        while (p < end && blank(db[p])) p++;
        unsigned w = p;
        while (p < end && !blank(db[p])) p++;
        if (p - w != 4) continue;
        const char *pat = db + w;
        /* word 2: the type */
        while (p < end && blank(db[p])) p++;
        unsigned t = p;
        while (p < end && !blank(db[p])) p++;
        int kind;
        if      (is_word(db + t, p - t, "eeprom4k"))  kind = N64RT_SAVEDB_EEP4K;
        else if (is_word(db + t, p - t, "eeprom16k")) kind = N64RT_SAVEDB_EEP16K;
        else if (is_word(db + t, p - t, "sram"))      kind = N64RT_SAVEDB_SRAM;
        else if (is_word(db + t, p - t, "flash"))     kind = N64RT_SAVEDB_FLASH;
        else if (is_word(db + t, p - t, "none"))      kind = N64RT_SAVEDB_NOSAVE;
        else continue;
        /* the ID */
        int hit = 1;
        for (int k = 0; k < 4; k++) if (pat[k] != '?' && lower((unsigned char)pat[k]) != lower(id[k])) hit = 0;
        /* the other fields: rev= narrows the line, the rest is for a later version of the file */
        while (hit && p < end) {
            while (p < end && blank(db[p])) p++;
            w = p;
            while (p < end && !blank(db[p])) p++;
            if (p - w >= 4u && lower((unsigned char)db[w]) == 'r' && lower((unsigned char)db[w + 1]) == 'e' &&
                lower((unsigned char)db[w + 2]) == 'v' && db[w + 3] == '=') {
                int r = rev_has(db + w, p - w, rev);
                if (r <= 0) hit = 0;                                        /* (not for this version, or malformed) */
            }
        }
        if (hit) return kind;
    }
    return N64RT_SAVEDB_NONE;
}

int n64rt_savedb_header(const unsigned char id[4], unsigned rev)
{
    static const signed char kinds[16] = { N64RT_SAVEDB_NOSAVE, N64RT_SAVEDB_EEP4K, N64RT_SAVEDB_EEP16K, N64RT_SAVEDB_SRAM,
                                           N64RT_SAVEDB_SRAM, N64RT_SAVEDB_FLASH, N64RT_SAVEDB_SRAM };    /* (7..15: none yet) */
    if (id[1] != 'E' || id[2] != 'D') return N64RT_SAVEDB_NONE;
    return kinds[(rev >> 4) & 15u];
}
