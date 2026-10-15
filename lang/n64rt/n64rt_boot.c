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
/* ---- libultra's 64-bit helpers, done here (n64rt_llsigs.h) ------------------------
 * The game's code calls them with the O32 ABI as the compiler made them: a
 * long long in a register pair, the high word first (this is big-endian), the
 * result in v0 (high) and v1 (low). The originals `break 7` on a zero divisor
 * and `break 6` on a quotient that overflows (a fault of the game's own): done
 * here as 0 -- this code runs at the game's level, where a break would reach the
 * runtime's vector, not the game's.
 *
 * Each runs behind a wrapper (llw_*, below) with the game's interrupts off: this
 * CPU takes the game's interrupt for the runtime's vector, not the game's, when
 * the instruction it interrupts is not in the game's window (KSEG0), and the
 * runtime is not. The game's Status goes back in the delay slot of the return,
 * where the next instruction is the game's again. */
#define LL_BREAK(n) ((void)(n))
u64 ll_ull_rshift(u64 a, u64 b) { return a >> (b & 63u); }
u64 ll_ll_lshift(u64 a, u64 b)  { return a << (b & 63u); }
u64 ll_ll_rshift(u64 a, u64 b)  { return (u64)((s64)a >> (b & 63u)); }
u64 ll_ll_mul(u64 a, u64 b)     { return a * b; }
u64 ll_ull_div(u64 a, u64 b)
{
    if (!b) { LL_BREAK(7); return 0; }
    if (!((a | b) >> 32)) return (u32)a / (u32)b;
    return a / b;
}
u64 ll_ull_rem(u64 a, u64 b)
{
    if (!b) { LL_BREAK(7); return 0; }
    if (!((a | b) >> 32)) return (u32)a % (u32)b;
    return a % b;
}
u64 ll_ll_rem(u64 a, u64 b) { return ll_ull_rem(a, b); }       /* (libultra's __ll_rem is an unsigned remainder) */
u64 ll_ll_div(u64 a, u64 b)
{
    if (!b) { LL_BREAK(7); return 0; }
    if (b == ~0ull && a == 0x8000000000000000ull) { LL_BREAK(6); return 0; }
    return (u64)((s64)a / (s64)b);
}
/* the remainder with the divisor's sign: added back when the two differ */
u64 ll_ll_mod(u64 a, u64 b)
{
    if (!b) { LL_BREAK(7); return 0; }
    if (b == ~0ull && a == 0x8000000000000000ull) { LL_BREAK(6); return 0; }
    s64 r = (s64)a % (s64)b;
    if ((r < 0 && (s64)b > 0) || (r > 0 && (s64)b < 0)) r += (s64)b;
    return (u64)r;
}
/* *q = a / d, *r = a % d, d a 16-bit number the original sign-extends and divides by as unsigned */
void ll_ull_divremi(u64 *q, u64 *r, u64 a, s32 d)
{
    u64 b = (u64)(s64)(s16)d;
    if (!b) { LL_BREAK(7); return; }
    *q = a / b;
    *r = a % b;
}
#include "n64rt_llsigs.h"

#define LL_WRAP(name) \
    ".globl llw_" #name "\n" \
    "llw_" #name ":\n" \
    "    addiu $sp, $sp, -32\n" \
    "    sw    $ra, 28($sp)\n" \
    "    sw    $t7, 24($sp)\n"               /* (the game's Status: the patch has it there) */ \
    "    lw    $t6, 48($sp)\n"               /* (the caller's fifth word, where the C function looks) */ \
    "    sw    $t6, 16($sp)\n" \
    "    jal   ll_" #name "\n" \
    "    nop\n" \
    "    lw    $ra, 28($sp)\n" \
    "    lw    $t7, 24($sp)\n" \
    "    addiu $sp, $sp, 32\n" \
    "    jr    $ra\n" \
    "    mtc0  $t7, $12\n"
__asm__(".text\n.set push\n.set noreorder\n.set noat\n.set mips3\n"
        LL_WRAP(ull_rshift) LL_WRAP(ull_rem) LL_WRAP(ull_div) LL_WRAP(ll_lshift) LL_WRAP(ll_rem)
        LL_WRAP(ll_div) LL_WRAP(ll_mul) LL_WRAP(ull_divremi) LL_WRAP(ll_mod) LL_WRAP(ll_rshift)
        ".set pop\n");
#define LL_DECL(name) void llw_##name(void);
LL_DECL(ull_rshift) LL_DECL(ull_rem) LL_DECL(ull_div) LL_DECL(ll_lshift) LL_DECL(ll_rem)
LL_DECL(ll_div) LL_DECL(ll_mul) LL_DECL(ull_divremi) LL_DECL(ll_mod) LL_DECL(ll_rshift)
static const struct { const u32 *w; u32 n; void *f; const char *name; } s_ll_sigs[] = { LL_SIGS };
static void ll_patch(u32 vram, u32 len)
{
    u32 patched = 0;
    for (u32 a = vram; a + 4u <= vram + len; a += 4u) {
        const u32 *w = rd32(a);
        for (u32 k = 0; k < sizeof s_ll_sigs / sizeof s_ll_sigs[0]; k++) {
            const u32 n = s_ll_sigs[k].n;
            if (w[0] != s_ll_sigs[k].w[0] || a + 4u * n > vram + len) continue;
            /* a CPU that does the function's 64-bit instruction itself runs the original: shifts (W64_SH), dmultu (W64_MD);
             * what divides (ddiv, ddivu: always the runtime's) is replaced on every CPU */
            if (n64rt_cpu_sh && (s_ll_sigs[k].w == s_sig_ull_rshift || s_ll_sigs[k].w == s_sig_ll_lshift || s_ll_sigs[k].w == s_sig_ll_rshift)) continue;
            if (n64rt_cpu_md && s_ll_sigs[k].w == s_sig_ll_mul) continue;
            u32 i = 1;
            while (i < n && w[i] == s_ll_sigs[k].w[i]) i++;
            if (i < n) continue;
            u32 f = (u32)(uintptr_t)s_ll_sigs[k].f;
            u32 *d = rd32(a);
            d[0] = 0x400F6000u;                      /* mfc0 t7, Status                  (the wrapper gives it back) */
            d[1] = 0x35EE0001u;                      /* ori  t6, t7, 1 */
            d[2] = 0x39CE0001u;                      /* xori t6, t6, 1                   (interrupts off) */
            d[3] = 0x408E6000u;                      /* mtc0 t6, Status */
            d[4] = 0;
            d[5] = 0x3C190000u | (f >> 16);          /* lui  t9, hi */
            d[6] = 0x37390000u | (f & 0xFFFFu);      /* ori  t9, t9, lo */
            d[7] = 0x03200008u;                      /* jr   t9 */
            d[8] = 0;
            patched++;
#ifdef N64RT_DEBUG
            n64rt_log("n64rt: __%s at %08lx done in the runtime\n", s_ll_sigs[k].name, (unsigned long)a);
#endif
            break;
        }
    }
    (void)patched;
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
    const u32 *rom = (const u32 *)(uintptr_t)g->rom_sdram;
    for (u32 i = 0; i < g->boot_len / 4u; i++) *rd32(g->boot_vram + 4u * i) = rom[g->boot_rom / 4u + i];
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
        for (u32 i = 0; i < 0x400u / 4u; i++) *rd32(4u * i) = rom[0x4C0u / 4u + i];
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
            *rd32(4u * i) = rom[0x4F0u / 4u + i] ^ x;
    /* What a CIC-6105 IPL3 leaves at 0x2FE1C0: a word of its own code, ROM 0x750 (0xAD170014, the `sw s7, 20(t0)` that stores the
     * boot parameters). Donkey Kong 64 (a 6105 game) checks it right after osInitialize and, finding anything else, spins on it
     * for ever: a black screen. Every 6105 ROM has the same IPL3, so this is the CIC's, whatever the game. */
    if (g->cic == 6105u && g->rom_size >= 0x1000u) *rd32(0x2FE1C0u) = rom[0x750u / 4u];
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
    /* (after the CPU is probed: what it does itself stays the game's) */
    ll_patch(g->boot_vram, g->boot_len);               /* (but the compiler's 64-bit helpers: n64rt_llsigs.h) */
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
 * that leaves it (layout.h), in the SDRAM: a ROM of up to 48 MiB. Nothing
 * is moved. The port links its boot stack below
 * N64RT_ROM_FILE_OFF (--defsym=_fstack=0x400FFFF0). */
#define FILE_SDRAM 0x40000000u
static void no_rom(const char *msg) __attribute__((noreturn));
static void no_rom(const char *msg)
{
    printf("n64rt: %s\n", msg);
    notice(N64RT_RDRAM_SDRAM, msg);         /* (on the screen too: nothing of the game's is in the RDRAM yet) */
    for (;;) ;
}
/* the word the Pocket loaded at an offset from the program's start */
static u32 file_word(u32 off)
{
    return *(const volatile u32 *)(uintptr_t)(FILE_SDRAM + off);
}
u32 n64rt_rom_word(u32 off)
{
    return *(const volatile u32 *)(uintptr_t)(n64rt_game->rom_sdram + off);
}
/* what both ways in begin with */
static void boot_checks(void)
{
    /* A core of another memory layout (its boot ROM has no mark: layout.h)
     * has the saves' slots elsewhere, in what is the RDRAM here: nothing is
     * run on it, so the Pocket reads back from them what it loaded. */
    if (*(const volatile u32 *)(uintptr_t)(0xBFC00000u + LAYOUT_MARK_OFS) != LAYOUT_MARK)
        no_rom("This program needs a newer Mirlo core, with its data.json: nothing was started. The saves are as they were.");
}
/* the ROM is `size` bytes at `off` of what the Pocket loaded */
static void rom_at(n64rt_game_t *g, u32 off, u32 size)
{
    if (off < N64RT_ROM_FILE_OFF || off >= LAYOUT_FILE_SDRAM || (off & 3u) || size < 0x1000u)
        no_rom("no ROM at 1 MiB of what the Pocket loaded");
    if (size > LAYOUT_FILE_SDRAM - off) no_rom("ROM exceeds maximum file size of 48 MiB.");
    if (file_word(off) != 0x80371240u) no_rom("the ROM is not a .z64 ROM");
    g->rom_sdram = FILE_SDRAM + off;
    g->rom_size = size;
}

void n64rt_boot_game_file(n64rt_game_t *g)
{
    boot_checks();
    u32 fsz = apf_bridge_file_size_read();
    if (fsz < 64u) no_rom("no game file");
    if (fsz > LAYOUT_FILE_SDRAM) no_rom("ROM exceeds maximum file size of 48 MiB.");
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
