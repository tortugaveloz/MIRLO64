// rtl/audio/AudioDspCfu.v against its C model (lang/c/audio/audio_cfu.h),
// op by op: random but in-range sequences shaped like an audio interpreter's use (configure,
// load the 16 lane volumes, a run of ENV_MIX2 in cursor order, read them
// back; resampler runs; ADPCM codebook loads and frames), every result
// compared.
//   ./obj_cfu/tb_cfu [iterations] [seed]
#include "VAudioDspCfu.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define ACFU_MODEL 1
extern "C" {
#include "../../lang/c/audio/audio_cfu.h"
}

static VAudioDspCfu *dut;
static uint64_t cyc;
static void tick() { dut->clk = 0; dut->eval(); dut->clk = 1; dut->eval(); cyc++; }

static uint32_t rtl(uint32_t id, uint32_t a, uint32_t b)
{
    dut->cmd_function_id = id; dut->cmd_inputs_0 = a; dut->cmd_inputs_1 = b; dut->cmd_valid = 1;
    dut->rsp_ready = 1;
    while (!dut->cmd_ready) tick();
    tick();                                  // accepted on this edge
    dut->cmd_valid = 0;
    int n = 0;
    while (!dut->rsp_valid) { tick(); if (++n > 50) { printf("no response to op %x\n", id); exit(2); } }
    uint32_t r = dut->rsp_outputs_0;
    tick();                                  // consumed
    return r;
}

static uint32_t rng = 1;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static long fails;
static uint32_t both(uint32_t id, uint32_t a, uint32_t b, const char *what)
{
    uint32_t m = acfu_model(id, a, b), r = rtl(id, a, b);
    if (m != r && fails++ < 10) printf("MISMATCH %s op %x a=%08x b=%08x: model %08x rtl %08x\n", what, id, a, b, m, r);
    return r;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 2000;
    rng = argc > 2 ? atol(argv[2]) : 7;
    dut = new VAudioDspCfu;
    dut->rst = 1; for (int i = 0; i < 4; i++) tick(); dut->rst = 0; tick();
    long ops = 0;
    for (long it = 0; it < iters && fails < 10; it++) {
        uint32_t kind = rnd() % 3;
        if (kind == 2) {                     // ADPCM: a codebook, then frames
            if (rnd() & 1) {
                uint32_t w0 = rnd() & 31, nw = rnd() % 32 + 1;
                both(ACFU_ADP_TRST, w0, 0, "trst");
                for (uint32_t w = 0; w < nw; w++) {
                    uint32_t a = rnd(), b = rnd();
                    if (rnd() & 1) { a &= 0x0FFF0FFF; b &= 0x0FFF0FFF; }   // realistic magnitudes too
                    both(ACFU_ADP_TLOAD, a, b, "tload");
                }
                ops += nw + 1;
            }
            both(ACFU_ADP_START, rnd(), 0, "start");
            int nf = rnd() % 8 + 1;
            for (int f = 0; f < nf * 2; f++) {
                uint32_t hdr = (rnd() & 0xF0) | (rnd() & 7);
                both(ACFU_ADP_GRP, hdr, rnd(), "grp");
                for (int k = 0; k < 3; k++) both(ACFU_ADP_NXT, 0, 0, "nxt");
            }
            ops += 1 + nf * 8;
        } else if (kind == 1) {              // an envelope mix
            int aux = rnd() & 1;
            both(ACFU_ENV_CFG_B, rnd() & 0xFFFF, rnd() & 0xFFFF, "cfgB");
            for (int c = 0; c < 2; c++) {
                uint32_t rate = (rnd() & 1) ? 0x10000 + (rnd() & 0xFFF) : 0xF000 + (rnd() & 0xFFF);
                if ((rnd() & 7) == 0) rate = rnd() & 0x1FFFF;
                both(ACFU_ENV_CFG_A, rate | ((uint32_t)c << 31), rnd() & 0x7FFF, "cfgA");
            }
            for (int l = 0; l < 16; l++) both(ACFU_ENV_VSET, l, rnd() & 0x7FFFFFFF, "vset");
            both(ACFU_ENV_START, aux, 0, "start");
            int nops = (int)(rnd() % 6 + 1) * 8 * (aux ? 2 : 1);
            for (int k = 0; k < nops; k++) both(ACFU_ENV_MIX2, rnd(), rnd(), "mix2");
            for (int l = 0; l < 16; l++) both(ACFU_ENV_VGET, l, 0, "vget");
            ops += 21 + nops + 16;
        } else {                             // a resampler run
            both(ACFU_RS_CFG, (rnd() & 0xFFFF) << 1, rnd() & 0xFFFF, "rscfg");
            int n = rnd() % 64 + 1;
            for (int k = 0; k < n; k++) both(ACFU_RS_OUT, rnd(), rnd(), "rsout");
            both(ACFU_RS_ACC, 0, 0, "rsacc");
            ops += n + 2;
        }
    }
    both(ACFU_ID, 0, 0, "id");
    printf("AudioDspCfu vs model: %ld ops, %ld mismatches -> %s\n", ops, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
