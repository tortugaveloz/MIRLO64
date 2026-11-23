/* GameShark picker: the cheat database and the game's file of codes, with no hardware in it (n64rt_gsdb.h). */
#include "n64rt_gsdb.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* ---- the ROM's header ---------------------------------------------------------------------------------------------------------- */
int gs_rom_header(const uint8_t *h, uint32_t n, gs_rom_t *r)
{
    uint8_t z[0x40];
    if (n < 0x40u) return 0;
    if (h[0] == 0x80 && h[1] == 0x37 && h[2] == 0x12 && h[3] == 0x40) { for (unsigned i = 0; i < 0x40u; i++) z[i] = h[i]; }
    else if (h[0] == 0x40 && h[1] == 0x12 && h[2] == 0x37 && h[3] == 0x80) { for (unsigned i = 0; i < 0x40u; i++) z[i] = h[(i & ~3u) + 3u - (i & 3u)]; }
    else if (h[0] == 0x37 && h[1] == 0x80 && h[2] == 0x40 && h[3] == 0x12) { for (unsigned i = 0; i < 0x40u; i++) z[i] = h[i ^ 1u]; }
    else return 0;
    r->crc1 = be32(z + 0x10); r->crc2 = be32(z + 0x14); r->country = z[0x3E];
    for (unsigned i = 0; i < 20u; i++) { const uint8_t c = z[0x20 + i]; r->title[i] = c >= 0x20 && c < 0x7F ? (char)c : ' '; }
    r->title[20] = 0;
    for (int i = 19; i >= 0 && r->title[i] == ' '; i--) r->title[i] = 0;
    return 1;
}

/* ---- the database -------------------------------------------------------------------------------------------------------------- */
int gs_db_head(const uint8_t *h, uint32_t n, uint32_t *ngames, uint32_t *index_bytes)
{
    if (n < 16u || h[0] != 'G' || h[1] != 'S' || h[2] != 'D' || h[3] != 'B' || be32(h + 4) != 1u) return 0;
    *ngames = be32(h + 8); *index_bytes = be32(h + 12);
    return *index_bytes == 16u * *ngames;
}

int gs_db_find_fn(gs_entry_fn entry, void *ctx, uint32_t ngames, const gs_rom_t *r, uint32_t *off, uint32_t *len)
{
    uint32_t lo = 0, hi = ngames;
    while (lo < hi) {                                           /* the first entry that is not before (crc1, crc2) */
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint8_t *e = entry(ctx, mid);
        if (!e) return 0;
        const uint32_t a = be32(e), b = be32(e + 4);
        if (a < r->crc1 || (a == r->crc1 && b < r->crc2)) lo = mid + 1u; else hi = mid;
    }
    uint32_t first = ngames, o = 0, l = 0;
    for (uint32_t i = lo; i < ngames; i++) {
        const uint8_t *e = entry(ctx, i);
        if (!e || be32(e) != r->crc1 || be32(e + 4) != r->crc2) break;
        if (first == ngames || e[12] == r->country) { first = i; o = be32(e + 8); l = (uint32_t)e[13] << 16 | be16(e + 14); }
        if (e[12] == r->country) break;
    }
    if (first == ngames) return 0;
    *off = o; *len = l;
    return 1;
}
static const uint8_t *mem_entry(void *ctx, uint32_t i) { return (const uint8_t *)ctx + 16u * i; }
int gs_db_find(const uint8_t *index, uint32_t ngames, const gs_rom_t *r, uint32_t *off, uint32_t *len)
{
    return gs_db_find_fn(mem_entry, (void *)index, ngames, r, off, len);
}

/* ---- a game ------------------------------------------------------------------------------------------------------------------------ */
int gs_game_parse(const uint8_t *blob, uint32_t len, gs_game_t *g)
{
    const uint8_t *p = blob, *const end = blob + len;
    if (p >= end || (uint32_t)(end - p) < 1u + p[0] + 2u) return 0;
    g->name = p; p += 1u + p[0];
    g->ncheats = (uint16_t)be16(p); p += 2;
    if (g->ncheats > GS_MAX_CHEATS) g->ncheats = GS_MAX_CHEATS;                      /* (the rest is not listed) */
    for (unsigned i = 0; i < g->ncheats; i++) {
        gs_cheat_t *c = &g->cheat[i];
        if (p >= end || (uint32_t)(end - p) < 1u + p[0] + 1u) return 0;
        c->name = p; p += 1u + p[0];
        c->ncodes = *p++;
        c->codes = p; c->optcode = -1;
        for (unsigned k = 0; k < c->ncodes; k++) {
            if ((uint32_t)(end - p) < 7u) return 0;
            const uint8_t kind = p[0];
            p += 7;
            if (kind == 1) {
                c->optcode = (int16_t)k;
                if (p >= end) return 0;
                const unsigned no = *p++;
                for (unsigned o = 0; o < no; o++) {
                    if ((uint32_t)(end - p) < 3u || (uint32_t)(end - p) < 3u + p[2]) return 0;
                    p += 3u + p[2];
                }
            } else if (kind != 0) return 0;
        }
    }
    return 1;
}

static unsigned pstr(const uint8_t *s, char *out, unsigned size)
{
    unsigned n = s[0], i;
    if (!size) return 0;
    if (n > size - 1u) n = size - 1u;
    for (i = 0; i < n; i++) out[i] = (char)s[1 + i];
    out[n] = 0;
    return n;
}
unsigned gs_cheat_name(const gs_cheat_t *c, char *out, unsigned size) { return pstr(c->name, out, size); }

/* the code k of a cheat (pointer to its kind byte) */
static const uint8_t *code_at(const gs_cheat_t *c, unsigned k)
{
    const uint8_t *p = c->codes;
    for (unsigned i = 0; i < k; i++) {
        const uint8_t kind = p[0];
        p += 7;
        if (kind == 1) { unsigned no = *p++; while (no--) p += 3u + p[2]; }
    }
    return p;
}
unsigned gs_cheat_nopts(const gs_cheat_t *c) { return c->optcode < 0 ? 0u : code_at(c, (unsigned)c->optcode)[7]; }
unsigned gs_cheat_opt(const gs_cheat_t *c, unsigned k, char *label, unsigned size, uint16_t *value)
{
    if (c->optcode < 0 || k >= gs_cheat_nopts(c)) return 0;
    const uint8_t *p = code_at(c, (unsigned)c->optcode) + 8;
    for (unsigned i = 0; i < k; i++) p += 3u + p[2];
    if (value) *value = (uint16_t)be16(p);
    return pstr(p + 2, label, size);
}

/* ---- the file ------------------------------------------------------------------------------------------------------------------ */
#define GS_MAX_LINES 8192u
enum { L_BLANK, L_COMMENT, L_CODE, L_OTHER };
/* the parse's tables: 144 KB, in SDRAM nothing else uses when the runtime has them (GS_WORK_ADDR: layout.h), in the host test's data */
typedef struct {
    gs_line_t line[GS_MAX_LINES];
    uint8_t   type[GS_MAX_LINES], used[GS_MAX_LINES];
    uint32_t  word[GS_MAX_LINES];
    uint16_t  val[GS_MAX_LINES];
    int16_t   owner[GS_MAX_LINES];          /* the comment line that governs a code line, -1 for none */
} gs_work_t;
#ifdef N64RT_GSDB_IN_RUNTIME
#include <layout.h>
#define GS_WORK_ADDR LAYOUT_GSP_WORK
#endif
#ifdef GS_WORK_ADDR
#define GSW ((gs_work_t *)(uintptr_t)GS_WORK_ADDR)
#else
static gs_work_t s_work_;
#define GSW (&s_work_)
#endif
#define s_line  GSW->line
#define s_type  GSW->type
#define s_used  GSW->used
#define s_word  GSW->word
#define s_val   GSW->val
#define s_owner GSW->owner

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static int is_code(const char *p, unsigned n, uint32_t *w, uint16_t *v)
{
    if (n < 13u || p[8] != ' ') return 0;
    uint32_t a = 0, b = 0;
    for (unsigned i = 0; i < 8; i++) { const int h = hexv(p[i]); if (h < 0) return 0; a = a << 4 | (uint32_t)h; }
    for (unsigned i = 0; i < 4; i++) { const int h = hexv(p[9 + i]); if (h < 0) return 0; b = b << 4 | (uint32_t)h; }
    if (n > 13u && p[13] != ' ' && p[13] != '\t') return 0;
    *w = a; *v = (uint16_t)b;
    return 1;
}
/* a comment's text: after the comment mark and the blanks, without the blanks at the end */
static void comment_text(const gs_line_t *l, const char **t, unsigned *n)
{
    const char *p = l->p; unsigned len = l->n;
    while (len && (*p == '#' || *p == ';' || *p == '/')) { p++; len--; }
    while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    *t = p; *n = len;
}
static int eq(const char *t, unsigned n, const uint8_t *pstr_, int plen, const char *suffix, unsigned slen)
{
    /* t[0..n) equals the Pascal string plus the suffix */
    unsigned pl = (unsigned)plen;
    if (n != pl + slen) return 0;
    for (unsigned i = 0; i < pl; i++) if ((uint8_t)t[i] != pstr_[i]) return 0;
    for (unsigned i = 0; i < slen; i++) if (t[pl + i] != suffix[i]) return 0;
    return 1;
}

void gs_file_parse(const gs_game_t *g, const char *txt, uint32_t len, gs_state_t *st)
{
    unsigned nl = 0;
    for (unsigned i = 0; i < GS_MAX_CHEATS; i++) st->sel[i] = 0;
    st->nextra = 0;
    for (uint32_t pos = 0; pos < len && nl < GS_MAX_LINES; ) {                  /* the lines */
        uint32_t e = pos;
        while (e < len && txt[e] != '\n') e++;
        uint32_t n = e - pos;
        while (n && (txt[pos + n - 1] == '\r' || txt[pos + n - 1] == ' ' || txt[pos + n - 1] == '\t')) n--;
        const char *p = txt + pos;
        while (n && (*p == ' ' || *p == '\t')) { p++; n--; }
        s_line[nl].p = p; s_line[nl].n = (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
        s_used[nl] = 0;
        if (!n) s_type[nl] = L_BLANK;
        else if (*p == '#' || *p == ';' || (n > 1 && p[0] == '/' && p[1] == '/')) s_type[nl] = L_COMMENT;
        else if (is_code(p, s_line[nl].n, &s_word[nl], &s_val[nl])) s_type[nl] = L_CODE;
        else s_type[nl] = L_OTHER;
        nl++;
        pos = e + 1u;
    }
    int owner = -1;
    for (unsigned i = 0; i < nl; i++) { if (s_type[i] == L_COMMENT) owner = (int)i; s_owner[i] = (int16_t)owner; }

    /* 1. a comment that is a cheat's name (and an option's label) makes the lines under it that cheat on */
    for (unsigned i = 0; i < nl; i++) {
        if (s_type[i] != L_COMMENT) continue;
        const char *t; unsigned n;
        comment_text(&s_line[i], &t, &n);
        for (unsigned j = 0; j < g->ncheats; j++) {
            const gs_cheat_t *c = &g->cheat[j];
            int hit = 0;
            if (st->sel[j]) continue;
            if (c->optcode < 0) { if (eq(t, n, c->name + 1, c->name[0], "", 0)) { st->sel[j] = 1; hit = 1; } }
            else {
                const unsigned no = gs_cheat_nopts(c);
                for (unsigned k = 0; k < no && !hit; k++) {
                    char lab[96];
                    const unsigned ll = gs_cheat_opt(c, k, lab, sizeof lab, 0);
                    char suf[110]; unsigned sl = 0;
                    suf[sl++] = ' '; suf[sl++] = '='; suf[sl++] = ' ';
                    for (unsigned q = 0; q < ll; q++) suf[sl++] = lab[q];
                    if (!eq(t, n, c->name + 1, c->name[0], suf, sl)) continue;
                    {   /* two options can have one label: the value of the option's code line under the comment says which */
                        uint16_t ov; int have = 0, same = 1;
                        (void)gs_cheat_opt(c, k, 0, 0, &ov);
                        const uint32_t ow = be32(code_at(c, (unsigned)c->optcode) + 1);
                        for (unsigned q = i + 1; q < nl && s_type[q] != L_COMMENT; q++) if (s_type[q] == L_CODE && s_word[q] == ow) { have = 1; same = s_val[q] == ov; break; }
                        if (have && !same) continue;
                    }
                    st->sel[j] = (uint8_t)(1u + k); hit = 1;
                }
            }
            if (hit) {
                for (unsigned q = i + 1; q < nl; q++) if (s_type[q] == L_CODE && s_owner[q] == (int16_t)i) s_used[q] = 1;
                break;
            }
        }
    }
    /* 2. the lines that are all of a cheat's codes, wherever they are, are that cheat on */
    for (unsigned j = 0; j < g->ncheats; j++) {
        const gs_cheat_t *c = &g->cheat[j];
        int16_t pick[256];
        unsigned opt = 0;
        int ok = c->ncodes > 0;
        if (st->sel[j]) continue;
        for (unsigned k = 0; k < c->ncodes && ok; k++) {
            const uint8_t *cp = code_at(c, k);
            const uint32_t w = be32(cp + 1);
            int found = -1;
            for (unsigned q = 0; q < nl && found < 0; q++) {
                if (s_type[q] != L_CODE || s_used[q] || s_word[q] != w) continue;
                int taken = 0;
                for (unsigned m = 0; m < k; m++) if (pick[m] == (int16_t)q) taken = 1;
                if (taken) continue;
                if (cp[0] == 0) { if (s_val[q] == be16(cp + 5)) found = (int)q; }
                else {
                    const unsigned no = gs_cheat_nopts(c);
                    for (unsigned o = 0; o < no; o++) { uint16_t v; (void)gs_cheat_opt(c, o, 0, 0, &v); if (v == s_val[q]) { found = (int)q; opt = o; break; } }
                }
            }
            if (found < 0) ok = 0; else pick[k] = (int16_t)found;
        }
        if (ok) {
            st->sel[j] = (uint8_t)(c->optcode < 0 ? 1u : 1u + opt);
            for (unsigned k = 0; k < c->ncodes; k++) s_used[pick[k]] = 1;
        }
    }
    /* 3. what is left is kept, with the comment that was over it */
    int emitted = -1;
    for (unsigned i = 0; i < nl && st->nextra + 2u < GS_MAX_EXTRA; i++) {
        if (s_type[i] != L_CODE || s_used[i]) continue;
        if (s_owner[i] >= 0 && s_owner[i] != emitted) { st->extra[st->nextra++] = s_line[s_owner[i]]; emitted = s_owner[i]; }
        st->extra[st->nextra++] = s_line[i];
    }
}

static unsigned put(char *out, unsigned size, unsigned o, const char *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) { if (o + 1u < size) out[o] = s[i]; o++; }
    return o;
}
static unsigned puthex(char *out, unsigned size, unsigned o, uint32_t v, unsigned digits)
{
    for (int i = (int)digits - 1; i >= 0; i--) { const char c = "0123456789ABCDEF"[(v >> (4 * (unsigned)i)) & 15u]; o = put(out, size, o, &c, 1); }
    return o;
}

unsigned gs_file_write(const gs_game_t *g, const gs_state_t *st, char *out, unsigned size)
{
    unsigned o = 0;
    char buf[128];
    o = put(out, size, o, "# Game Shark: ", 14);
    o = put(out, size, o, (const char *)g->name + 1, g->name[0]);
    o = put(out, size, o, "\n", 1);
    for (unsigned j = 0; j < g->ncheats; j++) {
        const gs_cheat_t *c = &g->cheat[j];
        if (!st->sel[j]) continue;
        unsigned n = gs_cheat_name(c, buf, sizeof buf);
        o = put(out, size, o, "# ", 2);
        o = put(out, size, o, buf, n);
        if (c->optcode >= 0) {
            n = gs_cheat_opt(c, (unsigned)st->sel[j] - 1u, buf, sizeof buf, 0);
            o = put(out, size, o, " = ", 3);
            o = put(out, size, o, buf, n);
        }
        o = put(out, size, o, "\n", 1);
        for (unsigned k = 0; k < c->ncodes; k++) {
            const uint8_t *cp = code_at(c, k);
            uint16_t v = (uint16_t)be16(cp + 5);
            if (cp[0] == 1) (void)gs_cheat_opt(c, (unsigned)st->sel[j] - 1u, 0, 0, &v);
            o = puthex(out, size, o, be32(cp + 1), 8);
            o = put(out, size, o, " ", 1);
            o = puthex(out, size, o, v, 4);
            o = put(out, size, o, "\n", 1);
        }
    }
    for (unsigned i = 0; i < st->nextra; i++) { o = put(out, size, o, st->extra[i].p, st->extra[i].n); o = put(out, size, o, "\n", 1); }
    if (o + 1u > size) { if (size) out[0] = 0; return 0; }
    out[o] = 0;
    return o;
}
