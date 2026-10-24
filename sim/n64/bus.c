/* The Mirlo-N64 bus: the N64's physical memory map and the RCP's register
 * blocks, as the gateware will implement them.
 *
 * The RSP is not an RSP: when the game starts a task (it clears SP_STATUS's
 * halt bit), the OSTask it placed in DMEM is handed to Mirlo's firmware
 * (n64_t.sp_task), which does what the task's microcode would; then the SP
 * halts with "broke" and the task-done signal, raising the interrupts the
 * microcode's `break` (and a graphics task's final full sync) would.
 */
#include <stdlib.h>
#include <string.h>

#include "n64.h"

/* ---- interrupts -------------------------------------------------------- */
void n64_raise(n64_t *n, uint32_t bits) { n->rcp.mi_intr |= bits; }
void n64_clear(n64_t *n, uint32_t bits) { n->rcp.mi_intr &= ~bits; }

/* virtual -> physical: KSEG0/KSEG1 directly, KUSEG/KSEG2 through the TLB */
static int phys(n64_t *n, uint32_t va, uint32_t *pa, int write)
{
    if (va - 0x80000000u < 0x40000000u) { *pa = va & 0x1FFFFFFFu; return 0; }
    const n64_cpu_t *c = &n->cpu;
    uint32_t asid = c->cop0[C0_ENTRYHI] & 0xFFu;
    for (int i = 0; i < N64_TLB_ENTRIES; i++) {
        const n64_tlb_t *e = &c->tlb[i];
        uint32_t span = (e->mask | 0x1FFFu), vpn = ~span;       /* the page pair */
        if ((va & vpn) != (e->hi & vpn)) continue;
        if (!((e->lo0 & e->lo1) & 1u) && (e->hi & 0xFFu) != asid) continue;   /* not global: ASID */
        uint32_t page = (span + 1u) >> 1;                       /* one page of the pair */
        uint32_t lo = (va & page) ? e->lo1 : e->lo0;
        if (!(lo & 2u)) return N64_TLB_INVALID;
        if (write && !(lo & 4u)) return N64_TLB_MOD;
        *pa = (((lo >> 6) << 12) & ~(page - 1u)) | (va & (page - 1u));
        { extern uint64_t n64_tlb_hits; extern int n64_tlb_maxi; n64_tlb_hits++; if (i > n64_tlb_maxi) n64_tlb_maxi = i; }
        return 0;
    }
    return N64_TLB_MISS;
}

uint8_t *n64_phys(n64_t *n, uint32_t va, uint32_t len)
{
    uint32_t pa = va & 0x1FFFFFFFu;
    if (pa + len > RDRAM_SIZE) return NULL;
    return n->mem.rdram + pa;
}

/* ---- SP (the "RSP") ---------------------------------------------------- */
enum {
    SP_HALT = 1 << 0, SP_BROKE = 1 << 1, SP_DMA_BUSY = 1 << 2, SP_DMA_FULL = 1 << 3,
    SP_IO_FULL = 1 << 4, SP_SSTEP = 1 << 5, SP_INTR_BREAK = 1 << 6,
    SP_SIG0 = 1 << 7,   /* yield */
    SP_SIG1 = 1 << 8,   /* yielded */
    SP_SIG2 = 1 << 9,   /* task done */
};

static void sp_dma(n64_t *n, uint32_t lenreg, int to_rdram)
{
    uint32_t len = (lenreg & 0xFFF) + 1, count = ((lenreg >> 12) & 0xFF) + 1, skip = lenreg >> 20;
    uint32_t mem = n->rcp.sp_mem_addr & 0x1FF8, dram = n->rcp.sp_dram_addr & 0xFFFFF8;
    len = (len + 7) & ~7u;
    for (uint32_t c = 0; c < count; c++) {
        for (uint32_t i = 0; i < len; i++) {
            uint32_t m = (mem & 0x1000) | ((mem + i) & 0xFFF);
            if (to_rdram) { if (dram + i < RDRAM_SIZE) wr8(n->mem.rdram, dram + i, rd8(n->mem.spmem, m)); }
            else wr8(n->mem.spmem, m, dram + i < RDRAM_SIZE ? rd8(n->mem.rdram, dram + i) : 0);
        }
        mem += len;
        dram += len + skip;
    }
    n->rcp.sp_mem_addr = mem;
    n->rcp.sp_dram_addr = dram;
}

static void sp_run(n64_t *n)
{
    if (n->sp_task) n->sp_task(n, 0xFC0);          /* the OSTask, where osSpTaskLoad put it */
    n->rcp.sp_status |= SP_HALT | SP_BROKE | SP_SIG2;
    if (n->rcp.sp_status & SP_INTR_BREAK) n64_raise(n, MI_INTR_SP);
}

static void sp_status_write(n64_t *n, uint32_t v)
{
    uint32_t *s = &n->rcp.sp_status;
    int start = (v & 1) && (*s & SP_HALT);
    if (v & (1 << 0)) *s &= ~SP_HALT;
    if (v & (1 << 1)) *s |= SP_HALT;
    if (v & (1 << 2)) *s &= ~SP_BROKE;
    if (v & (1 << 3)) n64_clear(n, MI_INTR_SP);
    if (v & (1 << 4)) n64_raise(n, MI_INTR_SP);
    if (v & (1 << 5)) *s &= ~SP_SSTEP;
    if (v & (1 << 6)) *s |= SP_SSTEP;
    if (v & (1 << 7)) *s &= ~SP_INTR_BREAK;
    if (v & (1 << 8)) *s |= SP_INTR_BREAK;
    for (int i = 0; i < 8; i++) {                  /* signals 0..7: clear at bit 9+2i, set at 10+2i */
        if (v & (1u << (9 + 2 * i))) *s &= ~(SP_SIG0 << i);
        if (v & (1u << (10 + 2 * i))) *s |= (SP_SIG0 << i);
    }
    if (start) sp_run(n);
}

/* ---- AI ---------------------------------------------------------------- */
static uint64_t ai_cycles(n64_t *n, uint32_t len)
{
    uint32_t rate = n->rcp.ai_dacrate + 1;
    /* samples = len / 4 at 48.681812 MHz / rate; the CPU runs at 93.75 MHz */
    return (uint64_t)(len / 4) * rate * 93750000ull / 48681812ull;
}

/* ---- PI ---------------------------------------------------------------- */
static void pi_dma_to_rdram(n64_t *n, uint32_t len)
{
    uint32_t dram = n->rcp.pi_dram_addr & 0xFFFFFE, cart = n->rcp.pi_cart_addr;
    len = len + 1;
    if (getenv("N64_PI_LOG")) {                         /* the DMAs a game makes: alignment, size */
        static uint64_t k, mis;
        k++; if ((dram | cart | len) & 7) mis++;
        if ((dram | cart | len) & 7 || k % 500 == 0)
            fprintf(stderr, "PI DMA #%llu (%llu not 8-aligned): dram %06x cart %08x len %u\n",
                    (unsigned long long)k, (unsigned long long)mis, dram, cart, len);
    }
    if (cart >= 0x10000000u && cart < 0x1FC00000u) {
        uint32_t off = cart - 0x10000000u;
        for (uint32_t i = 0; i < len; i++) {
            if (dram + i >= RDRAM_SIZE) break;          /* (as the RCP block: n64_rcp.sv dst_ok) */
            wr8(n->mem.rdram, dram + i, off + i < n->mem.rom_size ? rd8(n->mem.rom, off + i) : 0);
        }
    }
    n->rcp.pi_dram_addr = dram + len;
    n->rcp.pi_cart_addr = cart + len;
    n64_raise(n, MI_INTR_PI);
}

/* ---- register reads/writes --------------------------------------------- */
static uint32_t reg_read(n64_t *n, uint32_t pa)
{
    n64_rcp_t *r = &n->rcp;
    switch (pa) {
    /* SP */
    case 0x04040000: return r->sp_mem_addr;
    case 0x04040004: return r->sp_dram_addr;
    case 0x04040008: case 0x0404000C: return 0xFF8;
    case 0x04040010: return r->sp_status;
    case 0x04040014: case 0x04040018: return 0;
    case 0x0404001C: { uint32_t s = r->sp_semaphore; r->sp_semaphore = 1; return s; }
    case 0x04080000: return r->sp_pc;
    /* DP */
    case 0x04100000: return r->dp_start;
    case 0x04100004: return r->dp_end;
    case 0x04100008: return r->dp_current;
    case 0x0410000C: return r->dp_status;
    case 0x04100010: case 0x04100014: case 0x04100018: case 0x0410001C: return 0;
    /* MI */
    case 0x04300000: return r->mi_mode;
    case 0x04300004: return 0x02020102;
    case 0x04300008: return r->mi_intr;
    case 0x0430000C: return r->mi_mask;
    /* VI */
    case 0x04400000: return r->vi_control;
    case 0x04400004: return r->vi_origin;
    case 0x04400008: return r->vi_width;
    case 0x0440000C: return r->vi_v_intr;
    case 0x04400010: return r->vi_v_current;
    case 0x04400014: return r->vi_burst;
    case 0x04400018: return r->vi_v_sync;
    case 0x0440001C: return r->vi_h_sync;
    case 0x04400020: return r->vi_leap;
    case 0x04400024: return r->vi_h_start;
    case 0x04400028: return r->vi_v_start;
    case 0x0440002C: return r->vi_v_burst;
    case 0x04400030: return r->vi_x_scale;
    case 0x04400034: return r->vi_y_scale;
    /* AI */
    case 0x04500004:                                  /* the playing buffer's bytes left */
        if (!r->ai_count) return 0;
        return (uint32_t)(r->ai_left * r->ai_len[0] / (ai_cycles(n, r->ai_len[0]) + 1)) & ~7u;
    case 0x0450000C:
        return (r->ai_count >= 2 ? 0x80000001u : 0) | (r->ai_count ? 0x40000000u : 0) | 0x01100000u;
    /* PI */
    case 0x04600000: return r->pi_dram_addr;
    case 0x04600004: return r->pi_cart_addr;
    case 0x04600010: return r->pi_status;
    /* SI */
    case 0x04800000: return r->si_dram_addr;
    case 0x04800018: return r->si_status;
    }
    if (pa >= 0x04600014 && pa <= 0x04600030) return r->pi_dom[(pa - 0x04600014) / 4];
    if (pa >= 0x04700000 && pa < 0x04700020) return r->ri[(pa - 0x04700000) / 4];
    return 0;
}

static void reg_write(n64_t *n, uint32_t pa, uint32_t v)
{
    n64_rcp_t *r = &n->rcp;
    switch (pa) {
    /* SP */
    case 0x04040000: r->sp_mem_addr = v & 0x1FF8; return;
    case 0x04040004: r->sp_dram_addr = v & 0xFFFFF8; return;
    case 0x04040008: sp_dma(n, v, 0); return;
    case 0x0404000C: sp_dma(n, v, 1); return;
    case 0x04040010: sp_status_write(n, v); return;
    case 0x0404001C: r->sp_semaphore = 0; return;
    case 0x04080000: r->sp_pc = v & 0xFFC; return;
    /* DP: the firmware owns the RDP; a CPU-fed command list is not supported */
    case 0x04100000: r->dp_start = r->dp_current = v & 0xFFFFF8; return;
    case 0x04100004: r->dp_end = v & 0xFFFFF8; return;
    case 0x0410000C: return;
    /* MI */
    case 0x04300000:
        r->mi_mode = (r->mi_mode & ~0x7Fu) | (v & 0x7F);
        if (v & (1 << 7)) r->mi_mode &= ~(1u << 7);
        if (v & (1 << 8)) r->mi_mode |= 1u << 7;
        if (v & (1 << 11)) n64_clear(n, MI_INTR_DP);
        return;
    case 0x0430000C:
        for (int i = 0; i < 6; i++) {
            if (v & (1u << (2 * i))) r->mi_mask &= ~(1u << i);
            if (v & (1u << (2 * i + 1))) r->mi_mask |= 1u << i;
        }
        return;
    /* VI */
    case 0x04400000: r->vi_control = v; return;
    case 0x04400004: r->vi_origin = v & 0xFFFFFF; return;
    case 0x04400008: r->vi_width = v & 0xFFF; return;
    case 0x0440000C: r->vi_v_intr = v & 0x3FF; return;
    case 0x04400010: n64_clear(n, MI_INTR_VI); return;
    case 0x04400014: r->vi_burst = v; return;
    case 0x04400018: r->vi_v_sync = v & 0x3FF; return;
    case 0x0440001C: r->vi_h_sync = v; return;
    case 0x04400020: r->vi_leap = v; return;
    case 0x04400024: r->vi_h_start = v; return;
    case 0x04400028: r->vi_v_start = v; return;
    case 0x0440002C: r->vi_v_burst = v; return;
    case 0x04400030: r->vi_x_scale = v; return;
    case 0x04400034: r->vi_y_scale = v; return;
    /* AI */
    case 0x04500000: r->ai_dram_addr[r->ai_count < 2 ? r->ai_count : 1] = v & 0xFFFFF8; return;
    case 0x04500004: {
        uint32_t len = v & 0x3FFF8;
        if (r->ai_count < 2 && len) {
            r->ai_len[r->ai_count] = len;
            if (r->ai_count == 0) {
                r->ai_left = ai_cycles(n, len);
                if (n->ai_start) n->ai_start(n, r->ai_dram_addr[0], len);
            }
            r->ai_count++;
        }
        return;
    }
    case 0x04500008: r->ai_control = v & 1; return;
    case 0x0450000C: n64_clear(n, MI_INTR_AI); return;
    case 0x04500010: r->ai_dacrate = v & 0x3FFF; return;
    case 0x04500014: r->ai_bitrate = v & 0xF; return;
    /* PI */
    case 0x04600000: r->pi_dram_addr = v & 0xFFFFFF; return;
    case 0x04600004: r->pi_cart_addr = v; return;
    case 0x04600008: n64_raise(n, MI_INTR_PI); return;       /* RDRAM -> cart: no writable cart memory */
    case 0x0460000C: pi_dma_to_rdram(n, v & 0xFFFFFF); return;
    case 0x04600010: if (v & 2) n64_clear(n, MI_INTR_PI); return;
    /* SI */
    case 0x04800000: r->si_dram_addr = v & 0xFFFFF8; return;
    case 0x04800004:                                           /* PIF RAM -> RDRAM: the PIF runs the */
        n64_pif_process(n);                                    /* commands it holds again (polling)  */
        for (uint32_t i = 0; i < 64; i += 4) wr32(n->mem.rdram, (r->si_dram_addr + i) & (RDRAM_SIZE - 1), rd32(n->mem.pif_ram, i));
        r->si_status |= 1u << 12;
        n64_raise(n, MI_INTR_SI);
        return;
    case 0x04800010:                                           /* RDRAM -> PIF RAM, then the PIF runs */
        for (uint32_t i = 0; i < 64; i += 4) wr32(n->mem.pif_ram, i, rd32(n->mem.rdram, (r->si_dram_addr + i) & (RDRAM_SIZE - 1)));
        n64_pif_process(n);
        r->si_status |= 1u << 12;
        n64_raise(n, MI_INTR_SI);
        return;
    case 0x04800018: r->si_status &= ~(1u << 12); n64_clear(n, MI_INTR_SI); return;
    }
    if (pa >= 0x04600014 && pa <= 0x04600030) { r->pi_dom[(pa - 0x04600014) / 4] = v; return; }
    if (pa >= 0x04700000 && pa < 0x04700020) { r->ri[(pa - 0x04700000) / 4] = v; return; }
}

/* ---- Mirlo's system window (rtl/n64/soc/mirlo_n64.sv, MSYS) -------------
 *   +0    the ROM's size
 *   +0x20 the RDRAM's written-pages map: w 1 flip, 2 clear; r busy (0)
 *   +0x24 a word of it (0-63)   +0x28 its 32 pages' bits
 *         (here: the pages whose contents changed since the last flip;
 *         the hardware marks every write, a superset)
 *   +0x2C the CPU's last translation time (lang/n64/gfx), kept
 *   +0x30 1: the CPU's translations are taken (N64_CPUGFX=0: 0) */
static uint32_t s_dirty[64], s_dirty_addr, s_gfxtime;
static uint8_t *s_dirty_prev;
static void msys_dirty_flip(n64_t *n)
{
    if (!s_dirty_prev) s_dirty_prev = calloc(RDRAM_SIZE, 1);
    memset(s_dirty, 0, sizeof s_dirty);
    for (uint32_t pg = 0; pg < 2048u; pg++)
        if (memcmp(s_dirty_prev + pg * 4096u, n->mem.rdram + pg * 4096u, 4096u)) {
            s_dirty[pg >> 5] |= 1u << (pg & 31u);
            memcpy(s_dirty_prev + pg * 4096u, n->mem.rdram + pg * 4096u, 4096u);
        }
}
static int msys_cpugfx(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("N64_CPUGFX"); v = e ? atoi(e) : 1; }   /* (3: no display-list cache) */
    return v;
}
static uint32_t msys_read(n64_t *n, uint32_t o)
{
    switch (o) {
    case 0x00: return n->mem.rom_size;
    case 0x28: return s_dirty[s_dirty_addr & 63u];
    case 0x2C: return s_gfxtime;
    case 0x30: return msys_cpugfx();
    default:   return 0;
    }
}
static void msys_write(n64_t *n, uint32_t o, uint32_t v)
{
    if (o == 0x20 && (v & 1u)) msys_dirty_flip(n);
    else if (o == 0x24) s_dirty_addr = v;
    else if (o == 0x2C) s_gfxtime = v;
}
uint32_t n64_msys_gfxtime(void) { return s_gfxtime; }

/* ---- the bus ----------------------------------------------------------- */
int n64_read(n64_t *n, uint32_t va, int size, uint32_t *val)
{
    uint32_t pa;
    int t = phys(n, va, &pa, 0);
    if (t) return t;
    return n64_read_phys(n, pa, size, val);
}

int n64_read_phys(n64_t *n, uint32_t pa, int size, uint32_t *val)
{
    const uint8_t *m = NULL;
    uint32_t off = 0;
    if (pa < RDRAM_SIZE || (pa < n->mem.sdram_size && pa < 0x03F00000)) { m = n->mem.rdram; off = pa; }
    else if (pa >= 0x04000000 && pa < 0x04040000) { m = n->mem.spmem; off = pa & 0x1FFF; }
    else if (pa >= 0x10000000 && pa < 0x1FC00000) {
        off = pa - 0x10000000;
        if (off >= n->mem.rom_size) { *val = 0; return 0; }
        m = n->mem.rom;
    }
    else if (pa >= 0x1FC007C0 && pa < 0x1FC00800) { m = n->mem.pif_ram; off = pa - 0x1FC007C0; }
    if (m) {
        *val = size == 4 ? rd32(m, off) : size == 2 ? rd16(m, off) : rd8(m, off);
        return 0;
    }
    if (pa >= 0x03F00000 && pa < 0x03F00100) { *val = 0; return 0; }   /* RDRAM config registers */
    if (pa >= 0x04040000 && pa < 0x04900000) {
        uint32_t w = reg_read(n, pa & ~3u);
        *val = size == 4 ? w : size == 2 ? (w >> (16 - 8 * (pa & 2))) & 0xFFFF : (w >> (24 - 8 * (pa & 3))) & 0xFF;
        return 0;
    }
    if (pa >= 0x05000000 && pa < 0x10000000) { *val = 0; return 0; }   /* 64DD / SRAM: absent */
    if (pa >= 0x1FC00000 && pa < 0x1FC007C0) {                         /* PIF ROM: Mirlo's boot ROM */
        uint32_t o = pa - 0x1FC00000, w = n->mem.bootrom && o < n->mem.bootrom_size ? rd32(n->mem.bootrom, o) : 0;
        *val = size == 4 ? w : size == 2 ? (w >> (16 - 8 * (o & 2))) & 0xFFFF : (w >> (24 - 8 * (o & 3))) & 0xFF;
        return 0;
    }
    if (pa >= 0x1FD00000 && pa < 0x1FD00040) {                         /* the Mirlo system window */
        *val = msys_read(n, pa & 0x3Cu);
        return 0;
    }
    return -1;
}

int n64_write(n64_t *n, uint32_t va, int size, uint32_t v)
{
    uint32_t pa;
    int t = phys(n, va, &pa, 1);
    if (t) return t;
    return n64_write_phys(n, pa, size, v);
}

int n64_write_phys(n64_t *n, uint32_t pa, int size, uint32_t v)
{
    {   /* debugging: N64_WATCH=phys address logs every store to its word */
        static int init; static uint32_t watch = 0xFFFFFFFFu;
        if (!init) { const char *e = getenv("N64_WATCH"); if (e) watch = (uint32_t)strtoul(e, 0, 16); init = 1; }
        if ((pa & ~3u) == (watch & ~3u))
            fprintf(stderr, "watch: store%d %08x -> %08x at pc %08x (cycle %llu)\n", size * 8, v, pa, n->cpu.pc, (unsigned long long)n->cpu.cycles);
    }
    uint8_t *m = NULL;
    uint32_t off = 0;
    if (pa < RDRAM_SIZE || (pa < n->mem.sdram_size && pa < 0x03F00000)) { m = n->mem.rdram; off = pa; }
    else if (pa >= 0x04000000 && pa < 0x04040000) { m = n->mem.spmem; off = pa & 0x1FFF; }
    else if (pa >= 0x1FC007C0 && pa < 0x1FC00800) { m = n->mem.pif_ram; off = pa - 0x1FC007C0; }
    if (m) {
        if (size == 4) wr32(m, off, v); else if (size == 2) wr16(m, off, (uint16_t)v); else wr8(m, off, (uint8_t)v);
        return 0;
    }
    if (pa >= 0x03F00000 && pa < 0x03F00100) return 0;
    if (pa >= 0x04040000 && pa < 0x04900000) {
        if (size == 4) reg_write(n, pa, v);
        return 0;
    }
    if (pa >= 0x05000000 && pa < 0x1FC00000) return 0;              /* cart space: read-only here */
    if (pa >= 0x1FD00000 && pa < 0x1FD00040) { if (size == 4) msys_write(n, pa & 0x3Cu, v); return 0; }
    return -1;
}

/* ---- the RCP's clocks -------------------------------------------------- */
void n64_advance(n64_t *n, uint32_t cycles)
{
    n64_rcp_t *r = &n->rcp;
    /* VI: 525 half-lines (NTSC), a line (2 of them) every 93750000 / 60 / 262.5 CPU cycles */
    const uint32_t per_line = 5952;
    r->vi_line_frac += cycles;
    while (r->vi_line_frac >= per_line) {
        r->vi_line_frac -= per_line;
        uint32_t vs = r->vi_v_sync ? r->vi_v_sync : 525;
        r->vi_v_current = (r->vi_v_current + 2) % (vs + 1);
        if (r->vi_v_current == 0) n->vi_frames++;
        if (r->vi_v_intr && (r->vi_v_current & ~1u) == (r->vi_v_intr & ~1u)) n64_raise(n, MI_INTR_VI);
    }
    /* AI: the playing buffer drains; the next one starts with an interrupt */
    if (r->ai_count) {
        if (r->ai_left > cycles) r->ai_left -= cycles;
        else {
            r->ai_count--;
            r->ai_dram_addr[0] = r->ai_dram_addr[1];
            r->ai_len[0] = r->ai_len[1];
            r->ai_left = r->ai_count ? ai_cycles(n, r->ai_len[0]) : 0;
            if (r->ai_count && n->ai_start) n->ai_start(n, r->ai_dram_addr[0], r->ai_len[0]);
            n64_raise(n, MI_INTR_AI);
        }
    }
}
