/* The RSP reference model: the N64's Reality Signal Processor as its
 * documentation describes it (the scalar unit, COP0's DMA and SP/DP
 * registers, COP2's vector unit), in C. It is the golden model for MIRLO's
 * RSP RTL (rtl/rsp), and -- run on real tasks dumped from the sim
 * (sim/mips_soc RSPDUMP) -- the profiler that sizes it.
 *
 * Memory is the N64's: big-endian bytes. The RDP is not modelled: the
 * commands the RSP hands it (DPC_START/END) are passed to a callback as they
 * complete, and the RDP is never busy. */
#ifndef RSP_H
#define RSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {                                  /* profile buckets beyond the vector functs (0..63) */
    RSP_P_VEC = 0,                      /* +funct: COP2 computational ops */
    RSP_P_LWC2 = 64,                    /* +rd: LBV..LTV */
    RSP_P_SWC2 = 80,                    /* +rd: SBV..STV */
    RSP_P_MFC2 = 96, RSP_P_MTC2, RSP_P_CFC2, RSP_P_CTC2,
    RSP_P_MFC0, RSP_P_MTC0,
    RSP_P_LOAD, RSP_P_STORE, RSP_P_BRANCH, RSP_P_JUMP, RSP_P_ALU, RSP_P_SHIFT, RSP_P_OTHER,
    RSP_P_N
};

typedef struct rsp {
    uint8_t  dmem[4096], imem[4096];
    uint32_t r[32];
    uint32_t pc;                        /* IMEM byte address, 12 bits */
    int      delay;                     /* a branch's delay slot is next */
    uint32_t delay_target;
    uint16_t vr[32][8];                 /* lane 0 = the register's first (most significant) halfword */
    uint16_t acc_h[8], acc_m[8], acc_l[8];
    uint16_t vco, vcc;                  /* [7:0] carry / compare, [15:8] not-equal / clip */
    uint8_t  vce;
    int32_t  div_in, div_out;
    int      dph;                       /* a VRCPH/VRSQH came last: the next L op takes div_in */
    uint32_t status;                    /* SP_STATUS: [0] halt [1] broke [5] sstep [6] intr_break [7..14] sig0..7 */
    uint32_t sp_addr, ram_addr, rd_len, wr_len, semaphore;
    uint32_t sp_addr_w, ram_addr_w;     /* SP_MEM_ADDR / SP_DRAM_ADDR as last written (a DMA starts there) */
    uint32_t dma_full, dma_busy;        /* what SP_DMA_FULL and SP_DMA_BUSY read (and SP_STATUS's bits 3, 2): 0 here, where a
                                           DMA is done at its length's write; a bench gives the RTL's, whose DMA runs behind it */
    uint32_t dpc_start, dpc_end, dpc_current, dpc_status;
    uint8_t *rdram;
    uint32_t rdram_size;
    /* the RDP's side: each complete command (64-bit words, big-endian halves) */
    void   (*rdp_cmd)(void *user, const uint32_t *w, int nwords);
    void    *user;
    uint32_t cmdbuf[2 * 22];
    int      cmdn;
    /* the profile */
    uint64_t insts, cycles, dma_rd_bytes, dma_wr_bytes, dmas, rdp_cmds;
    uint64_t prof[RSP_P_N];
    uint64_t unimpl;                    /* ops this model does not know */
    uint32_t last_pc, last_ins;         /* the instruction rsp_step() ran */
    uint64_t ls_plain[2][16];           /* vector loads [0] / stores [1] with element 0 and a naturally aligned address */
    uint64_t vec_e[16];                 /* vector ops by element field (0-1 none, 2-3 quarters, 4-7 halves, 8-15 one lane) */
    uint64_t vec_after_vec;             /* vector ops (and vector loads/stores) that follow another: no scalar op to pair with */
} rsp_t;

void rsp_reset(rsp_t *s, uint8_t *rdram, uint32_t rdram_size);
/* runs until BREAK (returns 0) or max instructions (returns -1) */
int  rsp_run(rsp_t *s, uint64_t max_insts);
/* one instruction (last_pc, last_ins); returns 1 at BREAK */
int  rsp_step(rsp_t *s);
const char *rsp_prof_name(int i);

#ifdef __cplusplus
}
#endif

#endif
