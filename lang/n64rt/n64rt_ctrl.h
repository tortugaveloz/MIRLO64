/* The controller layouts a game can have (n64rt_pad.h's remap table with the settings that go with it): the preset families, which
 * family a cartridge is, the record a game's own layout is saved as (the Pocket's Controls slot, a file per game next to its .sav) and
 * how the menu's picks lay over them. Pure functions of their arguments (no hardware), so that test/ctrl_host.c can run them on the
 * host (test/ctrl_test.py).
 *
 * A layout is: the Shift key (set0 [14:10]), the stick's dead zone and range (set0 [29:20]) and the table's
 * 30 fields (map[5]). Every other setting (crops, pak) stays the menu's. A family sets the Shift key and the table, not the stick's
 * feel (dead zone, range): those are the player's; a game's saved layout has all of them.
 *
 * The menu has four layout lists (INTERACT2 [4:0] .. [19:15]): Normal (the face keys, shoulders and Select), Shift (the face keys and
 * shoulders while the Shift key is held), Shift: D-pad and D-pad keys. Where a list is on Auto (0), the layout comes from, in this order:
 * the game's saved layout, the family the cartridge is in, Standard. A list that names a layout puts it over that. Save keeps what
 * the pad mapping got (the layout and the picks together), Forget deletes the record. */
#ifndef N64RT_CTRL_H
#define N64RT_CTRL_H
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t set0, set1, m0, m1, map[5]; int mod_default; } n64rt_ctrl_t;   /* m0, m1: which bits of set0 / set1 the layout owns (a family's: the Modifier only);
                                                                                          * mod_default: its Modifier is a default, the menu's wins if it names one */
#define N64RT_CTRL_MOD   (31u << 10)                 /* set0: the Modifier */
#define N64RT_CTRL_MASK0 (N64RT_CTRL_MOD | (0x3FFu << 20))   /* ... and the stick's dead zone [24:20] and range [29:25] */
#define N64RT_CTRL_MASK1 0u                          /* set1: no bit of it is part of a layout */
#define N64RT_CTRL_BYTES 64u                     /* the Controls slot's file */
#define N64RT_CTRL_MAGIC 0x4D435431u             /* "MCT1" */

enum { N64RT_FAM_STANDARD, N64RT_FAM_PLATFORMER, N64RT_FAM_SHOOTER, N64RT_FAM_SKATE, N64RT_FAM_RACING, N64RT_FAM_FIGHTER,
       N64RT_FAMILIES };

/* the table's fields (n64rt_pad.h) */
enum { F_A, F_B, F_X, F_Y, F_L, F_R, F_ZL, F_ZR, F_SELECT, F_START, F_L3, F_R3, F_DPAD, F_LSTICK, F_RSTICK, F_HELD = 15 };
/* the outputs of a key's field, and a group's */
enum { O_DFLT = 0, O_A, O_B, O_Z, O_START, O_L, O_R, O_CU, O_CD, O_CL, O_CR, O_DU, O_DD, O_DL, O_DR, O_SAME, O_NONE = 31 };
enum { G_SETTINGS = 0, G_STICK, G_HALF, G_DPAD, G_C, G_NONE, G_SAME, G_BOTH };   /* G_BOTH: the stick and the N64 D-pad (the D-pad keys only) */
#define N64RT_MOD_SELECT 6u                      /* the Modifier setting's code for the Pocket's Select */

static inline void n64rt_ctrl_pack(n64rt_ctrl_t *c, const uint8_t f[30])
{
    for (int w = 0; w < 5; w++) {
        uint32_t v = 0;
        for (int i = 0; i < 6; i++) v |= (uint32_t)(f[6 * w + i] & 31u) << (5 * i);
        c->map[w] = v;
    }
}

/* family `fam`'s layout. They are starting points for the Pocket's keys, not a verified setup per game: the Modifier is Select (so
 * that the shoulders keep their jobs); holding it gives the second layer. */
static inline void n64rt_ctrl_preset(unsigned fam, n64rt_ctrl_t *c)
{
    uint8_t f[30];
    memset(f, 0, sizeof f);
    memset(c, 0, sizeof *c);
    c->m0 = N64RT_CTRL_MOD;                      /* a family owns the Modifier, and only that: the stick's feel is the player's */
    c->mod_default = 1;                          /* the family's Modifier (Select) is for a player who has not chosen one in the menu */
    switch (fam) {
    case N64RT_FAM_PLATFORMER:                   /* Mario 64, Banjo, Zelda ... : A, B, C-left (Y), C-right (X); Select + X / Y / D-pad: the rest */
    case N64RT_FAM_SHOOTER:                      /* the same, and the trigger on R (Z), L is the N64's R (aim) */
        c->set0 = N64RT_MOD_SELECT << 10;
        f[F_HELD + F_X] = O_CU;
        f[F_HELD + F_Y] = O_CD;
        f[F_HELD + F_A] = O_SAME; f[F_HELD + F_B] = O_SAME;      /* (the Held layer's own A, B are C-right, C-down) */
        f[F_HELD + F_L] = O_L;                   /* Select + L: the N64's L */
        f[F_HELD + F_DPAD] = G_C;                /* Select + D-pad: the C buttons */
        if (fam == N64RT_FAM_SHOOTER) { f[F_R] = O_Z; f[F_L] = O_R; }
        break;
    case N64RT_FAM_SKATE:                        /* Tony Hawk ...: the C buttons on the face keys, Select + A / B: the N64's A / B */
        c->set0 = N64RT_MOD_SELECT << 10;
        f[F_A] = O_CR; f[F_B] = O_CD; f[F_X] = O_CU; f[F_Y] = O_CL;
        f[F_HELD + F_A] = O_A; f[F_HELD + F_B] = O_B;
        f[F_HELD + F_X] = O_CU; f[F_HELD + F_Y] = O_CL;
        break;
    case N64RT_FAM_RACING:                       /* Mario Kart ...: the default keys; Select + D-pad steers gently, Select + X / Y: C-up / C-down */
        c->set0 = N64RT_MOD_SELECT << 10;
        f[F_HELD + F_A] = O_SAME; f[F_HELD + F_B] = O_SAME;
        f[F_HELD + F_X] = O_CU;
        f[F_HELD + F_Y] = O_CD;
        break;
    case N64RT_FAM_FIGHTER:                      /* Smash ...: C-up (jump) on X, Z (grab) on L and Y */
        f[F_X] = O_CU;
        f[F_Y] = O_Z;
        f[F_ZL] = O_L;
        break;
    default: break;                              /* Standard: the settings' defaults, with no Modifier */
    }
    n64rt_ctrl_pack(c, f);
}

/* ---- which family a cartridge is: its ID's first three characters (the fourth is the region) ---- */
typedef struct { char id[4]; uint8_t fam; } n64rt_ctrl_game_t;
#define N64RT_CG(i, f) { i, f }
static const n64rt_ctrl_game_t n64rt_ctrl_games[] = {
    /* platformers and adventures */
    N64RT_CG("NSM", N64RT_FAM_PLATFORMER), N64RT_CG("NBK", N64RT_FAM_PLATFORMER), N64RT_CG("NB7", N64RT_FAM_PLATFORMER), N64RT_CG("NDO", N64RT_FAM_PLATFORMER),
    N64RT_CG("NFU", N64RT_FAM_PLATFORMER), N64RT_CG("NGV", N64RT_FAM_PLATFORMER), N64RT_CG("NY2", N64RT_FAM_PLATFORMER), N64RT_CG("NYS", N64RT_FAM_PLATFORMER),
    N64RT_CG("NK4", N64RT_FAM_PLATFORMER), N64RT_CG("NMQ", N64RT_FAM_PLATFORMER), N64RT_CG("CZL", N64RT_FAM_PLATFORMER), N64RT_CG("NZL", N64RT_FAM_PLATFORMER),
    N64RT_CG("NZS", N64RT_FAM_PLATFORMER), N64RT_CG("NG5", N64RT_FAM_PLATFORMER), N64RT_CG("NBC", N64RT_FAM_PLATFORMER), N64RT_CG("NSC", N64RT_FAM_PLATFORMER),
    N64RT_CG("NFX", N64RT_FAM_PLATFORMER), N64RT_CG("NPW", N64RT_FAM_PLATFORMER), N64RT_CG("ND3", N64RT_FAM_PLATFORMER), N64RT_CG("ND4", N64RT_FAM_PLATFORMER),
    N64RT_CG("NBH", N64RT_FAM_PLATFORMER), N64RT_CG("NBM", N64RT_FAM_PLATFORMER), N64RT_CG("NBD", N64RT_FAM_PLATFORMER),
    /* shooters */
    N64RT_CG("NGE", N64RT_FAM_SHOOTER), N64RT_CG("NPD", N64RT_FAM_SHOOTER), N64RT_CG("NT2", N64RT_FAM_SHOOTER), N64RT_CG("NTU", N64RT_FAM_SHOOTER),
    N64RT_CG("NTK", N64RT_FAM_SHOOTER), N64RT_CG("NRW", N64RT_FAM_SHOOTER), N64RT_CG("NDM", N64RT_FAM_SHOOTER), N64RT_CG("NQK", N64RT_FAM_SHOOTER),
    N64RT_CG("NQ2", N64RT_FAM_SHOOTER), N64RT_CG("NHX", N64RT_FAM_SHOOTER), N64RT_CG("NDN", N64RT_FAM_SHOOTER), N64RT_CG("NDZ", N64RT_FAM_SHOOTER),
    N64RT_CG("NJF", N64RT_FAM_SHOOTER), N64RT_CG("NAR", N64RT_FAM_SHOOTER), N64RT_CG("NRS", N64RT_FAM_SHOOTER), N64RT_CG("NSD", N64RT_FAM_SHOOTER),
    N64RT_CG("NWD", N64RT_FAM_SHOOTER), N64RT_CG("NHV", N64RT_FAM_SHOOTER),
    /* skate and snowboard tricks */
    N64RT_CG("NTF", N64RT_FAM_SKATE), N64RT_CG("NTQ", N64RT_FAM_SKATE), N64RT_CG("N3T", N64RT_FAM_SKATE), N64RT_CG("NTE", N64RT_FAM_SKATE),
    N64RT_CG("NSK", N64RT_FAM_SKATE), N64RT_CG("NK2", N64RT_FAM_SKATE), N64RT_CG("NSB", N64RT_FAM_SKATE),
    /* racing */
    N64RT_CG("NKT", N64RT_FAM_RACING), N64RT_CG("NDY", N64RT_FAM_RACING), N64RT_CG("NFZ", N64RT_FAM_RACING), N64RT_CG("CFZ", N64RT_FAM_RACING),
    N64RT_CG("NWR", N64RT_FAM_RACING), N64RT_CG("NR2", N64RT_FAM_RACING), N64RT_CG("NSF", N64RT_FAM_RACING), N64RT_CG("NRU", N64RT_FAM_RACING),
    N64RT_CG("NB8", N64RT_FAM_RACING), N64RT_CG("NNS", N64RT_FAM_RACING), N64RT_CG("NRZ", N64RT_FAM_RACING), N64RT_CG("NEG", N64RT_FAM_RACING),
    N64RT_CG("NG2", N64RT_FAM_RACING), N64RT_CG("NTR", N64RT_FAM_RACING), N64RT_CG("NL2", N64RT_FAM_RACING), N64RT_CG("NGR", N64RT_FAM_RACING),
    N64RT_CG("NRC", N64RT_FAM_RACING), N64RT_CG("NWP", N64RT_FAM_RACING), N64RT_CG("NHW", N64RT_FAM_RACING), N64RT_CG("NCR", N64RT_FAM_RACING),
    N64RT_CG("NCU", N64RT_FAM_RACING), N64RT_CG("NCW", N64RT_FAM_RACING), N64RT_CG("NXO", N64RT_FAM_RACING), N64RT_CG("NLG", N64RT_FAM_RACING),
    N64RT_CG("NMX", N64RT_FAM_RACING), N64RT_CG("NVL", N64RT_FAM_RACING), N64RT_CG("NVY", N64RT_FAM_RACING), N64RT_CG("NPR", N64RT_FAM_RACING),
    N64RT_CG("NGB", N64RT_FAM_RACING),
    /* fighters */
    N64RT_CG("NAL", N64RT_FAM_FIGHTER), N64RT_CG("NM4", N64RT_FAM_FIGHTER), N64RT_CG("NMK", N64RT_FAM_FIGHTER), N64RT_CG("NKI", N64RT_FAM_FIGHTER),
    N64RT_CG("NKA", N64RT_FAM_FIGHTER), N64RT_CG("NFG", N64RT_FAM_FIGHTER), N64RT_CG("NDK", N64RT_FAM_FIGHTER), N64RT_CG("NWA", N64RT_FAM_FIGHTER),
    N64RT_CG("NME", N64RT_FAM_FIGHTER), N64RT_CG("NC2", N64RT_FAM_FIGHTER), N64RT_CG("NCF", N64RT_FAM_FIGHTER), N64RT_CG("NW2", N64RT_FAM_FIGHTER),
    N64RT_CG("NW4", N64RT_FAM_FIGHTER), N64RT_CG("NWN", N64RT_FAM_FIGHTER), N64RT_CG("NWM", N64RT_FAM_FIGHTER), N64RT_CG("NWW", N64RT_FAM_FIGHTER),
    N64RT_CG("NTI", N64RT_FAM_FIGHTER), N64RT_CG("NWV", N64RT_FAM_FIGHTER), N64RT_CG("NWX", N64RT_FAM_FIGHTER), N64RT_CG("NMY", N64RT_FAM_FIGHTER),
    N64RT_CG("NRD", N64RT_FAM_FIGHTER), N64RT_CG("N22", N64RT_FAM_FIGHTER), N64RT_CG("NBF", N64RT_FAM_FIGHTER),
};
#undef N64RT_CG
/* the family of the cartridge whose ID is `id` (four characters), or -1 */
static inline int n64rt_ctrl_family(const unsigned char id[4])
{
    for (unsigned i = 0; i < sizeof n64rt_ctrl_games / sizeof n64rt_ctrl_games[0]; i++)
        if ((unsigned char)n64rt_ctrl_games[i].id[0] == id[0] && (unsigned char)n64rt_ctrl_games[i].id[1] == id[1]
            && (unsigned char)n64rt_ctrl_games[i].id[2] == id[2]) return n64rt_ctrl_games[i].fam;
    return -1;
}

/* ---- a game's saved layout: 16 words (64 bytes) of the Controls slot: the magic, set0 and set1 (masked), the table, a checksum ---- */
static inline uint32_t n64rt_ctrl_sum(const uint32_t *w)
{
    uint32_t s = 0x4C525443u;
    for (int i = 0; i < 8; i++) s = (s << 5 | s >> 27) ^ w[i];
    return s;
}
static inline void n64rt_ctrl_store(volatile uint32_t *mem, const n64rt_ctrl_t *c)
{
    uint32_t w[16];
    memset(w, 0, sizeof w);
    w[0] = N64RT_CTRL_MAGIC;
    w[1] = c->set0 & N64RT_CTRL_MASK0;
    w[2] = c->set1 & N64RT_CTRL_MASK1;
    for (int i = 0; i < 5; i++) w[3 + i] = c->map[i];
    w[8] = n64rt_ctrl_sum(w);
    for (int i = 0; i < 16; i++) mem[i] = w[i];
}
/* 1 and the layout, or 0 (no file, another format, a damaged one) */
static inline int n64rt_ctrl_load(const volatile uint32_t *mem, unsigned file_bytes, n64rt_ctrl_t *c)
{
    if (file_bytes < 36u) return 0;
    uint32_t w[9];
    for (int i = 0; i < 9; i++) w[i] = mem[i];
    if (w[0] != N64RT_CTRL_MAGIC || w[8] != n64rt_ctrl_sum(w)) return 0;
    c->set0 = w[1] & N64RT_CTRL_MASK0;
    c->set1 = w[2] & N64RT_CTRL_MASK1;
    c->m0 = N64RT_CTRL_MASK0;
    c->m1 = N64RT_CTRL_MASK1;
    c->mod_default = 0;
    for (int i = 0; i < 5; i++) c->map[i] = w[3 + i];
    return 1;
}

/* ---- the menu's picks ---- */
typedef struct { unsigned normal, shift, shift_dpad, dpad_keys, sticks; } n64rt_ctrl_pick_t;   /* sticks: 1 is the default (the dock pad's own) */
enum { N64RT_PICK_NORMAL = 5, N64RT_PICK_SHIFT = 5, N64RT_PICK_SDPAD = 6, N64RT_PICK_DKEYS = 4 };   /* how many entries each list has (Auto first) */

/* Normal: the outputs of A, B, X, Y, L, R, Select; entry 1.. of the list ("A B cR cL / Z R", "cR cD cU cL / Z R", "A B cR cL / R Z", "A B cU Z / Z R") */
static const uint8_t n64rt_ctrl_normal[N64RT_PICK_NORMAL - 1][7] = {
    { O_A,  O_B,  O_CR, O_CL, O_Z, O_R, O_CD   },
    { O_CR, O_CD, O_CU, O_CL, O_Z, O_R, O_NONE },
    { O_A,  O_B,  O_CR, O_CL, O_R, O_Z, O_CD   },
    { O_A,  O_B,  O_CU, O_Z,  O_Z, O_R, O_CD   },
};
static inline void n64rt_ctrl_setf(uint32_t map[5], unsigned f, unsigned v)
{
    map[f / 6u] = (map[f / 6u] & ~(31u << (5u * (f % 6u)))) | ((v & 31u) << (5u * (f % 6u)));
}
/* the picks over a layout's table: a list that is on Auto (0), or out of range, changes nothing */
static inline void n64rt_ctrl_overlay(const n64rt_ctrl_pick_t *pk, uint32_t map[5])
{
    if (pk->normal >= 1u && pk->normal < N64RT_PICK_NORMAL) {
        const uint8_t *o = n64rt_ctrl_normal[pk->normal - 1u];
        for (unsigned i = 0; i < 6; i++) n64rt_ctrl_setf(map, i, o[i]);            /* A B X Y L R */
        n64rt_ctrl_setf(map, F_SELECT, o[6]);
    }
    switch (pk->shift) {
    case 1: for (unsigned i = 0; i < 6; i++) n64rt_ctrl_setf(map, F_HELD + i, O_SAME); break;                   /* same as normal */
    case 2: n64rt_ctrl_setf(map, F_HELD + F_A, O_CR); n64rt_ctrl_setf(map, F_HELD + F_B, O_CD);
            n64rt_ctrl_setf(map, F_HELD + F_X, O_CU); n64rt_ctrl_setf(map, F_HELD + F_Y, O_CL); break;           /* cR cD cU cL */
    case 3: n64rt_ctrl_setf(map, F_HELD + F_A, O_A);  n64rt_ctrl_setf(map, F_HELD + F_B, O_B);
            n64rt_ctrl_setf(map, F_HELD + F_X, O_CU); n64rt_ctrl_setf(map, F_HELD + F_Y, O_CD); break;           /* A B cU cD */
    case 4: n64rt_ctrl_setf(map, F_HELD + F_L, O_L);  n64rt_ctrl_setf(map, F_HELD + F_R, O_R); break;           /* / L R */
    default: break;
    }
    static const uint8_t sd[N64RT_PICK_SDPAD] = { 0, G_SAME, G_C, G_HALF, G_DPAD, G_BOTH };
    if (pk->shift_dpad >= 1u && pk->shift_dpad < N64RT_PICK_SDPAD) n64rt_ctrl_setf(map, F_HELD + F_DPAD, sd[pk->shift_dpad]);
    static const uint8_t dk[N64RT_PICK_DKEYS] = { 0, G_DPAD, G_BOTH, G_C };
    if (pk->dpad_keys >= 1u && pk->dpad_keys < N64RT_PICK_DKEYS) n64rt_ctrl_setf(map, F_DPAD, dk[pk->dpad_keys]);
    /* Analog sticks (a dock pad): the left and right stick groups, in both layers; 1, or a value beyond the list, changes nothing */
    static const uint8_t st[6][2] = { { G_STICK, G_NONE }, { 0, 0 }, { G_HALF, G_C }, { G_DPAD, G_C }, { G_C, G_STICK }, { G_NONE, G_NONE } };
    if (pk->sticks < 6u && pk->sticks != 1u)
        for (unsigned g = 0; g < 2; g++) {
            n64rt_ctrl_setf(map, F_LSTICK + g, st[pk->sticks][g]);
            n64rt_ctrl_setf(map, F_HELD + F_LSTICK + g, G_SAME);
        }
}
/* the settings the pad mapping reads: the menu's words with the layout's parts put in */
static inline void n64rt_ctrl_apply(const n64rt_ctrl_t *c, uint32_t *set0, uint32_t *set1, uint32_t map[5])
{
    if (!c) return;
    uint32_t m0 = c->m0;
    if (c->mod_default && ((*set0 >> 10) & 31u) != 0u) m0 &= ~N64RT_CTRL_MOD;      /* (the menu's Shift key, if it names a key, over a family's) */
    *set0 = (*set0 & ~m0) | (c->set0 & m0);
    *set1 = (*set1 & ~c->m1) | (c->set1 & c->m1);
    for (int i = 0; i < 5; i++) map[i] = c->map[i];
}

/* ---- what the pad mapping uses: the game's saved layout, else its family's, else Standard, with the menu's picks over it ---- */
typedef struct { n64rt_ctrl_t saved; int saved_ok, family; } n64rt_ctrl_state_t;

static inline void n64rt_ctrl_effective(const n64rt_ctrl_state_t *st, const n64rt_ctrl_pick_t *pk, uint32_t *set0, uint32_t *set1, uint32_t map[5])
{
    n64rt_ctrl_t tmp;
    const n64rt_ctrl_t *c = &st->saved;
    if (!st->saved_ok) { n64rt_ctrl_preset(st->family >= 0 ? (unsigned)st->family : N64RT_FAM_STANDARD, &tmp); c = &tmp; }
    n64rt_ctrl_apply(c, set0, set1, map);
    n64rt_ctrl_overlay(pk, map);
}
/* Save: what the pad mapping gets (from the menu's words and picks) as a game's record */
static inline void n64rt_ctrl_capture(const n64rt_ctrl_state_t *st, const n64rt_ctrl_pick_t *pk, uint32_t menu0, uint32_t menu1, n64rt_ctrl_t *rec)
{
    uint32_t s0 = menu0, s1 = menu1, map[5];
    n64rt_ctrl_effective(st, pk, &s0, &s1, map);
    memset(rec, 0, sizeof *rec);
    rec->set0 = s0 & N64RT_CTRL_MASK0;
    rec->set1 = s1 & N64RT_CTRL_MASK1;
    rec->m0 = N64RT_CTRL_MASK0;
    rec->m1 = N64RT_CTRL_MASK1;
    for (int i = 0; i < 5; i++) rec->map[i] = map[i];
}
#endif
