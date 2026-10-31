/* n64rt: load an N64 game into the RDRAM window and start it -- what the
 * N64's IPL3 does, plus the patches that hand libultra's hardware functions
 * to the runtime. */
#include <stdio.h>
#include <string.h>
#include <system.h>
#include <generated/csr.h>
#include <layout.h>
#include <notice.h>
#include "n64rt.h"

const n64rt_game_t *n64rt_game;
u32 n64rt_tv = 1;                       /* osTvType: 1 NTSC until the ROM's region says otherwise */

/* the replaced functions, by N64RT_* index (n64rt_exc.S n64rt_dispatch) */
#ifdef N64RT_OWN
/* (the game's own libultra: three functions of the table only, the TLB's -- see below) */
void n64rt_osMapTLB(void); void n64rt_osUnmapTLB(void); void n64rt_osUnmapTLBAll(void);
void (*const n64rt_table[N64RT_NFUNCS])(void) = {
    [N64RT_osMapTLB] = n64rt_osMapTLB, [N64RT_osUnmapTLB] = n64rt_osUnmapTLB, [N64RT_osUnmapTLBAll] = n64rt_osUnmapTLBAll,
};
#else
#define N64RT_DECL(n) void n64rt_##n(void);
N64RT_FUNCS(N64RT_DECL)
#undef N64RT_DECL
#ifdef N64RT_SI_GAME                    /* (not in this runtime: their places in the table are empty) */
#define N64RT_NONE(n) void n64rt_##n(void) __attribute__((weak));
N64RT_SI_FUNCS(N64RT_NONE)
#if N64RT_SI_GAME == 1
N64RT_SI_DMA(N64RT_NONE)
#endif
#undef N64RT_NONE
#endif
void (*const n64rt_table[N64RT_NFUNCS])(void) = {
#define N64RT_ENTRY(n) n64rt_##n,
    N64RT_FUNCS(N64RT_ENTRY)
#undef N64RT_ENTRY
};
#endif
static const char *const s_names[N64RT_NFUNCS] = {
#define N64RT_NAME(n) #n,
    N64RT_FUNCS(N64RT_NAME)
#undef N64RT_NAME
};
extern char n64rt_dispatch[];

void n64rt_os_init(void);
void n64rt_os_start(u32 entry, u32 sp) __attribute__((noreturn));

/* libultra's boot parameters, which IPL3 leaves at 0x8000_0300 */
#define BOOT_TV_TYPE    0x300u      /* osTvType: 0 PAL, 1 NTSC, 2 MPAL */
#define BOOT_ROM_TYPE   0x304u
#define BOOT_ROM_BASE   0x308u      /* osRomBase: the cartridge's PI address */
#define BOOT_RESET_TYPE 0x30Cu      /* osResetType: 0 cold */
#define BOOT_CIC_ID     0x310u
#define BOOT_VERSION    0x314u
#define BOOT_MEM_SIZE   0x318u

static inline u32 *rd32(u32 pa) { return (u32 *)(uintptr_t)N64RT_K0(pa); }

/* The TV system of the console the cartridge was made for, from the ROM header's destination code (ROM 0x3E, the fourth character
 * of the cartridge ID: the public list of the codes, n64brew's "ROM Header"). The PIF hands IPL3 this in osTvType (0 PAL, 1 NTSC,
 * 2 MPAL), and libultra derives everything from it: osViClock (49,656,530 / 48,681,812 / 48,628,316 Hz), the VI mode it starts
 * with, the audio DAC's divisor. PAL: D German, F French, H Dutch, I Italian, L Gateway 64 (PAL), P European, S Spanish,
 * U Australian, W Scandinavian, X and Y European. MPAL: B Brazilian. All the others -- A Asian, C Chinese, E North America,
 * G Gateway 64, J Japanese, K Korean, N Canadian, 7 beta, and a header with none -- are NTSC. */
#ifdef N64RT_OWN
static u32 tv_of_region(u32 c)
{
    switch (c) {
    case 'D': case 'F': case 'H': case 'I': case 'L': case 'P': case 'S': case 'U': case 'W': case 'X': case 'Y': return 0;
    case 'B': return 2;
    default: return 1;
    }
}
#endif
static void hooks_arm(void);


#ifdef N64RT_OWN
/* ---- libultra's envelope rate, done here ------------------------------------------
 * The audio library's envelope mixer turns each volume ramp into a rate, a 16.16 factor the microcode applies every
 * eight samples: (target / volume)^(8 / samples). Its function for it takes the power as (1 + l) to the n, squared three
 * times, with l = ln(target / volume) / 2^30 and n = 2^30 / samples, in double precision. This FPU computes a double as a
 * float (docs/mips.md, "The FPU"), and there 1 + l is 1.0 (|l| is at most 1e-8, a float's step at 1 is 1.2e-7): every
 * rate came out as 1.0 and no volume ever moved -- a note that starts from silence stayed silent, one that starts loud
 * never faded and stopped dead (music of drum hits with silence between).
 * The rate itself needs no double: (1 + l)^(8 n) is 2^y with y = (log2 of the ratio) * n * 2^-27, the library's own
 * logarithm (its table of the mantissa's top three bits, and the exponent) and its own n, and ul_getrate takes 2^y in
 * floats: 2^k by the exponent, the rest by a polynomial. Against the double steps, a rate under 2 (a ramp of more than
 * eight samples) is the same 16.16 value or one 2^-16 off; above that, a float's 24 bits leave a relative 1e-6.
 * The function is found by its words, those that a link relocates masked (a lui's half, the offsets and immediates
 * off a register a lui set, a jump's target: ul_mask), through a hash. Three builds of it are known (s_ul_fns), and
 * ul_patch puts a jump to a wrapper over each:
 *  - two take the O32 ABI's arguments, the two doubles in $f12 / $f14, the count and the rate's lower half's address in
 *    the caller's fifth and sixth words;
 *  - one, from a compiler that allocates registers across a module, takes the count in $a0, the volume in $f16, the
 *    target in $f14 and the address in $s1, and its callers keep values in what it leaves alone.
 * So the wrappers keep every register but $v0 (the rate's upper half) as it was -- the patch's own $t6 / $t7 / $t9
 * aside, which each build writes too -- and pass ul_getrate two floats in $f12 / $f13, as this single-float build
 * takes them. */
static const float s_ul_log2[8] = { -0.912537f, -0.752072f, -0.607683f, -0.476438f, -0.356144f, -0.245112f, -0.142019f, -0.045804f };
static s32 ul_trunc(float f) { return (s32)f; }                           /* (trunc.w.s) */
s32 ul_getrate(float vol, float tgt, s32 count, u16 *ratel)
{
    if (count == 0) {
        if (vol <= tgt) { *ratel = 0xFFFFu; return 0x7FFF; }
        *ratel = 0; return 0;
    }
    if (tgt < 1.0f) tgt = 1.0f;
    if (vol <= 0.0f) vol = 1.0f;
    union { float f; u32 u; } a = { tgt / vol };                          /* the mantissa's top bits and the exponent */
    const s32 ex = (s32)((a.u >> 23) & 0xFFu) - 126, n = 0x40000000 / count;
    const float y = (s_ul_log2[(a.u >> 20) & 7u] + (float)ex) * ((float)n * (1.0f / 134217728.0f));
    if (y >= 15.0f) { *ratel = 0xFFFFu; return 0x7FFF; }                 /* (past the 16.16 range) */
    if (y < -24.0f) { *ratel = 0; return 0; }
    const s32 k = ul_trunc(y < 0.0f ? y - 0.5f : y + 0.5f);
    const float f = (y - (float)k) * 0.6931472f;                          /* 2^(y - k) = e^f, |f| <= 0.35 */
    const float p = 1.0f + f * (1.0f + f * (0.5f + f * (1.0f / 6 + f * (1.0f / 24 + f * (1.0f / 120 + f * (1.0f / 720 + f * (1.0f / 5040)))))));
    const union { u32 u; float f; } sc = { (u32)(127 + k) << 23 };
    const float r = p * sc.f;
    const s32 s = (s16)ul_trunc(r);
    *ratel = (u16)ul_trunc((r - (float)s) * 65535.0f);
    return s;
}
/* the wrappers' frame: ul_getrate's argument words, then $ra, the game's Status ($t7), the GPRs it may change, HI, LO,
 * FCR31 and $f0 - $f19 */
#define UL_SAVE(r, o) "    sw      $" #r ", " #o "($sp)\n"
#define UL_LOAD(r, o) "    lw      $" #r ", " #o "($sp)\n"
#define UL_REGS(op) op(ra, 16) op(t7, 20) op(at, 24) op(v1, 28) op(a0, 32) op(a1, 36) op(a2, 40) op(a3, 44) op(t0, 48) \
                    op(t1, 52) op(t2, 56) op(t3, 60) op(t4, 64) op(t5, 68) op(t8, 72)
#define UL_FREGS(op) op(0, 96) op(1, 100) op(2, 104) op(3, 108) op(4, 112) op(5, 116) op(6, 120) op(7, 124) op(8, 128) \
                     op(9, 132) op(10, 136) op(11, 140) op(12, 144) op(13, 148) op(14, 152) op(15, 156) op(16, 160) \
                     op(17, 164) op(18, 168) op(19, 172)
#define UL_FSAVE(r, o) "    swc1    $f" #r ", " #o "($sp)\n"
#define UL_FLOAD(r, o) "    lwc1    $f" #r ", " #o "($sp)\n"
#define UL_ENTER "    addiu   $sp, $sp, -176\n" UL_REGS(UL_SAVE) UL_FREGS(UL_FSAVE) \
                 "    mfhi    $t0\n    mflo    $t1\n    cfc1    $t2, $31\n    sw      $t0, 76($sp)\n    sw      $t1, 80($sp)\n" \
                 "    sw      $t2, 84($sp)\n"
__asm__(".text\n.set push\n.set noreorder\n.set noat\n.set mips3\n.set doublefloat\n.set oddspreg\n"
        ".globl ulw_getrate\n"
        "ulw_getrate:\n"                         /* (O32: the doubles in $f12 / $f14, the caller's fifth and sixth words) */
        UL_ENTER
        "    lw      $a2, 192($sp)\n"
        "    lw      $a3, 196($sp)\n"
        "    cvt.s.d $f12, $f12\n"
        "    b       1f\n"
        "    cvt.s.d $f13, $f14\n"
        ".globl ulw_getrate_r\n"
        "ulw_getrate_r:\n"                       /* (count $a0, volume $f16, target $f14, the address $s1) */
        UL_ENTER
        "    move    $a2, $a0\n"
        "    move    $a3, $s1\n"
        "    cvt.s.d $f12, $f16\n"
        "    cvt.s.d $f13, $f14\n"
        "1:  jal     ul_getrate\n"
        "    nop\n"
        "    lw      $t0, 76($sp)\n    lw      $t1, 80($sp)\n    lw      $t2, 84($sp)\n"
        "    mthi    $t0\n    mtlo    $t1\n    ctc1    $t2, $31\n"
        UL_FREGS(UL_FLOAD) UL_REGS(UL_LOAD)
        "    addiu   $sp, $sp, 176\n"
        "    jr      $ra\n"
        "    mtc0    $t7, $12\n"
        ".set pop\n");
void ulw_getrate(void);
void ulw_getrate_r(void);
/* a word as the hash takes it: what a link relocates masked -- `hi` the registers a lui set so far */
static u32 ul_mask(u32 w, u32 *hi)
{
    const u32 op = w >> 26, rs = (w >> 21) & 31u, rt = (w >> 16) & 31u;
    /* addiu, lb, lh, lw, lbu, lhu, sb, sh, sw, lwc1, ldc1, ld, swc1, sdc1, sd */
    static const u32 s_imm_lo = 1u << 0x09, s_imm_hi = 1u << (0x20 - 32) | 1u << (0x21 - 32) | 1u << (0x23 - 32) |
        1u << (0x24 - 32) | 1u << (0x25 - 32) | 1u << (0x28 - 32) | 1u << (0x29 - 32) | 1u << (0x2B - 32) |
        1u << (0x31 - 32) | 1u << (0x35 - 32) | 1u << (0x37 - 32) | 1u << (0x39 - 32) | 1u << (0x3D - 32) | 1u << (0x3F - 32);
    if (op == 0x0Fu) { *hi |= 1u << rt; return w & 0xFFFF0000u; }         /* lui */
    if (op == 0x02u || op == 0x03u) return w & 0xFC000000u;               /* j, jal */
    if ((*hi >> rs & 1u) && ((op < 32u ? s_imm_lo >> op : s_imm_hi >> (op - 32u)) & 1u)) return w & 0xFFFF0000u;
    return w;
}
static const struct { u32 first, last2, last1, words, hash; void (*fn)(void); } s_ul_fns[] = {
    { 0x27BDFFB8u, 0x03E00008u, 0x27BD0048u, 160u, 0x97830ABCu, ulw_getrate },     /* (FNV-1a of the masked words) */
    { 0x27BDFF48u, 0x03E00008u, 0x27BD00B8u, 121u, 0xB31A06F9u, ulw_getrate },
    { 0x27BDFF18u, 0x03E00008u, 0x00000000u, 116u, 0x23CA9D96u, ulw_getrate_r },
};
/* The patch goes into the runtime's code with interrupts off (an interrupt of the game's would arrive at the runtime's
 * vector) and Status bit 4 (KSU's, which the game's code never sets) on: while the game runs, 0x2000_0000 - 0x4FFF_FFFF are
 * the game's mapped addresses (mips_core, game_view: a game can map an overlay at 0x4000_0000 and run it there), and bit 4
 * gives the runtime's code back its own SDRAM there. The wrapper's mtc0 puts the game's Status back in the delay slot of the
 * return, where the next instruction is the game's again. */
static void ul_patch(u32 vram, u32 len)
{
    for (u32 a = vram; a + 4u * 116u <= vram + len; a += 4u) {
        const u32 *w = rd32(a);
        for (u32 k = 0; k < sizeof s_ul_fns / sizeof s_ul_fns[0]; k++) {
            const u32 n = s_ul_fns[k].words;
            if (a + 4u * n > vram + len || w[0] != s_ul_fns[k].first || w[n - 2] != s_ul_fns[k].last2 || w[n - 1] != s_ul_fns[k].last1) continue;
            u32 h = 0x811C9DC5u, hi = 0;
            for (u32 i = 0; i < n; i++) {
                const u32 m = ul_mask(w[i], &hi);
                for (int b = 24; b >= 0; b -= 8) h = (h ^ ((m >> b) & 0xFFu)) * 0x01000193u;
            }
            if (h != s_ul_fns[k].hash) continue;
            const u32 f = (u32)(uintptr_t)s_ul_fns[k].fn;
            u32 *d = rd32(a);
            d[0] = 0x400F6000u;                  /* mfc0 t7, Status                  (the wrapper gives it back; bit 4 too) */
            d[1] = 0x35EE0011u;                  /* ori  t6, t7, 0x11 */
            d[2] = 0x39CE0001u;                  /* xori t6, t6, 1 */
            d[3] = 0x408E6000u;                  /* mtc0 t6, Status */
            d[4] = 0;
            d[5] = 0x3C190000u | (f >> 16);      /* lui  t9, hi */
            d[6] = 0x37390000u | (f & 0xFFFFu);  /* ori  t9, t9, lo */
            d[7] = 0x03200008u;                  /* jr   t9 */
            d[8] = 0;
            printf("n64rt: the audio library's envelope rate at %08lx done in the runtime (build %lu)\n", (unsigned long)a,
                   (unsigned long)k);
            break;
        }
    }
}

/* ---- a float matrix to the RSP's fixed point by a double's mantissa -------------------------------------------------
 * A hand-written guMtxF2L (a leaf: $a0 the 16 floats, $a1 the Mtx) converts each float with no conversion instruction:
 * cvt.d.s, then add.d of 1.5 * 2^36, whose step is 2^-16, so that the sum's lower word -- mfc1 of the even register --
 * is the 16.16 value, rounded to nearest, in two's complement. This FPU computes a double as a float widened (docs/mips.md,
 * "The FPU"): that lower word is always 0 and every matrix it makes is all zeros. Models drawn through one of them are
 * transformed onto a single point and cover no pixel: the scenes' 3D is missing, the 2D over it and the clear colour
 * stay. Its 47 words are written over with mf_f2l's (below), the same conversion in integers: a float M * 2^(E - 150) is
 * M * 2^(E - 134) in 16.16, shifted left (mod 2^32, as the lower word wraps) or right, rounded to nearest with ties to
 * even (FCR31's mode, which games keep), 0 when below a half; the halves stored as the Mtx has them, the integer parts'
 * 16 halfwords then the fractions'. It writes only registers the original writes ($t0 - $t7, $a0, $a1), so callers that
 * keep values in the others see no difference, and it needs no 64-bit instruction
 * (the original's ld, sd and dsll are the CPU's, but a conversion by integers is shorter). Found as ul_patch finds its functions. */
__asm__(".text\n.set push\n.set noreorder\n.set noat\n"
        ".globl mf_f2l\n.globl mf_f2l_end\n"
        "mf_f2l:\n"
        "    addiu   $t6, $a0, 64\n"
        "1:  lw      $t0, 0($a0)\n"
        "    srl     $t1, $t0, 23\n"
        "    andi    $t1, $t1, 0xFF\n"            /* E */
        "    sll     $t2, $t0, 9\n"
        "    srl     $t2, $t2, 9\n"
        "    lui     $t3, 0x80\n"
        "    or      $t2, $t2, $t3\n"             /* M, the hidden bit set (E = 0 lands below a half: 0) */
        "    addiu   $t3, $t1, -134\n"            /* sh: the 16.16 value is M * 2^sh */
        "    bltz    $t3, 2f\n"
        "    subu    $t4, $zero, $t3\n"           /* n = -sh */
        "    sltiu   $t5, $t3, 32\n"              /* (a shift of 32 or more leaves 0 in the word) */
        "    b       3f\n"
        "    sllv    $t2, $t2, $t3\n"
        "2:  sltiu   $t5, $t4, 25\n"              /* (n of 25 or more: below a half) */
        "    srlv    $t1, $t2, $t4\n"             /* r = M >> n */
        "    sllv    $t3, $t1, $t4\n"
        "    subu    $t3, $t2, $t3\n"             /* the bits shifted out */
        "    andi    $t2, $t1, 1\n"
        "    addu    $t3, $t3, $t2\n"
        "    addiu   $t2, $t4, -1\n"
        "    addiu   $t7, $zero, 1\n"
        "    sllv    $t7, $t7, $t2\n"             /* a half */
        "    sltu    $t7, $t7, $t3\n"             /* above it, or at it with r odd: up */
        "    addu    $t2, $t1, $t7\n"
        "3:  subu    $t5, $zero, $t5\n"
        "    and     $t2, $t2, $t5\n"
        "    sra     $t1, $t0, 31\n"
        "    xor     $t2, $t2, $t1\n"
        "    subu    $t2, $t2, $t1\n"             /* the sign */
        "    sh      $t2, 32($a1)\n"              /* the fraction */
        "    srl     $t2, $t2, 16\n"
        "    sh      $t2, 0($a1)\n"               /* the integer part */
        "    addiu   $a0, $a0, 4\n"
        "    bne     $a0, $t6, 1b\n"
        "    addiu   $a1, $a1, 2\n"
        "    jr      $ra\n"
        "    nop\n"
        "mf_f2l_end:\n"
        ".set pop\n");
extern const u32 mf_f2l[], mf_f2l_end[];
static void mf_patch(u32 vram, u32 len)
{
    enum { N = 47 };                             /* (lui at / lwc1 the constant ... jr ra / nop) */
    for (u32 a = vram; a + 4u * N <= vram + len; a += 4u) {
        const u32 *w = rd32(a);
        if (w[0] != 0x3C010000u + (w[0] & 0xFFFFu) || w[N - 2] != 0x03E00008u || w[N - 1] != 0) continue;
        u32 h = 0x811C9DC5u, hi = 0;
        for (u32 i = 0; i < N; i++) {
            const u32 m = ul_mask(w[i], &hi);
            for (int b = 24; b >= 0; b -= 8) h = (h ^ ((m >> b) & 0xFFu)) * 0x01000193u;
        }
        if (h != 0x21588392u) continue;
        u32 *d = rd32(a);
        const u32 n = (u32)(mf_f2l_end - mf_f2l);
        for (u32 i = 0; i < N; i++) d[i] = i < n ? mf_f2l[i] : 0;
        printf("n64rt: a float matrix's conversion by a double's mantissa at %08lx done in integers\n", (unsigned long)a);
    }
}
#endif
void n64rt_run(const n64rt_game_t *g)
{
    n64rt_game = g;
    printf("n64rt: %s -- ROM %lu bytes at %08lx, boot %lu bytes %08lx -> %08lx, entry %08lx\n",
           g->name, (unsigned long)g->rom_size, (unsigned long)g->rom_sdram, (unsigned long)g->boot_len,
           (unsigned long)g->boot_rom, (unsigned long)g->boot_vram, (unsigned long)g->entry);

#ifdef N64RT_OWN
    {   /* the console's TV system: from the ROM header's destination code (tv_of_region) */
        const u32 region = (n64rt_rom_word(0x3Cu) >> 8) & 0xFFu;
        n64rt_tv = tv_of_region(region);
        printf("n64rt: region '%c' (%02lx): osTvType %lu (%s)\n", region >= 0x20u && region < 0x7Fu ? (char)region : '?', (unsigned long)region,
               (unsigned long)n64rt_tv, n64rt_tv == 0u ? "PAL" : n64rt_tv == 2u ? "MPAL" : "NTSC");
    }
#endif
    /* RDRAM: cleared (the game's mem_size of it), then IPL3's copy of the boot segment */
    for (u32 a = 0; a < g->mem_size; a += 4) *rd32(a) = 0;
    /* The RDP's hidden-bit plane (layout.h LAYOUT_HID: the coverage's other two bits of each 16-bit word of RDRAM):
     * all ones, as the hardware starts it -- a pixel the RDP has not drawn has the coverage its bit 0 says (7 or 3) */
    for (u32 a = 0; a < 0x00100000u; a += 4) *(volatile u32 *)(uintptr_t)(LAYOUT_HID + a) = 0xFFFFFFFFu;
    for (u32 i = 0; i < g->boot_len / 4u; i++) *rd32(g->boot_vram + 4u * i) = n64rt_rom_word(g->boot_rom + 4u * i);
    /* What IPL3 leaves in the RDRAM's first 0x400 bytes: for a CIC-6103 boot, a copy of its own code, ROM 0x4C0..0x8C0 at 0
     * (the game's boot segment, from 0x400 on, covers the rest). Banjo-Kazooie checks three of those words -- 0x1D8, 0x22C,
     * 0x238 (a lui/lw from 0xA0000000, compared with the ROM's own 0x698, 0x6EC, 0x6F8) -- and when one differs it keeps its
     * viewport's near and far at 750 and 1250: only what is that close is drawn, a few trees on black in the intro's flight
     * over Spiral Mountain. */
#ifdef N64RT_OWN
    if (g->cic == 6103u && g->rom_size >= 0x8C0u)         /* (the runtime keeps nothing of its own in the game's RDRAM: any size of it) */
#else
    if (g->cic == 6103u && g->rom_size >= 0x8C0u && g->mem_size <= 0x7F0000u - 0x1000u)   /* (a game with the whole 8 MiB has the runtime's structures from 0x200) */
#endif
        for (u32 i = 0; i < 0x400u / 4u; i++) *rd32(4u * i) = n64rt_rom_word(0x4C0u + 4u * i);
    /* A CIC-6106 boot leaves its own second stage at 0 the same way, the 0x2BC bytes of ROM 0x4F0..0x7AC, which are stored
     * enciphered: its first stage copies them word by word, each XORed with the next value of x <- x * 0x0260BCD5 (32 bits),
     * x starting from the CIC's seed, 0x85, as 0x85 * 0x0260BCD5 + 1, and jumps there (the code that writes the boot words
     * below and fills the RSP's memories). The boot code is the CIC's (rom_cic knows it by its CRC), so these words are the
     * same for every such ROM. */
#ifdef N64RT_OWN
    if (g->cic == 6106u && g->rom_size >= 0x7ACu)
#else
    if (g->cic == 6106u && g->rom_size >= 0x7ACu && g->mem_size <= 0x7F0000u - 0x1000u)
#endif
        for (u32 i = 0, x = 0x85u * 0x0260BCD5u + 1u; i < (0x7ACu - 0x4F0u) / 4u; i++, x *= 0x0260BCD5u)
            *rd32(4u * i) = n64rt_rom_word(0x4F0u + 4u * i) ^ x;
    /* A CIC-6105 IPL3 copies the 0x334 bytes of its own second stage, DMEM 0x554..0x888 (ROM 0x554..0x888, which its first stage
     * does not touch), to RDRAM 4 and jumps there. Those words stay in the RDRAM's first 0x338 bytes: a game with this CIC
     * compares the word at 0x2E8 with the ROM's own at 0x838 each time it runs a function that
     * frees its delayed objects, and when it differs frees none of them: its list of them (2560 entries) fills in some 30
     * seconds of its intro and the game then waits for ever for a free entry, the picture frozen. The words of the boot
     * information (0x300..) are written over their place below. */
#ifdef N64RT_OWN
    if (g->cic == 6105u && g->rom_size >= 0x888u)
#else
    if (g->cic == 6105u && g->rom_size >= 0x888u && g->mem_size <= 0x7F0000u - 0x1000u)
#endif
        for (u32 i = 0; i < (0x888u - 0x554u) / 4u; i++) *rd32(4u + 4u * i) = n64rt_rom_word(0x554u + 4u * i);
    /* What a CIC-6105 boot leaves at 0x2FE1C0 and 0x2FB1F4: words of IPL3's own code, ROM 0x750 (0xAD170014, the `sw s7, 20(t0)`
     * that stores the boot parameters) and ROM 0x73C (0xAD090010, the `sw t1, 16(t0)` that stores osCicId). One 6105 game
     * checks the first right after osInitialize and, finding anything else, spins on it for ever: a black screen.
     * Another 6105 game checks both in a thread of its own at its start and, when either differs, marks itself as not on a
     * console: its first scene is drawn and never goes on (no logo, no title, the buttons do nothing). Every 6105 ROM has the
     * same IPL3, so these are the CIC's, whatever the game. */
    if (g->cic == 6105u && g->rom_size >= 0x1000u) {
        *rd32(0x2FE1C0u) = n64rt_rom_word(0x750u);
        *rd32(0x2FB1F4u) = n64rt_rom_word(0x73Cu);
    }
    /* The boot information words, as the real IPL3 leaves them (osTvType from the console's PIF -- see below --, osRomType 0 cartridge, osRomBase, osResetType 0 cold, osVersion 0). osCicId is written by
     * the 6103's IPL3 (`addiu t1, 0x17d7; sw t1, 16(t0)`), the 6105's (0x17d9) and the 6106's (0x17d8, in its enciphered second
     * stage) and by no other we have read (6101, 6102, the PAL 7102: nothing, the word stays 0): n64rt_cic_id. It was the
     * decimal CIC number (6101..6106), which no boot code leaves there; libultra itself does not read it.
     * A game's own libultra (N64RT_OWN) has osTvType from the ROM's region (tv_of_region): the console is the one the cartridge
     * was made for; the VI's timing follows what the game then programs (V_SYNC: rtl/soc/rcp_vi.sv), starting from this. */
    *rd32(BOOT_TV_TYPE) = n64rt_tv;
    *rd32(BOOT_ROM_TYPE) = 0;
    *rd32(BOOT_ROM_BASE) = 0xB0000000u;
    *rd32(BOOT_RESET_TYPE) = 0;
    *rd32(BOOT_CIC_ID) = n64rt_cic_id(g->cic);
    *rd32(BOOT_VERSION) = 0;
    *rd32(BOOT_MEM_SIZE) = g->mem_size;

#ifdef N64RT_OWN
    /* The CPU's translation tables are the runtime's to fill (mips_core TLB_RAM): a game's Super Mario 64 maps its Goddard
     * dynlists at 0x04000000 for the Mario head's screen, and a page it maps must be entered there. A core with the monitor
     * (COP0 register 23's bit 0) takes the game's own tlbwi / tlbwr / tlbr / tlbp as a Reserved Instruction for the monitor
     * level (n64rt_os.c, tlb_insn): nothing of the game is patched, no address of its libultra is needed, and code that
     * the game loads or unpacks later is covered too. A core without that (an older bitstream) has the VR4300's TLB
     * instructions do nothing; then osMapTLB, osUnmapTLB and osUnmapTLBAll, if the game's list has their addresses, are the
     * runtime's: their first instruction becomes a reserved one (SPECIAL, function 0x35, the function's number in the code
     * field), which the monitor level takes (a call to the runtime's code could not: an interrupt of the game's own arrives
     * in it, at the runtime's vector) and does for the game (n64rt_os.c, own_tlb_call). */
    {
        u32 c23, done = 0;
        __asm__ volatile("mfc0 %0, $23" : "=r"(c23));
        if ((c23 & ~8u) == 0x54420001u) {
            printf("n64rt: the game's own libultra, nothing of it replaced: its TLB instructions are the monitor's\n");
        } else {
            for (u32 i = 0; i < g->npatches; i++) {
                const n64rt_patch_t *p = &g->patches[i];
                if (p->func != N64RT_osMapTLB && p->func != N64RT_osUnmapTLB && p->func != N64RT_osUnmapTLBAll) continue;
                *rd32(p->vram) = 0x00000035u | (p->func << 6);
                done++;
            }
            printf("n64rt: the game's own libultra: nothing of it replaced but its TLB functions (%lu; this core does not take the TLB instructions)\n",
                   (unsigned long)done);
        }
    }
    (void)s_names;
#else
    /* each replaced function: lui t9, %hi(n64rt_dispatch) / jr t9 / ori t9, t9, index */
    u32 disp = (u32)(uintptr_t)n64rt_dispatch;
    if (disp & 0xFFFFu) n64rt_fatal("n64rt: n64rt_dispatch %08lx is not 64 KiB aligned\n", (unsigned long)disp);
    u32 done = 0, have = 0;
    for (u32 i = 0; i < N64RT_NFUNCS; i++) if (n64rt_table[i]) have++;
    for (u32 i = 0; i < g->npatches; i++) {
        const n64rt_patch_t *p = &g->patches[i];
        if (p->func >= N64RT_NFUNCS) n64rt_fatal("n64rt: patch %lu: no such function\n", (unsigned long)i);
        if (!n64rt_table[p->func]) continue;         /* (the game's own stays) */
        u32 *w = rd32(p->vram);
        w[0] = 0x3C190000u | (disp >> 16);
        w[1] = 0x03200008u;
        w[2] = 0x37390000u | p->func;
        done++;
    }
    printf("n64rt: %lu libultra functions replaced (of %lu the runtime has)\n", (unsigned long)done, (unsigned long)have);
    if (done != g->npatches) printf("n64rt: %lu more left as the game has them\n", (unsigned long)(g->npatches - done));
    (void)s_names;
#ifdef N64RT_STATS
    {   /* (n64rt_dispatch goes through it: n64rt_exc.S) */
        extern u32 n64rt_stats[512];
        _Static_assert(N64RT_NFUNCS <= 256, "n64rt_stats");
        for (u32 i = 0; i < N64RT_NFUNCS; i++) n64rt_stats[256u + i] = (u32)(uintptr_t)n64rt_table[i];
    }
#endif
    hooks_arm();
#endif
    flush_cpu_dcache();
    flush_cpu_icache();
#ifdef N64RT_DEBUG
    { extern void n64rt_watch_arm(void); n64rt_watch_arm(); }
#endif

    n64rt_os_init();
#ifdef N64RT_OWN
    /* (the game's code is its own but for two functions that compute in doubles, which the FPU does as floats) */
    ul_patch(g->boot_vram, g->boot_len);               /* (the audio library's envelope rate) */
    mf_patch(g->boot_vram, g->boot_len);               /* (and a matrix conversion through a double's mantissa) */
    flush_cpu_dcache();
    flush_cpu_icache();
#endif
    n64rt_io_init();
    n64rt_os_start(g->entry, N64RT_K0(g->mem_size - 0x10u));     /* (the RDRAM's top: 8 MiB is not an address of the window) */
}

/* ---- the functions in code loaded later (n64rt.h) ---------------------------------
 * The last one waiting stands for the rest: code is written from its start
 * on, so when the highest address has its words the lower ones have. */
/* (the game's own lists, or a port's globals) */
#define G_LATE   (n64rt_game->late ? n64rt_game->late : n64rt_late)
#define G_NLATE  (n64rt_game->late ? n64rt_game->nlate : (n64rt_late && &n64rt_nlate) ? n64rt_nlate : 0u)
#define G_HOOKS  (n64rt_game->hooks ? n64rt_game->hooks : n64rt_hooks)
#define G_NHOOKS (n64rt_game->hooks ? n64rt_game->nhooks : (n64rt_hooks && &n64rt_nhooks) ? n64rt_nhooks : 0u)
static u32 s_late_done[4];              /* a bit a function patched (up to 128) */
static u32 s_late_left = ~0u;
static void late_apply(int all);
#ifdef N64RT_OWN
void n64rt_late_poll(void) { }          /* (the game's own libultra, loaded later or not: nothing is put into it) */
#else
void n64rt_late_poll(void) { late_apply(0); }
#endif
#define HOOK_BREAK 0x000A954Du          /* break, a code of the runtime's own */
#define HOOK_MAX 32u
static u32 s_hook_insn[HOOK_MAX];
static int s_hooks_on;
static void hooks_arm(void)
{
    if (!G_NHOOKS) return;
    u32 n = G_NHOOKS > HOOK_MAX ? HOOK_MAX : G_NHOOKS;
    for (u32 i = 0; i < n; i++) { u32 *w = rd32(G_HOOKS[i]); s_hook_insn[i] = *w; *w = HOOK_BREAK; }
    s_hooks_on = 1;
}
int n64rt_hook_break(u32 epc)
{
    if (!s_hooks_on) return 0;
    u32 n = G_NHOOKS > HOOK_MAX ? HOOK_MAX : G_NHOOKS, mine = 0;
    for (u32 i = 0; i < n; i++) if (N64RT_K0(G_HOOKS[i]) == N64RT_K0(epc)) mine = 1;
    if (!mine) return 0;
    for (u32 i = 0; i < n; i++) *rd32(G_HOOKS[i]) = s_hook_insn[i];
    s_hooks_on = 0;
    n64rt_log("n64rt: the boot segment goes into its later code at %08lx\n", (unsigned long)epc);
    late_apply(1);
    flush_cpu_dcache();
    flush_cpu_icache();
    return 1;
}
static void late_apply(int all)
{
    if (s_late_left == 0u) return;
    if (!G_NLATE) { s_late_left = 0; return; }
    u32 n = G_NLATE > 128u ? 128u : G_NLATE;
    if (s_late_left == ~0u) s_late_left = n;
    if (s_late_left == 0u) return;
    int last = -1;
    for (int i = 0; i < (int)n; i++)                 /* (the one at the highest address) */
        if (!(s_late_done[i >> 5] & (1u << (i & 31))) && (last < 0 || G_LATE[i].vram > G_LATE[last].vram)) last = i;
    if (last < 0) { s_late_left = 0; return; }
    const u32 *lw = rd32(G_LATE[last].vram);
    if (!all && (lw[0] != G_LATE[last].w[0] || lw[1] != G_LATE[last].w[1])) return;
    u32 disp = (u32)(uintptr_t)n64rt_dispatch, done = 0;
    for (u32 i = 0; i < n; i++) {
        const n64rt_late_t *p = &G_LATE[i];
        if (s_late_done[i >> 5] & (1u << (i & 31))) continue;
        u32 *w = rd32(p->vram);
        if (w[0] != p->w[0] || w[1] != p->w[1] || p->func >= N64RT_NFUNCS) continue;
        s_late_done[i >> 5] |= 1u << (i & 31);
        s_late_left--;
        if (!n64rt_table[p->func]) continue;         /* (the game's own stays) */
        w[0] = 0x3C190000u | (disp >> 16);
        w[1] = 0x03200008u;
        w[2] = 0x37390000u | p->func;
        done++;
    }
#ifdef N64RT_DEBUG
    for (u32 i = 0, shown = 0; i < n && shown < 6u; i++)
        if (!(s_late_done[i >> 5] & (1u << (i & 31)))) {
            const u32 *w = rd32(G_LATE[i].vram);
            n64rt_log("n64rt:   not yet: %s at %08lx has %08lx %08lx, not %08lx %08lx\n", s_names[G_LATE[i].func], (unsigned long)G_LATE[i].vram,
                      (unsigned long)w[0], (unsigned long)w[1], (unsigned long)G_LATE[i].w[0], (unsigned long)G_LATE[i].w[1]);
            shown++;
        }
#endif
    if (done) {
        flush_cpu_icache();
        n64rt_log("n64rt: %lu libultra functions replaced in code the game has loaded (%lu to come)\n",
                  (unsigned long)done, (unsigned long)s_late_left);
    }
}

/* ---- the ROM ------------------------------------------------------------------------
 * The Pocket loads the program at 0x4000_0000 and the ROM at
 * N64RT_ROM_FILE_OFF from there: as two files (the ROM a .z64 as it is,
 * n64rt_boot_rom) or as one game file (tools/make_mirlo_game.py: the port's
 * program and the ROM as block ROM0, the port's Makefile's
 * ROM0@0x100000=<rom.z64>; n64rt_boot_game_file). The ROM is read where
 * that leaves it (layout.h): the first 30 MiB are in PSRAM, and the rest
 * is in SDRAM, where the bridge wrote it -- all
 * through the core's window, N64RT_ROM_WIN. Nothing is moved. The port links its boot stack below
 * N64RT_ROM_FILE_OFF (--defsym=_fstack=0x400FFFF0). */
#define FILE_SDRAM 0x40000000u
static void no_rom(const char *msg) __attribute__((noreturn));
static void no_rom(const char *msg)
{
    apf_bridge_savestate_write(0);          /* (no game: no save state, n64rt_state.c) */
    printf("n64rt: %s\n", msg);
    notice(N64RT_RDRAM_SDRAM, msg);         /* (on the screen too: nothing of the game's is in the RDRAM yet) */
    for (;;) ;
}
/* the word the Pocket loaded at an offset from the program's start: below 1 MiB the SDRAM's, from there the window's */
static u32 file_word(u32 off)
{
    if (off < N64RT_ROM_FILE_OFF) return *(const volatile u32 *)(uintptr_t)(FILE_SDRAM + off);
    return *(const volatile u32 *)(uintptr_t)(N64RT_ROM_WIN + (off - N64RT_ROM_FILE_OFF));
}
u32 n64rt_rom_word(u32 off)
{
    return file_word(n64rt_game->rom_sdram - FILE_SDRAM + off);
}
void n64rt_rom_copy(void *dst, u32 off, u32 size)
{
    u8 *d = dst;
    u32 w = 0, have = ~0u;
    for (u32 i = 0; i < size; i++) {
        u32 a = off + i;
        if ((a & ~3u) != have) { have = a & ~3u; w = n64rt_rom_word(have); }
        d[i] = (u8)(w >> (24u - 8u * (a & 3u)));
    }
}
#define CPU_LEVELS 0xF8000000u          /* COP0 7: ALU, SH, MD, DIV and UA (the 64-bit adds, shifts, multiplies, divides and unaligned accesses) */
/* what both ways in begin with */
static void boot_checks(void)
{
    /* A core of another memory layout (its boot ROM has no mark: layout.h)
     * has the saves' slots elsewhere, in what is the RDRAM here: nothing is
     * run on it, so the Pocket reads back from them what it loaded. */
    if (*(const volatile u32 *)(uintptr_t)(0xBFC00000u + LAYOUT_MARK_OFS) != LAYOUT_MARK)
        no_rom("This program needs a newer Mirlo core, with its data.json: nothing was started. The saves are as they were.");
    /* The bitstream and this program are a pair: the CPU does every 64-bit integer instruction itself (the runtime does none for
     * it), and says which levels it has in COP0 7's bits 31..27 -- ALU, SH, MD, DIV, UA (mips_core's W64_*); a core before them
     * reads 0 there. */
    {
        u32 mctl; __asm__ volatile("mfc0 %0, $7" : "=r"(mctl));
        if ((mctl & CPU_LEVELS) != CPU_LEVELS)
            no_rom("This program needs a newer Mirlo core, whose CPU does the 64-bit instructions: nothing was started. The saves are as they were.");
    }
    /* (the ROM in the PSRAM and its window: MONITOR.ID bit 7, and the chips set up, bit 8) */
    while ((monitor_id_read() & 0xFFFF0180u) != 0x4D4F0180u) {
        if ((monitor_id_read() & 0xFFFF0080u) != 0x4D4F0080u) no_rom("This program needs a newer Mirlo core: nothing was started.");
    }
}
/* the ROM is `size` bytes at `off` of what the Pocket loaded */
static void rom_at(n64rt_game_t *g, u32 off, u32 size)
{
    if (off < N64RT_ROM_FILE_OFF || off >= LAYOUT_FILE_MAX || (off & 3u) || size < 0x1000u)
        no_rom("no ROM at 1 MiB of what the Pocket loaded");
    if (file_word(off) != 0x80371240u) no_rom("the ROM is not a .z64 ROM");
    g->rom_sdram = FILE_SDRAM + off;
    g->rom_size = size;
#ifdef N64RT_DEBUG
    if (size > N64RT_ROM_SPLIT) {
        /* (what a simulation compares with the file: words either side of where the SDRAM's part starts and the last one,
         * and a copy across it) */
        const n64rt_game_t *was = n64rt_game;
        n64rt_game = g;
        u8 b[16];
        const u32 t = N64RT_ROM_SPLIT, at[4] = { t - 4u, t, t + 0x00800000u, size - 4u };
        n64rt_rom_copy(b, t - 6u, 16);
        printf("n64rt: a ROM of %lu bytes, from %08lx on in SDRAM: words", (unsigned long)size, (unsigned long)t);
        for (int i = 0; i < 4; i++) if (at[i] < size) printf(" %08lx=%08lx", (unsigned long)at[i], (unsigned long)n64rt_rom_word(at[i]));
        printf("; bytes at %08lx:", (unsigned long)(t - 6u));
        for (int i = 0; i < 16; i++) printf(" %02x", b[i]);
        printf("\n");
        n64rt_game = was;
    }
#endif
}

void n64rt_boot_game_file(n64rt_game_t *g)
{
    boot_checks();
    u32 fsz = apf_bridge_file_size_read();
    if (fsz < 64u) no_rom("no game file");
    u32 ft[8];
    for (u32 i = 0; i < 8u; i++) ft[i] = file_word(fsz - 32u + 4u * i);
    if (memcmp(&ft[4], "MIRLODATA", 10) != 0 || ft[3] != 1u) no_rom("no game file footer");
    u32 n = ft[1], table = ft[2], off = 0, size = 0;
    if (n == 0 || n > 16u || table + n * 16u > fsz) no_rom("bad block table");
    for (u32 i = 0; i < n; i++) {
        u32 tag = file_word(table + 16u * i);
        if (memcmp(&tag, "ROM0", 4) == 0) { off = file_word(table + 16u * i + 4u); size = file_word(table + 16u * i + 8u); }
    }
    if (off >= fsz || size > fsz - off) no_rom("no ROM0 block in the game file");
    rom_at(g, off, size);
    n64rt_run(g);
}

/* RDRAM for what the runtime hands the game (a PI handle...): game code can
 * only reach the RDRAM window, not the runtime's own memory. Taken just above
 * the game's mem_size, which nothing of the game's uses. A game that wants
 * all 8 MiB leaves no room there: its own goes in 0x200 - 0x2FF, between the
 * N64's exception vectors and the boot parameters, which no game uses. */
void *n64rt_rdram_alloc(u32 size)
{
    static u32 s_top;
    const u32 low = n64rt_game->mem_size > 0x7F0000u - 0x1000u;
    if (!s_top) s_top = low ? 0x200u : n64rt_game->mem_size;
    u32 pa = s_top;
    s_top = (s_top + size + 15u) & ~15u;
    if (s_top > (low ? 0x300u : 0x800000u))
        n64rt_fatal("n64rt: no RDRAM left for the runtime's structures (the game's is %lu bytes)\n", (unsigned long)n64rt_game->mem_size);
    for (u32 a = pa; a < s_top; a += 4) *rd32(a) = 0;
    return (void *)(uintptr_t)N64RT_K0(pa);
}

/* (for messages) */
const char *n64rt_func_name(u32 i) { return i < N64RT_NFUNCS ? s_names[i] : "?"; }

/* The other way in: the ROM is a file of its own -- data slot 0, a .z64 as
 * it is -- which the Pocket loaded at N64RT_ROM_FILE_OFF, where a game file
 * has its ROM0; rom_size is the file's. Sets g->rom_sdram and rom_size,
 * then n64rt_run(g). */
void n64rt_boot_rom(n64rt_game_t *g, u32 rom_size)
{
    boot_checks();
    rom_at(g, N64RT_ROM_FILE_OFF, rom_size);
    n64rt_run(g);
}
