// The RSP's RTL (rtl/rsp) against the reference model (rsp.c), in lockstep,
// on real tasks (the dumps rsp_task runs): the task is loaded the way the
// N64 does it -- the header and the boot microcode by DMA through the host's
// registers -- and then, at every instruction that leaves the scalar unit's
// E stage, the model runs one too and the two must agree: the PC, DMEM, the
// vector registers, the accumulator, the flags, and the scalar register the
// instruction writes (when it reaches W). The vector unit finishes some
// instructions after they leave E: its state is compared as each one's
// effects are all in (tr_v), against the model's state after that instruction.
// At the end the RDRAM and the RDP's commands must be the same.
//
//   tb_rsp [-v] task.bin ...
//   RAM_LAT=<cycles> a read's latency (default 1), RAM_STALL=<percent> of cycles the RDRAM takes no request
//   KINDS=1 where the cycles go, by kind of instruction; PERF=<file> every instruction's cycles (perf_kinds.py)
#include "Vrsp_top.h"
#include "Vrsp_top___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include "rsp.h"

#define RDRAM_SIZE (8u << 20)
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static std::vector<uint32_t> model_rdp, rtl_rdp;

// where the cycles go: by the kind of the instruction that leaves E (the
// cycles since the one before left it)
enum { K_SCALAR, K_LOAD, K_BRANCH, K_VMUL, K_VOTHER, K_VRCP, K_VLOAD, K_VSTORE, K_C2MOVE, K_COP0, K_DMA, K_N };
static const char *kname[K_N] = { "scalar", "scalar load", "branch/jump", "vector multiply", "vector other", "vector reciprocal",
                                  "vector load", "vector store", "MFC2/MTC2/CFC2/CTC2", "MFC0/MTC0", "DMA (MTC0)" };
static double kcyc[K_N], kins[K_N];
static int kind_of(uint32_t in)
{
    unsigned op = in >> 26, fn = in & 63u;
    if (op == 0x32) return K_VLOAD;
    if (op == 0x3A) return K_VSTORE;
    if (op == 0x12) {
        if (!(in & (1u << 25))) return K_C2MOVE;
        if (fn < 0x10) return K_VMUL;
        if (fn >= 0x30 && fn <= 0x36 && fn != 0x33) return K_VRCP;
        return K_VOTHER;
    }
    if (op == 0x10) return ((in >> 21) & 31u) == 4 && (((in >> 11) & 15u) == 2 || ((in >> 11) & 15u) == 3) ? K_DMA : K_COP0;
    if (op >= 0x20 && op <= 0x27) return K_LOAD;
    if ((op >= 1 && op <= 7) || (op == 0 && (fn == 8 || fn == 9))) return K_BRANCH;
    return K_SCALAR;
}
static void on_rdp(void *, const uint32_t *w, int n) { for (int i = 0; i < 2 * n; i++) model_rdp.push_back(w[i]); }

struct Tb {
    Vrsp_top *t;
    std::vector<uint8_t> ram;
    long cyc = 0;
    int lat, stall = 0;
    uint32_t dstart = 0, dend = 0, dcur = 0, dxbus = 0;
    uint32_t dmem_word(uint32_t a) { return t->rootp->rsp_top__DOT__dmem__DOT__mem[(a >> 2) & 1023]; }
    std::deque<std::pair<long, uint32_t>> rdq;   // reads under way: (the cycle its word is back, the address)
    void tick() {
        // the RDRAM, as the SoC's SDRAM arbiter gives it: a request a cycle (now and
        // then not taken: RAM_STALL), a read's word back `lat` cycles later, in order
        t->r_ready = stall <= 0 || (rand() % 100) >= stall;
        t->r_rvalid = 0;
        if (!rdq.empty() && rdq.front().first <= cyc) {
            t->r_rvalid = 1; t->r_rdata = be32(&ram[rdq.front().second]);
            rdq.pop_front();
        }
        t->dpc_start = dstart; t->dpc_end = dend; t->dpc_current = dcur; t->dpc_status = dxbus | 0x80u;
        t->eval();
        uint32_t ra = ((uint32_t)t->r_addr << 2) & (RDRAM_SIZE - 1);
        bool go = t->r_valid && t->r_ready, wr = go && t->r_we;
        if (go && !t->r_we) rdq.push_back({ cyc + lat, ra });
        uint32_t wd = t->r_wdata;
        bool s_we = t->dpc_start_we, e_we = t->dpc_end_we, st_we = t->dpc_status_we;
        uint32_t dw = t->dpc_wdata;
        t->clk = 1; t->eval(); t->clk = 0; t->eval();
        cyc++;
        if (wr) wbe32(&ram[ra], wd);
        if (st_we) { if (dw & 1u) dxbus = 0; if (dw & 2u) dxbus = 1; }
        if (s_we) dstart = dcur = dw & 0xFFFFF8u;
        if (e_we) {
            dend = dw & 0xFFFFF8u;
            while (dcur != dend) {
                uint32_t hi, lo;
                if (dxbus) { hi = dmem_word(dcur); lo = dmem_word(dcur + 4); }
                else { hi = be32(&ram[dcur % RDRAM_SIZE]); lo = be32(&ram[(dcur + 4) % RDRAM_SIZE]); }
                rtl_rdp.push_back(hi); rtl_rdp.push_back(lo);
                dcur = (dcur + 8) & 0xFFFFF8u;
            }
            // DPCT=<file>: when the RDP's words were handed over (the cycle, the 64-bit words so far)
            static FILE *dt = getenv("DPCT") ? fopen(getenv("DPCT"), "w") : nullptr;
            if (dt) fprintf(dt, "%llu %zu\n", (unsigned long long)cyc, rtl_rdp.size() / 2);
        }
    }
    void hw(int reg, uint32_t v) {
        t->h_addr = reg; t->h_wdata = v; t->h_we = 1;
        tick();
        t->h_we = 0;
    }
    uint32_t hr(int reg) { t->h_addr = reg; t->eval(); return t->h_rdata; }
    void dma_in(uint32_t sp, uint32_t rdram, uint32_t bytes) {
        hw(0, sp); hw(1, rdram); hw(2, bytes - 1);
        for (int n = 0; hr(6) && n < 1000000; n++) tick();
    }
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int verbose = 0, ai = 1, bad_tasks = 0;
    if (ai < argc && !strcmp(argv[ai], "-v")) { verbose = 1; ai++; }
    int deep = getenv("DEEP") ? atoi(getenv("DEEP")) : 64;       // DMEM is compared every `deep` instructions
    FILE *perf = getenv("PERF") ? fopen(getenv("PERF"), "w") : nullptr;   // a line an instruction: its cycles, the instruction (perf_kinds.py)
    double tot_cyc = 0, tot_ins = 0;
    for (; ai < argc; ai++) {
        std::vector<uint8_t> mram(RDRAM_SIZE);
        uint8_t hdr[16];
        FILE *f = fopen(argv[ai], "rb");
        if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "RSPT", 4) || fread(mram.data(), 1, RDRAM_SIZE, f) != RDRAM_SIZE) {
            fprintf(stderr, "%s: not a task dump\n", argv[ai]); return 2;
        }
        fclose(f);
        // the task's header, pointers physical, where a DMA can take it from (the RDRAM's last 64 bytes)
        uint32_t ta = be32(hdr + 4) & (RDRAM_SIZE - 1u), scratch = RDRAM_SIZE - 64u;
        uint8_t task[64];
        memcpy(task, &mram[ta], 64);
        static const int ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };
        for (int p : ptrs) wbe32(task + 4 * p, be32(task + 4 * p) & 0x1FFFFFFFu);
        memcpy(&mram[scratch], task, 64);
        uint32_t boot = be32(task + 8) & (RDRAM_SIZE - 1u), boot_size = (be32(task + 12) + 7u) & ~7u;
        if (boot_size > 4096 || boot_size == 0) boot_size = 4096;

        Tb tb;
        tb.t = new Vrsp_top;
        tb.ram = mram;
        tb.lat = getenv("RAM_LAT") ? atoi(getenv("RAM_LAT")) : 1;
        tb.stall = getenv("RAM_STALL") ? atoi(getenv("RAM_STALL")) : 0;
        static rsp_t m;
        rsp_reset(&m, mram.data(), RDRAM_SIZE);
        m.rdp_cmd = on_rdp;
        model_rdp.clear(); rtl_rdp.clear();
        memcpy(m.dmem + 0xFC0, task, 64);
        memcpy(m.imem, &mram[boot], boot_size);
        m.pc = 0; m.status &= ~1u;

        Vrsp_top *t = tb.t;
        t->rst = 1; t->h_we = 0;
        for (int i = 0; i < 4; i++) tb.tick();
        t->rst = 0; tb.tick();
        tb.dma_in(0x0FC0, scratch, 64);
        tb.dma_in(0x1000, boot, boot_size);
        tb.hw(8, 0);
        tb.hw(4, 0x1 | 0x4);                          // clear halt, clear broke
        long c0 = tb.cyc, ins = 0, last_e = tb.cyc, pairs = 0;
        int bad = 0, ended = 0;
        std::deque<std::pair<int, uint32_t>> expect;  // register writes the RTL owes
        struct Vs { long ins; uint32_t pc, in; uint16_t vr[32][8], ah[8], am[8], al[8], vco, vcc; uint8_t vce; };
        std::deque<Vs> vexp;                          // vector-unit instructions the RTL has not finished
        int drain = 0, dmem_late = 0;
        auto &R = *t->rootp;
        for (long n = 0; n < 2000000000L && !bad; n++) {
            // the cycle's events are sampled before its edge
            t->eval();
            bool e = t->tr_e, e2 = t->tr_e2, we = t->tr_we; int vdone = t->tr_v;
            // the microcode's DMA runs behind it, as the console's: what it reads of SP_DMA_FULL / SP_DMA_BUSY is the RTL's,
            // and DMEM is compared only with no DMA under way or queued (the model's was done at its length's write)
            const bool dma_full = R.rsp_top__DOT__p_full, dma_on = dma_full || R.rsp_top__DOT__ds != 0;
            // (the vector unit, two passes: a computation whose register write
            // was put off behind a load's is complete while the next one's
            // first pass has already been through the accumulator and the
            // flags -- then the registers are compared, and the rest with the
            // instruction after)
            bool vpart = t->tr_vp;
            uint32_t epc = t->tr_e_pc, wr = t->tr_wr, wv = t->tr_val;
            bool e_we = t->tr_e_we; uint32_t e_wr = t->tr_e_wr;
            if (we) {
                if (expect.empty()) { printf("  !! a register write (r%u = %08x) the model did not make\n", wr, wv); bad = 1; }
                else {
                    auto x = expect.front(); expect.pop_front();
                    if (x.first != (int)wr || x.second != wv) {
                        printf("  !! r%u = %08x, the model: r%d = %08x (around instruction %ld)\n", wr, wv, x.first, x.second, ins);
                        bad = 1;
                    }
                }
            }
            tb.tick();
            if (dmem_late && !bad) {
                dmem_late = 0;
                for (uint32_t a = 0; a < 4096; a += 4)
                    if (tb.dmem_word(a) != be32(m.dmem + a)) {
                        printf("  !! after instruction %ld (pc %03x, %08x): DMEM[%03x] = %08x, the model %08x\n", ins, m.last_pc, m.last_ins, a,
                               tb.dmem_word(a), be32(m.dmem + a));
                        bad = 1; break;
                    }
            }
            if (e) {
                m.dma_full = dma_full; m.dma_busy = dma_on;
                const uint64_t dmas = m.dmas;
                const long ins0 = ins;
                int brk = 0;
                // (a pair -- a vector computation and a scalar instruction leaving E together: the model runs both, in order;
                // the cycles are the first's)
                for (int k2 = 0; k2 < (e2 ? 2 : 1); k2++) {
                    brk |= rsp_step(&m);
                    ins++;
                    {
                        int k = kind_of(m.last_ins); kcyc[k] += tb.cyc - last_e; kins[k]++;
                        if (perf) fprintf(perf, "%ld %08x\n", tb.cyc - last_e, m.last_ins);
                        last_e = tb.cyc;
                    }
                    const uint32_t pc_k = (epc + 4u * k2) & 0xFFFu;
                    if (m.last_pc != pc_k) { printf("  !! instruction %ld: pc %03x, the model %03x\n", ins, pc_k, m.last_pc); bad = 1; }
                    unsigned op = m.last_ins >> 26;
                    if (op == 0x12 || op == 0x32 || op == 0x3A) {
                        Vs v; v.ins = ins; v.pc = m.last_pc; v.in = m.last_ins;
                        memcpy(v.vr, m.vr, sizeof v.vr); memcpy(v.ah, m.acc_h, 16); memcpy(v.am, m.acc_m, 16); memcpy(v.al, m.acc_l, 16);
                        v.vco = m.vco; v.vcc = m.vcc; v.vce = m.vce;
                        vexp.push_back(v);
                    }
                }
                if (e2) pairs++;
                const bool dma_now = dma_on || m.dmas != dmas;          // (or this instruction starts one)
                const bool deep_now = ins0 / deep != ins / deep;
                if (e_we) expect.push_back({ (int)e_wr, m.r[e_wr] });
                // (a store's second half may be going behind the next instruction,
                // one that leaves DMEM alone: DMEM is compared a cycle on)
                if (!bad && !brk && deep_now && R.rsp_top__DOT__vu__DOT__st == 4) dmem_late = 1;
                else if (!bad && !dma_now && (brk || deep_now))
                    for (uint32_t a = 0; a < 4096; a += 4)
                        if (tb.dmem_word(a) != be32(m.dmem + a)) {
                            printf("  !! instruction %ld (pc %03x, %08x): DMEM[%03x] = %08x, the model %08x\n", ins, m.last_pc, m.last_ins, a,
                                   tb.dmem_word(a), be32(m.dmem + a));
                            bad = 1; break;
                        }
                if (verbose && ins <= 200) printf("  %6ld  pc %03x  %08x\n", ins, m.last_pc, m.last_ins);
                if (brk) ended = 1;
            }
            for (; vdone > 1 && !vexp.empty(); vdone--) vexp.pop_front();   // (two finished: the state is the later one's)
            if (vdone && !bad) {
                // the vector unit's state, against the model's after the instruction that just finished
                if (vexp.empty()) { printf("  !! the vector unit finished an instruction the model did not run (around %ld)\n", ins); bad = 1; }
                else {
                    Vs v = vexp.front(); vexp.pop_front();
                    for (int r = 0; r < 32 && !bad; r++)
                        for (int l = 0; l < 8; l++)
                            if (R.rsp_top__DOT__vu__DOT__shadow[r][l] != v.vr[r][l]) {
                                printf("  !! instruction %ld (pc %03x, %08x): v%d[%d] = %04x, the model %04x\n", v.ins, v.pc, v.in, r, l,
                                       R.rsp_top__DOT__vu__DOT__shadow[r][l], v.vr[r][l]);
                                bad = 1; break;
                            }
                    for (int l = 0; l < 8 && !bad && !vpart; l++)
                        if (R.rsp_top__DOT__vu__DOT__acc_h[l] != v.ah[l] || R.rsp_top__DOT__vu__DOT__acc_m[l] != v.am[l] || R.rsp_top__DOT__vu__DOT__acc_l[l] != v.al[l]) {
                            printf("  !! instruction %ld (pc %03x, %08x): acc[%d] = %04x %04x %04x, the model %04x %04x %04x\n", v.ins, v.pc, v.in, l,
                                   R.rsp_top__DOT__vu__DOT__acc_h[l], R.rsp_top__DOT__vu__DOT__acc_m[l], R.rsp_top__DOT__vu__DOT__acc_l[l], v.ah[l], v.am[l], v.al[l]);
                            bad = 1;
                        }
                    if (!bad && !vpart && (R.rsp_top__DOT__vu__DOT__vco != v.vco || R.rsp_top__DOT__vu__DOT__vcc != v.vcc || R.rsp_top__DOT__vu__DOT__vce != v.vce)) {
                        printf("  !! instruction %ld (pc %03x, %08x): vco %04x vcc %04x vce %02x, the model %04x %04x %02x\n", v.ins, v.pc, v.in,
                               R.rsp_top__DOT__vu__DOT__vco, R.rsp_top__DOT__vu__DOT__vcc, R.rsp_top__DOT__vu__DOT__vce, v.vco, v.vcc, v.vce);
                        bad = 1;
                    }
                }
            }
            // (after the BREAK the vector unit may still have an instruction's
            // execute to run: its state is compared like any other's)
            if (ended && (vexp.empty() || bad || ++drain > 8)) break;
            if (!ended && t->halted && !e) { printf("  !! the RTL halted at instruction %ld, the model has not\n", ins); bad = 1; }
        }
        long cycles = tb.cyc - c0;
        for (int i = 0; i < 8 && !t->halted; i++) tb.tick();
        // (a DMA the microcode left running at its BREAK: to its end; DMEM compared then)
        if (ended && !bad) {
            for (int n = 0; (R.rsp_top__DOT__p_full || R.rsp_top__DOT__ds != 0) && n < 100000; n++) tb.tick();
            for (uint32_t a = 0; a < 4096; a += 4)
                if (tb.dmem_word(a) != be32(m.dmem + a)) {
                    printf("  !! at the end: DMEM[%03x] = %08x, the model %08x\n", a, tb.dmem_word(a), be32(m.dmem + a));
                    bad = 1; break;
                }
        }
        if (ended && !bad && !vexp.empty()) { printf("  !! %zu vector-unit instructions never finished\n", vexp.size()); bad = 1; }
        if (ended && !bad) {
            if (!t->halted) { printf("  !! the RTL did not halt at the BREAK\n"); bad = 1; }
            if (memcmp(tb.ram.data(), mram.data(), RDRAM_SIZE)) {
                uint32_t a = 0; while (tb.ram[a] == mram[a]) a++;
                printf("  !! RDRAM differs at %06x: %02x, the model %02x\n", a, tb.ram[a], mram[a]); bad = 1;
            }
            if (rtl_rdp != model_rdp) { printf("  !! the RDP's commands differ (%zu words, the model %zu)\n", rtl_rdp.size(), model_rdp.size()); bad = 1; }
            if (t->unimpl) printf("  (an instruction the vector unit does not implement ran)\n");
        }
        printf("%s: %s -- %ld instructions (%ld pairs), %ld cycles (%.2f a instruction, %.2f ms at 62.8 MHz), %zu RDP words\n", argv[ai],
               bad ? "MISMATCH" : ended ? "identical" : "did not end", ins, pairs, cycles, ins ? (double)cycles / ins : 0.0, cycles / 62832.0, rtl_rdp.size());
        tot_cyc += cycles; tot_ins += ins;
        bad_tasks += bad || !ended;
        t->final(); delete t;
    }
    if (getenv("KINDS")) {
        double tc = 0; for (int k = 0; k < K_N; k++) tc += kcyc[k];
        for (int k = 0; k < K_N; k++)
            if (kins[k]) printf("  %-22s %9.0f instructions (%4.1f %%), %5.2f cycles each, %4.1f %% of the cycles\n", kname[k], kins[k],
                                100.0 * kins[k] / tot_ins, kcyc[k] / kins[k], 100.0 * kcyc[k] / tc);
    }
    if (tot_ins) printf("total: %.0f instructions, %.0f cycles (%.2f a instruction)%s\n", tot_ins, tot_cyc, tot_cyc / tot_ins, bad_tasks ? " -- MISMATCHES" : "");
    return bad_tasks ? 1 : 0;
}
