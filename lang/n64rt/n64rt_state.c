/* n64rt: save states -- the Pocket's Sleep and its Memories (docs/mips.md, "Save states").
 *
 * The Pocket asks the core for a state (Savestate Start: on Sleep, or a Memory made from its menu), reads it out of
 * the SDRAM when the core says it is ready, and keeps it in a file; to load one (on the wake from a Sleep, or a Memory
 * chosen) it starts the core as for any launch -- the ROM and the data slots loaded, the game booted afresh -- copies
 * the file back where the core said, and asks for its load (Savestate Load). The state is one block of the SDRAM,
 * LAYOUT_STATE (layout.h; target/pocket/core_top.sv tells the Pocket its address and its size), outside every memory
 * the game or the runtime uses: the game goes on while the Pocket reads a Memory out, and the game the wake booted
 * runs while the Pocket copies one in -- neither touches it.
 *
 * The block holds as much as the state needs, not more: its memories (DMEM, the save, the Controller Pak, the coverage plane,
 * the RDRAM and the runtime's own) are each a stream of tokens over 32-bit words -- a count of words that follow as they are,
 * or a word repeated -- which is some tenth of the RDRAM when a game has used little of it, and the size the core tells the
 * Pocket (APF_BRIDGE.TRANSFER_LENGTH's bits 23:20, in MiB) is what was written: the Pocket reads, keeps and copies back that much, and its time
 * goes with the size. The header and the registers are at fixed offsets, uncompressed.
 *
 * Both requests come to the monitor (MONITOR.PEND bit 5: n64rt_rcp_events), with the game stopped wherever it was,
 * its registers in n64rt_mon_ctx, as at any of the monitor's wake-ups. A state is then
 *
 *   - the runtime's own memory, its .data and .bss whole: the monitor's context (the game's registers, 64 bits,
 *     HI, LO, the FPU's), the game's TLB entries and the kept ones, the emulated PI and RI registers, the saves'
 *     sizes and kinds, the stacks -- everything the runtime knows of the game. Its code is not in the state: a state
 *     is for the same n64rt.sys (a sum of its code and constants says so) and the same ROM (its header);
 *   - the RDRAM, 8 MiB, and the RDP's hidden coverage bits of it (LAYOUT_HID);
 *   - the cartridge's save, a FlashRAM's page buffer and status words, the Controller Pak: the memories the game
 *     reads as they were when the state was made (the Pocket's files are written back when the core quits);
 *   - the RCP's registers as the hardware has them, with what they hide: the VI's base and field (rcp_vi.sv's
 *     0x38), its line, the MI's mode, interrupts and mask, the PIF RAM, the RSP's DMEM, its status and PC, the DPC's
 *     registers, and the CPU's COP0 registers the game has (Count, Compare, EPC, Cause's software bits, the TLB's
 *     registers) and its FPU's condition bit.
 *
 * What is not kept, because the state is made with the hardware at rest: before a state is made (and before one is
 * put back) the monitor waits, with the game stopped, for the RSP to halt, its DMA, the RDP, the PI's engine and the
 * SI to be idle and the AI to have played its buffers -- the tasks the game started end, and their interrupts are
 * pending for the game in the state. So the RSP's registers and IMEM (a task loads its microcode into IMEM, and its
 * registers, every time it starts), the AI's queue and the SI's DMA are not in it. Neither are the RDP's modes and
 * its TMEM, which no register reads: a game sets what it draws with in every frame's list, so the first frame after
 * a load is drawn as it would have been.
 *
 * A load puts the hardware back first, then the memories, waits for the VI's line to be the one of the state (the
 * game's next VI interrupt is as far away as it was), puts back Count, and last copies the runtime's memory over its
 * own and leaves the monitor as any entry does (n64rt_state_resume, n64rt_exc.S): the game goes on where it was. */
#include <string.h>
#include <system.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <layout.h>
#include "n64rt.h"

#define SS_MAGIC    0x4D535331u         /* "MSS1" */
#define SS_VERSION  2u
/* the state's block (layout.h LAYOUT_STATE, LAYOUT_STATE_BYTES, its largest): the header and the registers at fixed offsets,
 * then the regions one after the other, each a stream of tokens (rle_put) of the words of a memory */
#define SS_HEAD     0x000000u           /* ss_head_t */
#define SS_HW       0x001000u           /* ss_hw_t */
#define SS_BODY     0x003000u           /* the first region */
#define SS_RT_MAX   0x03C000u           /* the runtime's .data and .bss, at most */
enum { RG_DMEM, RG_SAVE, RG_CPAK, RG_HID, RG_RDRAM, RG_RT, RG_N };      /* (the runtime's last: a load goes on from its copy) */
/* what a load decodes the runtime's memory into before it is copied over the runtime's own, and what a save keeps RDRAM's first 4 KiB
 * in while DMEM goes through them: SDRAM nothing else uses, above the block's largest size */
#define SS_SCR_RT   (LAYOUT_STATE + 0x00B00000u)
#define SS_SCR_RAM  (LAYOUT_STATE + 0x00B40000u)
_Static_assert(SS_SCR_RAM + 0x1000u <= LAYOUT_SAVEDB, "the scratch areas lie below the save-type database");
_Static_assert(SS_BODY + 0x1000u + SS_RT_MAX + LAYOUT_SAVE_MAX + 0x1000u + LAYOUT_PAK_BYTES + 0x100000u + 0x800000u + 64u <= LAYOUT_STATE_BYTES,
               "the largest state fits the size the core tells the Pocket it may load: target/pocket/core_top.sv's SS_SIZE");
#define SS_AT(o)    (LAYOUT_STATE + (o))

typedef struct {
    u32 magic, version, bytes, layout;  /* SS_MAGIC, SS_VERSION, the state's size (what the Pocket is told), LAYOUT_MARK */
    u32 code_sum, code_bytes;           /* the runtime's code and constants: a state is for this n64rt.sys */
    u32 rt_bytes, mon_id;               /* its .data and .bss; MONITOR.ID: the core's blocks */
    u32 rom[12];                        /* the ROM's header, 0x10 - 0x3F: its check words, its title, its ID */
    u32 off[RG_N], len[RG_N];           /* where each region's tokens are in the block, and how many bytes */
} ss_head_t;

typedef struct {
    /* the CPU's COP0 (Status and ErrorEPC are the context's) and its FPU's condition bit */
    u32 index, entrylo0, entrylo1, context, pagemask, wired, badvaddr, count, entryhi, compare, cause, epc;
    u32 config, lladdr, watchlo, watchhi, taglo, mctl, fcc;
    /* the MI */
    u32 mi_mode, mi_intr, mi_mask;
    /* the VI: its registers (CURRENT: the line), the base and field (rcp_vi.sv's 0x38) */
    u32 vi[16], vi_base;
    /* the AI: STATUS, DACRATE */
    u32 ai_status, ai_dac;
    /* the SI: DRAM_ADDR, the PIF RAM, the block holds commands (rcp_si.sv's joy) */
    u32 si_dram, pif[16], si_joy;
    /* the RSP: SP_STATUS, SP_PC, SP_SEMAPHORE, SP_MEM_ADDR, SP_DRAM_ADDR */
    u32 sp_status, sp_pc, sp_sema, sp_mem, sp_dram;
    /* the DPC: START, END, CURRENT, STATUS */
    u32 dpc_start, dpc_end, dpc_cur, dpc_status;
    /* the monitor's: CTL, MASK, PI2 */
    u32 mon_ctl, mon_mask, mon_pi2;
    /* the Pocket's table of file sizes: the save's slot and the Controller Pak's */
    u32 fsize[3];
    u64 uptime;                         /* the SoC's clock when the state was made */
} ss_hw_t;
_Static_assert(sizeof(ss_head_t) <= 0x1000u && sizeof(ss_hw_t) <= 0x2000u, "the state's header and registers");

extern char _ftext[], _erodata[], _fdata[], _end[];
void n64rt_state_resume(const void *src, void *dst, u32 words, const void *arg) __attribute__((noreturn));   /* n64rt_exc.S */
void n64rt_rcp_events(void);
void n64rt_vi_hw_event(void);
void n64rt_ai_hw_event(void);
void n64rt_tr_flush(void);              /* n64rt_os.c */
void n64rt_io_rebase(u32 cycles);       /* n64rt_io.c: the SoC's clock moved on by this much under the runtime's feet */
extern u32 n64rt_si_joy;                /* n64rt_io.c: the PIF has run commands (rcp_si.sv's joy is set) */

/* ---- the hardware's registers, as the runtime reaches them ----------------------------------------------------- */
static inline u32 hw_rd(u32 pa) { return *(volatile u32 *)(uintptr_t)(0xA0000000u | pa); }
static inline void hw_wr(u32 pa, u32 v) { *(volatile u32 *)(uintptr_t)(0xA0000000u | pa) = v; }
#define RSP(o)          (*(volatile u32 *)(uintptr_t)(0x20000000u + (o)))
#define SP_MEM_ADDR     RSP(0x00)
#define SP_DRAM_ADDR    RSP(0x04)
#define SP_RD_LEN       RSP(0x08)
#define SP_WR_LEN       RSP(0x0C)
#define SP_STATUS       RSP(0x10)
#define SP_DMA_FULL     RSP(0x14)
#define SP_DMA_BUSY     RSP(0x18)
#define SP_SEMAPHORE    RSP(0x1C)
#define SP_PC           RSP(0x20)
#define DPC_START       RSP(0x40)
#define DPC_END         RSP(0x44)
#define DPC_CURRENT     RSP(0x48)
#define DPC_STATUS      RSP(0x4C)
#define MI_MODE         0x04300000u
#define MI_MASK         0x0430000Cu
#define VI_REG(n)       (0x04400000u + 4u * (n))
#define VI_BASE         0x04400038u     /* (rcp_vi.sv: the runtime's) */
#define AI_CONTROL      0x04500008u
#define AI_STATUS       0x0450000Cu
#define AI_DACRATE      0x04500010u
#define SI_DRAM_ADDR    0x04800000u
#define SI_STATUS       0x04800018u
#define PIF_RAM         0x1FC007C0u

#define MFC0(r)     ({ u32 _v; __asm__ volatile("mfc0 %0, $" #r : "=r"(_v)); _v; })
#define MTC0(r, v)  __asm__ volatile("mtc0 %0, $" #r "\n\tnop\n\tnop" :: "r"((u32)(v)))

static inline u64 uptime(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
#define MS(n) ((u64)(n) * (CONFIG_CLOCK_FREQUENCY / 1000u))

static void copy_words(u32 dst, u32 src, u32 bytes)
{
    volatile u32 *d = (volatile u32 *)(uintptr_t)dst;
    const volatile u32 *s = (const volatile u32 *)(uintptr_t)src;
    for (u32 n = bytes / 16u; n; n--, d += 4, s += 4) {
        u32 a = s[0], b = s[1], c = s[2], e = s[3];
        d[0] = a; d[1] = b; d[2] = c; d[3] = e;
    }
}
/* A memory as tokens: a word with bit 31 clear is the count of words that follow it as they are; with bit 31 set, the count of
 * times the word after it is repeated (three or more). rle_put writes from s to d and gives where it ended; rle_get puts
 * n words at d from the tokens at s and gives where they ended. */
static u32 *rle_put(u32 *d, const u32 *s, u32 n)
{
    const u32 *lit = s, *const end = s + n;
    while (s < end) {
        const u32 v = *s;
        const u32 *t = s + 1;
        while (t < end && *t == v) t++;
        if (t - s >= 3) {
            if (s > lit) { *d++ = (u32)(s - lit); do *d++ = *lit++; while (lit < s); }
            *d++ = 0x80000000u | (u32)(t - s);
            *d++ = v;
            lit = t;
        }
        s = t;
    }
    if (end > lit) { *d++ = (u32)(end - lit); do *d++ = *lit++; while (lit < end); }
    return d;
}
static const u32 *rle_get(u32 *d, const u32 *s, u32 n)
{
    u32 *const end = d + n;
    while (d < end) {
        const u32 t = *s++;
        u32 c = t & 0x7FFFFFFFu;
        if (!c) break;                                  /* (a state that is not one: no endless loop) */
        if (c > (u32)(end - d)) c = (u32)(end - d);
        if (t >> 31) { const u32 v = *s++; do *d++ = v; while (--c); }
        else do *d++ = *s++; while (--c);
    }
    return s;
}
static u32 *put_region(ss_head_t *h, u32 rg, u32 *d, const void *src, u32 bytes)
{
    h->off[rg] = (u32)((uintptr_t)d - LAYOUT_STATE);
    u32 *e = rle_put(d, (const u32 *)src, bytes / 4u);
    h->len[rg] = (u32)((uintptr_t)e - (uintptr_t)d);
    return e;
}
/* a region back to dst; 0 if its tokens did not end where the header says */
static int get_region(const ss_head_t *h, u32 rg, void *dst, u32 bytes)
{
    const u32 *e = rle_get((u32 *)dst, (const u32 *)(uintptr_t)SS_AT(h->off[rg]), bytes / 4u);
    return (u32)((uintptr_t)e - LAYOUT_STATE) == h->off[rg] + h->len[rg];
}
static u32 code_sum(void)
{
    u32 s = 0x4D49524Cu;
    for (const volatile u32 *p = (const volatile u32 *)_ftext; p < (const volatile u32 *)_erodata; p++) s = (s << 5 | s >> 27) ^ *p;
    return s;
}
static u32 slot_size(u32 id, u32 want)  /* a slot's size in the Pocket's table; with want, raised to it */
{
    u32 prev = apf_bridge_slot_id_read();
    apf_bridge_slot_id_write(id);
    for (volatile int i = 0; i < 256; i++) { }
    u32 size = apf_bridge_file_size_read();
    if (want > size) { apf_bridge_file_size_write(want); for (volatile int i = 0; i < 256; i++) { } }
    apf_bridge_slot_id_write(prev);
    return size;
}
static void answer(int failed, u32 bytes)   /* bytes: a state's size, a whole number of MiB (the Pocket reads that much); 0 for a load */
{
    if (bytes) apf_bridge_transfer_length_write(bytes);     /* (before the answer: APF_BRIDGE.TRANSFER_LENGTH is the size the Pocket is told) */
    __asm__ volatile("" ::: "memory");  /* (every store of the state before it: the Pocket reads it out once told) */
    apf_bridge_savestate_done_write(failed ? 1u : 0u);
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: save state: %s\n", failed ? "failed" : "done");
#endif
}

/* ---- the hardware at rest --------------------------------------------------------------------------------------
 * The RSP halted, no DMA of its own; the RDP's list done (CURRENT at END, or past it: a microcode writes an END
 * behind CURRENT) and the RDP idle; the PI's engine and the SI idle; the AI's buffers played. Meanwhile the monitor's
 * other wake-ups are answered as they come (the PIF's commands that an SI read waits for, the VI's changes): the
 * game is stopped, its hardware goes on. For a load (force) the game is the one the wake booted, which is thrown
 * away: its RSP is halted where it is if it does not stop by itself, and the rest is waited for as long as it takes
 * (up to a second), then taken as it is. */
static int rcp_quiet(void)
{
    if (!(SP_STATUS & 1u) || SP_DMA_BUSY || SP_DMA_FULL) return 0;
    if (DPC_CURRENT < DPC_END || !rdp_status_idle_read() || (DPC_STATUS & 0x200u)) return 0;      /* (END valid: a list waiting) */
    if (monitor_miintr_read() & 0x10000u) return 0;
    if (hw_rd(SI_STATUS) & 3u) return 0;
    if (hw_rd(AI_STATUS) & 0xC0000001u) return 0;
    return 1;
}
static int quiesce(int force)
{
    const u64 t0 = uptime();
    int halted = 0;
    while (!rcp_quiet()) {
        if (monitor_pend_read() & monitor_mask_read() & 0x1Fu) n64rt_rcp_events();
        const u64 t = uptime() - t0;
        if (force && !halted && t > MS(200)) { SP_STATUS = 2u; halted = 1; }       /* (SP_STATUS: set halt) */
        if (t > MS(force ? 1000 : 500)) {
#ifdef N64RT_DEBUG
            n64rt_log("n64rt: save state: the hardware not at rest: SP %08lx DPC %06lx/%06lx %08lx MI %08lx SI %08lx AI %08lx\n",
                      (unsigned long)SP_STATUS, (unsigned long)DPC_CURRENT, (unsigned long)DPC_END, (unsigned long)DPC_STATUS,
                      (unsigned long)monitor_miintr_read(), (unsigned long)hw_rd(SI_STATUS), (unsigned long)hw_rd(AI_STATUS));
#endif
            return 0;
        }
    }
    return 1;
}

/* ---- the RSP's DMEM, through the RDRAM's first 4 KiB (its DMA reaches the RDRAM only) --------------------------- */
static void sp_dma(u32 sp, u32 rdram, u32 bytes, int to_rdram)
{
    while (SP_DMA_FULL || SP_DMA_BUSY) { }
    SP_MEM_ADDR = sp; SP_DRAM_ADDR = rdram;
    if (to_rdram) SP_WR_LEN = bytes - 1u; else SP_RD_LEN = bytes - 1u;
    while (SP_DMA_FULL || SP_DMA_BUSY) { }
}

static void time_put(const ss_hw_t *w);

/* ---- the Pocket's read of the state ------------------------------------------------------------------------------
 * Once the state is answered the Pocket reads its block out through the bridge, a MiB takes it seconds, and the port it reads by is
 * the one the AI's reader and the game's own SDRAM accesses go through: a game that goes on meanwhile has its audio buffers
 * come late (the audio crackles for as long as the read lasts, 17 s for a 4 MB state) and its frames slow. So the game stays
 * stopped, as for the state, until the read is over. The core does not say when that is; it shows in the time a read of the
 * RDRAM takes: the baseline is taken before the answer, the contended one in the first moments after, and the read is over
 * when the time is back near the baseline and stays there. If the contention is not to be seen the wait is as long as the
 * size says it takes (RD_MS_PER_MIB), and it is never over RD_MAX_MS. */
#define RD_MS_PER_MIB   4000u
#define RD_MAX_MS       40000u
#define RD_QUIET_MS     1000u
static u32 probe(void)                                  /* the CPU cycles a cold read of 4 KiB of the RDRAM takes */
{
    const u32 at = LAYOUT_RDRAM + 0x600000u;
    flush_cpu_dcache_range((const void *)(uintptr_t)at, 0x1000u);
    const volatile u32 *p = (const volatile u32 *)(uintptr_t)at;
    u32 sum = 0;
    const u64 t0 = uptime();
    for (u32 n = 0; n < 1024u; n += 8u) sum += p[n];
    const u32 t = (u32)(uptime() - t0);
    __asm__ volatile("" :: "r"(sum));
    return t;
}
static u32 probe_avg(u32 n)
{
    u64 sum = 0;
    for (u32 i = 0; i < n; i++) sum += probe();
    return (u32)(sum / n);
}
static void readout_wait(u32 mib, u32 base)
{
    const u64 t0 = uptime();
    u32 busy = 0;                                       /* the time with the Pocket reading, once seen */
    u64 seen = t0;                                      /* the last time a read was slow */
    u32 n = 0;
    u64 sum = 0;
    for (;;) {
        if (monitor_pend_read() & monitor_mask_read() & 0x1Fu) n64rt_rcp_events();
        const u64 now = uptime();
        const u64 t = now - t0;
        if (t > MS(RD_MAX_MS)) return;
        const u32 d = probe();
        if (!busy) {                                    /* the first 300 ms: how slow a read is with the Pocket's */
            if (t < MS(300)) { sum += d; n++; continue; }
            const u32 c = n ? (u32)(sum / n) : base;
            busy = c > 2u * base ? base + (c - base) / 2u : 0xFFFFFFFFu;    /* (0xFFFFFFFF: not to be seen) */
        }
        if (busy == 0xFFFFFFFFu) { if (t >= MS((u64)mib * RD_MS_PER_MIB)) return; }
        else {
            if (d > busy) seen = now;
            else if (now - seen >= MS(RD_QUIET_MS)) return;
        }
        for (const u64 w = uptime(); uptime() - w < MS(2); ) { }
    }
}

/* ---- a state made --------------------------------------------------------------------------------------------- */
static void save(void)
{
    ss_head_t *h = (ss_head_t *)(uintptr_t)SS_AT(SS_HEAD);
    ss_hw_t *w = (ss_hw_t *)(uintptr_t)SS_AT(SS_HW);
    const u32 rt = (u32)(_end - _fdata);
    h->magic = 0;                                       /* (a state half made is no state) */
    if (n64rt_pak_kind(0) >= N64RT_PAK_TRANSFER || rt > SS_RT_MAX || !quiesce(0)) { answer(1, 0); return; }
    memset(w, 0, sizeof *w);
    w->uptime = uptime();
    w->count = MFC0(9);                                 /* (the game's time as the hardware came to rest: time_put puts it back) */
    /* COP0 */
    w->index = MFC0(0); w->entrylo0 = MFC0(2); w->entrylo1 = MFC0(3); w->context = MFC0(4); w->pagemask = MFC0(5);
    w->wired = MFC0(6); w->badvaddr = MFC0(8); w->entryhi = MFC0(10); w->compare = MFC0(11); w->cause = MFC0(13);
    w->epc = MFC0(14); w->config = MFC0(16); w->lladdr = MFC0(17); w->watchlo = MFC0(18); w->watchhi = MFC0(19);
    w->taglo = MFC0(28); w->mctl = MFC0(7);
    { u32 c; __asm__ volatile(".set push\n.set noreorder\nli %0, 0\nbc1t 1f\nnop\nb 2f\nnop\n1: li %0, 1\n2:\n.set pop" : "=r"(c)); w->fcc = c; }
    /* the MI */
    w->mi_mode = hw_rd(MI_MODE);
    { const u32 m = monitor_miintr_read(); w->mi_intr = m & 0x3Fu; w->mi_mask = (m >> 8) & 0x3Fu; }
    /* the VI */
    for (u32 n = 0; n < 14u; n++) w->vi[n] = hw_rd(VI_REG(n));
    w->vi_base = hw_rd(VI_BASE);
    /* the AI, the SI */
    w->ai_status = hw_rd(AI_STATUS); w->ai_dac = monitor_airate_read() & 0x3FFFu;
    w->si_dram = hw_rd(SI_DRAM_ADDR);
    for (u32 n = 0; n < 16u; n++) w->pif[n] = hw_rd(PIF_RAM + 4u * n);
    w->si_joy = n64rt_si_joy;
    /* the RSP (its semaphore is taken by the read: given back when it was free), the DPC */
    w->sp_status = SP_STATUS; w->sp_pc = SP_PC; w->sp_mem = SP_MEM_ADDR; w->sp_dram = SP_DRAM_ADDR;
    w->sp_sema = SP_SEMAPHORE;
    if (!w->sp_sema) SP_SEMAPHORE = 0;
    w->dpc_start = DPC_START; w->dpc_end = DPC_END; w->dpc_cur = DPC_CURRENT; w->dpc_status = DPC_STATUS;
    /* the monitor's, the Pocket's table */
    w->mon_ctl = monitor_ctl_read(); w->mon_mask = monitor_mask_read(); w->mon_pi2 = monitor_pi2_read();
    w->fsize[1] = slot_size(1, 0); w->fsize[2] = slot_size(LAYOUT_CPAK_SLOT, 0);
    /* the memories, region after region; DMEM through the RDRAM's first 4 KiB, which wait in the scratch area meanwhile */
    u32 *d = (u32 *)(uintptr_t)SS_AT(SS_BODY);
    copy_words(SS_SCR_RAM, LAYOUT_RDRAM, 0x1000u);
    sp_dma(0x0000u, 0x000000u, 0x1000u, 1);
    flush_cpu_dcache_range((const void *)(uintptr_t)LAYOUT_RDRAM, 0x1000u);
    d = put_region(h, RG_DMEM, d, (const void *)(uintptr_t)LAYOUT_RDRAM, 0x1000u);
    copy_words(LAYOUT_RDRAM, SS_SCR_RAM, 0x1000u);
    SP_MEM_ADDR = w->sp_mem; SP_DRAM_ADDR = w->sp_dram;
    d = put_region(h, RG_SAVE, d, (const void *)(uintptr_t)LAYOUT_SAVE, LAYOUT_SAVE_MAX + 0x1000u);
    d = put_region(h, RG_CPAK, d, (const void *)(uintptr_t)LAYOUT_CPAK, LAYOUT_PAK_BYTES);
    d = put_region(h, RG_HID, d, (const void *)(uintptr_t)LAYOUT_HID, 0x100000u);
    d = put_region(h, RG_RDRAM, d, (const void *)(uintptr_t)LAYOUT_RDRAM, 0x800000u);
    /* the header, and last the runtime's own memory: a load goes on from here (n64rt_state_resumed) */
    h->version = SS_VERSION; h->layout = LAYOUT_MARK;
    h->code_sum = code_sum(); h->code_bytes = (u32)(_erodata - _ftext); h->rt_bytes = rt; h->mon_id = monitor_id_read();
    for (u32 n = 0; n < 12u; n++) h->rom[n] = n64rt_rom_word(0x10u + 4u * n);
    d = put_region(h, RG_RT, d, _fdata, (rt + 15u) & ~15u);
    const u32 total = ((u32)((uintptr_t)d - LAYOUT_STATE) + 15u) & ~15u;
    h->bytes = total;
    h->magic = SS_MAGIC;
    const u32 padded = (total + 0xFFFFFu) & ~0xFFFFFu;    /* (what the core tells the Pocket: whole MiB, 4 bits of a register) */
    const u32 quiet = probe_avg(8);                     /* (what a read of the RDRAM takes with nothing else on the SDRAM's port) */
    answer(0, padded);
    readout_wait(padded >> 20, quiet);
    n64rt_io_rebase((u32)(uptime() - w->uptime));       /* (the wait is no time of the runtime's) */
    time_put(w);
}

/* ---- a state put back ----------------------------------------------------------------------------------------- */
static int state_ok(const ss_head_t *h)
{
    if (h->magic != SS_MAGIC || h->version != SS_VERSION || h->bytes < SS_BODY || h->bytes > LAYOUT_STATE_BYTES || h->layout != LAYOUT_MARK) return 0;
    if (h->code_bytes != (u32)(_erodata - _ftext) || h->rt_bytes != (u32)(_end - _fdata) || h->code_sum != code_sum()) return 0;
    if (h->mon_id != monitor_id_read()) return 0;
    for (u32 n = 0; n < 12u; n++) if (h->rom[n] != n64rt_rom_word(0x10u + 4u * n)) return 0;
    for (u32 r = 0; r < RG_N; r++) if (h->off[r] < SS_BODY || h->len[r] > h->bytes || h->off[r] > h->bytes - h->len[r]) return 0;
    return 1;
}
static int rsp_put(const ss_head_t *h, const ss_hw_t *w)
{
    /* DMEM through the RDRAM's first 4 KiB (the RDRAM is put back after this) */
    const int ok = get_region(h, RG_DMEM, (void *)(uintptr_t)LAYOUT_RDRAM, 0x1000u);
    sp_dma(0x0000u, 0x000000u, 0x1000u, 0);
    /* BROKE: only a BREAK sets it -- one, at IMEM's start (whose words the next task loads anew), run with the interrupt
     * on a BREAK off; else cleared */
    SP_STATUS = 0x80u | 0x20u;                          /* (clear the interrupt on a BREAK, single step) */
    if (w->sp_status & 2u) {
        *(volatile u32 *)(uintptr_t)LAYOUT_RDRAM = 0x0000000Du;
        *(volatile u32 *)(uintptr_t)(LAYOUT_RDRAM + 4u) = 0x0000000Du;
        sp_dma(0x1000u, 0x000000u, 8u, 0);
        SP_PC = 0;
        SP_STATUS = 1u;                                 /* (clear halt) */
        for (u32 n = 0; !(SP_STATUS & 1u) && n < 100000u; n++) { }
    } else SP_STATUS = 4u;                              /* (clear broke) */
    SP_PC = w->sp_pc;
    u32 st = 0;
    st |= (w->sp_status & 0x20u) ? 0x40u : 0x20u;       /* single step */
    st |= (w->sp_status & 0x40u) ? 0x100u : 0x80u;      /* the interrupt on a BREAK */
    for (u32 i = 0; i < 8u; i++) st |= ((w->sp_status >> (7u + i)) & 1u) ? 2u << (9u + 2u * i) : 1u << (9u + 2u * i);
    SP_STATUS = st;
    SP_STATUS = (w->mi_intr & 1u) ? 0x10u : 0x08u;      /* the SP interrupt, as the MI had it */
    if (w->sp_sema) (void)SP_SEMAPHORE; else SP_SEMAPHORE = 0;
    SP_MEM_ADDR = w->sp_mem; SP_DRAM_ADDR = w->sp_dram;
    return ok;
}
static void dpc_put(const ss_hw_t *w)
{
    /* CURRENT and END as they were (a START at CURRENT, then END: nothing to fetch), then a START that was waiting for
     * its END; START_GCLK and PIPE_BUSY are cleared by a SYNC FULL, through the RDP's command port, when they were */
    DPC_STATUS = (w->dpc_status & 1u) ? 2u : 1u;        /* XBUS */
    DPC_START = w->dpc_cur;
    DPC_END = w->dpc_end;
    if (!(w->dpc_status & 0x28u)) {
        rdp_cmd_data_write(0x29000000u); rdp_cmd_data_write(0u);
        for (u32 n = 0; !rdp_status_idle_read() && n < 1000000u; n++) { }
    }
    if (w->dpc_status & 0x400u) DPC_START = w->dpc_start;
    DPC_STATUS = ((w->dpc_status & 2u) ? 8u : 4u) | ((w->dpc_status & 4u) ? 0x20u : 0x10u);   /* FREEZE, FLUSH */
}
static void vi_put(const ss_hw_t *w)
{
    for (u32 n = 0; n < 14u; n++) if (n != 4u) hw_wr(VI_REG(n), w->vi[n]);
    hw_wr(VI_BASE, w->vi_base);
}
static void mi_put(const ss_hw_t *w)
{
    const u32 m = w->mi_mode;
    hw_wr(MI_MODE, (m & 0x7Fu) | ((m & 0x80u) ? 0x100u : 0x80u) | ((m & 0x100u) ? 0x400u : 0x200u) | ((m & 0x200u) ? 0x2000u : 0x1000u));
    u32 k = 0;
    for (u32 i = 0; i < 6u; i++) k |= ((w->mi_mask >> i) & 1u) ? 2u << (2u * i) : 1u << (2u * i);
    hw_wr(MI_MASK, k);
}
/* The game's time as the state has it: the time it takes to make a state (the copies) or to put one back is not the
 * game's -- it was stopped when the hardware came to rest, and it goes on as from there. The VI's line as it was (the
 * game's next VI interrupt as far ahead, two fields at most) and its field; the MI's interrupts as they were (a VI
 * interrupt meanwhile is not the game's); Compare (its write clears Cause.IP7) and the timer's interrupt if it was
 * pending (Count brought to Compare), and Count. */
static void time_put(const ss_hw_t *w)
{
    const u32 line = w->vi[4] >> 1;
    const u64 t0 = uptime();
    while ((hw_rd(VI_REG(4)) >> 1) != line && uptime() - t0 < MS(50)) { }
    hw_wr(VI_BASE, w->vi_base);
    monitor_miclr_write(~w->mi_intr & 0x3Eu);
    monitor_miraise_write(w->mi_intr & 0x3Eu);
    MTC0(11, w->compare);
    if (w->cause & 0x8000u) {
        MTC0(9, w->compare - 2u);
        for (u32 n = 0; !(MFC0(13) & 0x8000u) && n < 1000u; n++) { }
    }
    MTC0(9, w->count);
}
static void load(void)
{
    const ss_head_t *h = (const ss_head_t *)(uintptr_t)SS_AT(SS_HEAD);
    const ss_hw_t *w = (const ss_hw_t *)(uintptr_t)SS_AT(SS_HW);
    flush_cpu_dcache();                                 /* (the Pocket wrote it behind the D-cache) */
    if (!state_ok(h)) { answer(1, 0); return; }
    (void)quiesce(1);
    /* the hardware, then the memories (DMEM and the RSP's BREAK went through the RDRAM) */
    int ok = rsp_put(h, w);
    dpc_put(w);
    hw_wr(SI_DRAM_ADDR, w->si_dram);
    for (u32 n = 0; n < 15u; n++) hw_wr(PIF_RAM + 4u * n, w->pif[n]);
    hw_wr(PIF_RAM + 60u, (w->pif[15] & ~3u) | (w->si_joy ? 1u : 0u));     /* (the run flag: the block holds commands) */
    hw_wr(AI_DACRATE, w->ai_dac);
    hw_wr(AI_CONTROL, (w->ai_status >> 25) & 1u);
    vi_put(w);
    monitor_pi2_write(w->mon_pi2);
    monitor_ctl_write(w->mon_ctl);
    monitor_mask_write(w->mon_mask);
    ok &= get_region(h, RG_SAVE, (void *)(uintptr_t)LAYOUT_SAVE, LAYOUT_SAVE_MAX + 0x1000u);
    ok &= get_region(h, RG_CPAK, (void *)(uintptr_t)LAYOUT_CPAK, LAYOUT_PAK_BYTES);
    (void)slot_size(1, w->fsize[1]); (void)slot_size(LAYOUT_CPAK_SLOT, w->fsize[2]);
    ok &= get_region(h, RG_HID, (void *)(uintptr_t)LAYOUT_HID, 0x100000u);
    ok &= get_region(h, RG_RDRAM, (void *)(uintptr_t)LAYOUT_RDRAM, 0x800000u);
    ok &= get_region(h, RG_RT, (void *)(uintptr_t)SS_SCR_RT, (h->rt_bytes + 15u) & ~15u);
    flush_cpu_dcache(); flush_cpu_icache();
    n64rt_tr_flush();                                   /* (the CPU's tables: filled again from the state's entries) */
    mi_put(w);
    /* COP0 (BadVAddr, Context's BadVPN2, Index's P, Cause's BD and ExcCode: the monitor level's to write) */
    MTC0(0, w->index); MTC0(2, w->entrylo0); MTC0(3, w->entrylo1); MTC0(5, w->pagemask); MTC0(6, w->wired);
    MTC0(4, w->context); MTC0(8, w->badvaddr); MTC0(10, w->entryhi); MTC0(13, w->cause); MTC0(14, w->epc);
    MTC0(16, w->config); MTC0(17, w->lladdr); MTC0(18, w->watchlo); MTC0(19, w->watchhi); MTC0(28, w->taglo);
    MTC0(7, (MFC0(7) & 3u) | (w->mctl & 0x500u) | 0xA00u);  /* (the pass-through, the 4 MiB of RDRAM: each with its enable) */
    time_put(w);
    monitor_pend_write(0x1Fu);          /* (the booted game's wake-ups: the state's runtime computes the VI's and the AI's again) */
    if (!ok) { answer(1, 0); return; }  /* (a state whose tokens do not add up: the game is no longer one that can go on, but it is the Pocket's to say) */
    answer(0, 0);
    n64rt_state_resume((const void *)(uintptr_t)SS_SCR_RT, _fdata, ((u32)(_end - _fdata) + 15u) / 16u * 4u, w);
}

/* The runtime is the state's from here (n64rt_state_resume copied it), on its exception stack; the monitor's exit
 * follows. What the runtime keeps of the SoC's clock is moved on by the time between the state and now, and what it
 * computes from the hardware is computed again. */
static int s_in;                        /* a request is being answered (the state's copy of it says so too) */
void n64rt_state_resumed(const ss_hw_t *w);
void n64rt_state_resumed(const ss_hw_t *w)
{
    s_in = 0;
    n64rt_io_rebase((u32)(uptime() - w->uptime));
    n64rt_vi_hw_event();
    n64rt_ai_hw_event();
    if (w->fcc) __asm__ volatile("c.ueq.s $f0, $f0");   /* (the FPU's condition bit: $f0 is the game's again on the way out) */
    else __asm__ volatile("c.f.s $f0, $f0");
}

/* MONITOR.PEND bit 5 (n64rt_rcp_events): the Pocket asks */
void n64rt_state_event(void)
{
    if (s_in) return;
    s_in = 1;
    const u32 r = apf_bridge_savestate_read();
    if (r & 2u) save();
    else if (r & 4u) load();
    s_in = 0;
}
/* whether the Pocket may ask: not with a Game Boy cartridge in the Transfer Pak (its RAM and clock are the cartridge's
 * own, which no state can hold; the virtual one's neither, for now) */
void n64rt_state_poll(void)
{
    const u32 ok = n64rt_pak_kind(0) < N64RT_PAK_TRANSFER;
    if ((apf_bridge_savestate_read() & 1u) != ok) apf_bridge_savestate_write(ok);
    if (!(apf_bridge_transfer_length_read() >> 20 & 0xFu)) apf_bridge_transfer_length_write(LAYOUT_STATE_BYTES);   /* (the size told until a state says its own) */
}
