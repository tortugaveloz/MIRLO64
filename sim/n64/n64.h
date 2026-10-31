/* The reference model of the game CPU that runs an unmodified N64 ROM -- a
 * MIPS CPU executing the ROM's code as it is -- with the N64's register
 * blocks around it. sim/mips runs the RTL's core (rtl/mips) in lockstep
 * with it.
 *
 * The model is the specification the RTL is checked against, so it keeps
 * to what the hardware will do:
 *
 *  - The CPU's integer registers are 64 bits, as the VR4300's: libultra
 *    copies pairs of words with one ld/sd and builds long long values in
 *    single registers, so an unmodified ROM needs them. The game's own code
 *    is 32-bit operations almost only (cpu.c counts the 64-bit ones); the
 *    hardware keeps a 32-bit fast path. The FPU keeps the FR=0 register
 *    pairs but computes doubles as floats.
 *
 *  - Memory is "word-native": a 32-bit word is stored as the host's native
 *    uint32_t, so the N64's big-endian bytes and halfwords are at address
 *    ^ 3 and ^ 2. The ROM is kept the same way (each 32-bit word of the .z64
 *    byte-swapped once, at load).
 */
#ifndef N64_H
#define N64_H

#include <stdint.h>
#include <stdio.h>

#define RDRAM_SIZE   (8u << 20)          /* 8 MiB (the game sees osMemSize) */
#define ROM_MAX      (64u << 20)

/* ---- memory (word-native) --------------------------------------------- */
typedef struct {
    uint8_t *rdram;                       /* RDRAM_SIZE bytes, word-native */
    uint32_t sdram_size;                  /* rdram's size: past RDRAM_SIZE, Mirlo's SDRAM (0: RDRAM_SIZE) */
    uint8_t *rom;                         /* word-native */
    uint32_t rom_size;
    uint8_t *bootrom;                     /* Mirlo's boot ROM at 0x1FC00000 (the PIF ROM's place), or NULL */
    uint32_t bootrom_size;
    uint8_t  spmem[0x2000];               /* SP DMEM (0x000) + IMEM (0x1000) */
    uint8_t  pif_ram[64];                 /* word-native */
    uint8_t  eeprom[2048];                /* 4 Kbit (512 B) or 16 Kbit */
    uint32_t eeprom_size;
} n64_mem_t;

static inline uint32_t rd32(const uint8_t *m, uint32_t a) { return *(const uint32_t *)(m + (a & ~3u)); }
static inline void     wr32(uint8_t *m, uint32_t a, uint32_t v) { *(uint32_t *)(m + (a & ~3u)) = v; }
static inline uint16_t rd16(const uint8_t *m, uint32_t a) { return *(const uint16_t *)(m + ((a & ~1u) ^ 2)); }
static inline void     wr16(uint8_t *m, uint32_t a, uint16_t v) { *(uint16_t *)(m + ((a & ~1u) ^ 2)) = v; }
static inline uint8_t  rd8(const uint8_t *m, uint32_t a)  { return m[a ^ 3]; }
static inline void     wr8(uint8_t *m, uint32_t a, uint8_t v) { m[a ^ 3] = v; }

/* ---- the RCP's register blocks ----------------------------------------- */
enum { MI_INTR_SP = 1, MI_INTR_SI = 2, MI_INTR_AI = 4, MI_INTR_VI = 8, MI_INTR_PI = 16, MI_INTR_DP = 32 };

typedef struct {
    /* MI */
    uint32_t mi_mode, mi_intr, mi_mask;
    /* SP */
    uint32_t sp_mem_addr, sp_dram_addr, sp_status, sp_pc, sp_semaphore;
    /* DP */
    uint32_t dp_start, dp_end, dp_current, dp_status;
    /* VI */
    uint32_t vi_control, vi_origin, vi_width, vi_v_intr, vi_v_current, vi_burst, vi_v_sync,
             vi_h_sync, vi_leap, vi_h_start, vi_v_start, vi_v_burst, vi_x_scale, vi_y_scale;
    /* AI */
    uint32_t ai_dram_addr[2], ai_len[2], ai_count, ai_control, ai_dacrate, ai_bitrate;
    uint64_t ai_left;                     /* CPU cycles until the playing buffer ends */
    /* PI */
    uint32_t pi_dram_addr, pi_cart_addr, pi_status, pi_dom[8];
    /* SI */
    uint32_t si_dram_addr, si_status;
    /* RI / RDRAM interface: read back what was written */
    uint32_t ri[8];
    uint32_t vi_line_frac;                /* CPU cycles into the current line */
} n64_rcp_t;

/* ---- the CPU ----------------------------------------------------------- */
/* The TLB: a game may map its data at virtual 0x04000000 in 64 KiB pages. The VR4300 has 32 entries; the hardware may
 * keep fewer (a write beyond them is dropped, counted in n64_tlb_dropped). */
#ifndef N64_TLB_ENTRIES
#define N64_TLB_ENTRIES 32
#endif
typedef struct { uint32_t mask, hi, lo0, lo1; } n64_tlb_t;   /* PageMask, EntryHi, EntryLo0/1 */
enum { N64_TLB_MISS = -2, N64_TLB_INVALID = -3, N64_TLB_MOD = -4 };

typedef struct {
    uint64_t r[32], hi, lo;
    uint32_t pc;
    uint32_t next_pc;                     /* the delay slot's successor */
    int      in_delay;                    /* executing a branch's delay slot */
    uint32_t f[32];                       /* FPU: 32 32-bit registers (FR=0 pairs) */
    uint32_t fcr31;
    uint32_t cop0[32];
    uint64_t cycles;                      /* instructions retired (1 CPU cycle each) */
    uint32_t count_frac;                  /* Count advances every 2 cycles */
    int      ll_bit;
    n64_tlb_t tlb[N64_TLB_ENTRIES];
} n64_cpu_t;

enum {
    C0_INDEX = 0, C0_RANDOM = 1, C0_ENTRYLO0 = 2, C0_ENTRYLO1 = 3, C0_CONTEXT = 4, C0_PAGEMASK = 5,
    C0_WIRED = 6, C0_BADVADDR = 8, C0_COUNT = 9, C0_ENTRYHI = 10, C0_COMPARE = 11, C0_STATUS = 12,
    C0_CAUSE = 13, C0_EPC = 14, C0_PRID = 15, C0_CONFIG = 16, C0_LLADDR = 17, C0_WATCHLO = 18,
    C0_WATCHHI = 19, C0_XCONTEXT = 20, C0_TAGLO = 28, C0_TAGHI = 29, C0_ERROREPC = 30,
};
enum { EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11,
       EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15 };

typedef struct n64 {
    n64_cpu_t cpu;
    n64_rcp_t rcp;
    n64_mem_t mem;
    uint32_t  buttons;                    /* controller 1: N64 button bits | stick x << 8 | stick y */
    int       trace;                      /* log each instruction */
    FILE     *log;
    uint64_t  vi_frames;                  /* VI interrupts so far */
    /* the "RSP": Mirlo's firmware carrying out a task (set by the harness) */
    void    (*sp_task)(struct n64 *, uint32_t task_dmem);
    void    (*ai_start)(struct n64 *, uint32_t dram_addr, uint32_t len);   /* a buffer starts playing */
    int       halted;                     /* a fatal fault stopped the model */
    int       irq_external;               /* co-simulation: take interrupts only in n64_cpu_interrupt() */
} n64_t;

/* cpu.c: executed 64-bit instructions, by kind */
struct n64_site { uint32_t pc, insn; uint64_t count; };
extern struct n64_site n64_sites[];
extern int n64_nsites;
void n64_op64_site(uint32_t pc, uint32_t insn);
enum { N64_OP64_SHIFT, N64_OP64_MULDIV, N64_OP64_ADD, N64_OP64_MEM, N64_OP64_KINDS };
extern uint64_t n64_op64_count[8];

extern uint64_t n64_tlb_dropped;

/* bus.c */
int      n64_read(n64_t *n, uint32_t vaddr, int size, uint32_t *val);   /* 0 = ok */
int      n64_write(n64_t *n, uint32_t vaddr, int size, uint32_t val);
/* the same by physical address (the RTL CPU translates itself) */
int      n64_read_phys(n64_t *n, uint32_t pa, int size, uint32_t *val);
int      n64_write_phys(n64_t *n, uint32_t pa, int size, uint32_t val);
void     n64_advance(n64_t *n, uint32_t cycles);   /* the RCP's clocks: VI, AI */
void     n64_raise(n64_t *n, uint32_t mi_bits);
void     n64_clear(n64_t *n, uint32_t mi_bits);
uint8_t *n64_phys(n64_t *n, uint32_t vaddr, uint32_t len);          /* RDRAM pointer, or NULL */
int      n64_xlat_check(n64_t *n, uint32_t vaddr, int write);
uint32_t n64_xlat_pa(n64_t *n, uint32_t vaddr);                       /* its physical address, 0 for a TLB fault */       /* the TLB fault an access would take (N64_TLB_*), or 0 */
/* cpu.c */
void     n64_cpu_reset(n64_t *n);
void     n64_cpu_step(n64_t *n);
/* COP1 but its branches: 1 = wrote *gpr_out, 0 = no GPR result, -1 = unknown */
int      n64_cop1_exec(n64_cpu_t *c, uint32_t insn, uint64_t rt, uint64_t *gpr_out);
int      n64_cpu_irq_pending(n64_t *n);   /* an enabled interrupt is pending before the next instruction */
void     n64_cpu_interrupt(n64_t *n);     /* take it now (co-simulation follows the RTL) */
/* boot.c */
int      n64_load_rom(n64_t *n, const char *path);
void     n64_boot(n64_t *n);                        /* the hardware reset + what the boot does (HLE) */
void     n64_reset(n64_t *n);                       /* the hardware reset: the CPU at 0xBFC00000 */
int      n64_load_bootrom(n64_t *n, const char *path);
/* pif.c */
void     n64_pif_process(n64_t *n);

#define N64_FATAL(n, ...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); (n)->halted = 1; } while (0)

#endif
