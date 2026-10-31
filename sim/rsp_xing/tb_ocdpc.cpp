// The real RSP across the clock crossing (rsp_oc_wrap.sv: rsp_clk, rsp_xing, rsp_top, rdp_dpc) against the RSP's model, as
// sim/rdp's tb_dpc does without it: a task -- a real one's dump, or gen_xbus_task.py's -- runs on both, and the command words
// the fetch hands on must be the model's, in order. clk_sys is 6 ticks a cycle and clk_c 4 (3:2, rising together every two
// cycles of clk_sys), the memory answers with random latency and stalls, and the RDP's words are taken with random back-pressure.
//   tb_ocdpc task.bin ...     TB_CREADY=<percent> (the RDP takes a word)
#include "Vrsp_oc_wrap.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
extern "C" {
#include "rsp.h"
}
#define RDRAM_SIZE (8u << 20)
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static std::vector<uint32_t> model_rdp, rtl_rdp;
static void on_rdp(void *, const uint32_t *w, int n) { for (int i = 0; i < 2 * n; i++) model_rdp.push_back(w[i]); }
static const int c_pct = getenv("TB_CREADY") ? atoi(getenv("TB_CREADY")) : 75;
struct Tb {
    Vrsp_oc_wrap *t;
    std::vector<uint8_t> ram;
    long cyc = 0, k = 0;
    std::deque<std::pair<long, uint32_t>> rq, mq;   // reads under way: the RSP's DMA, the fetch
    // one cycle of clk_sys (6 ticks; clk_c has 4)
    void tick() {
        for (int u = 0; u < 6; u++, k++) {
            if (u == 0) {
                // the sys-side inputs of the edge about to come, then what the design drives
                t->clk = 0; 
                t->p_ready = rand() % 100 >= 20; t->m_ready = rand() % 100 >= 30; t->c_ready = rand() % 100 < c_pct;
                t->p_rvalid = 0; t->m_rvalid = 0;
                if (!rq.empty() && rq.front().first <= cyc) { t->p_rvalid = 1; t->p_rdata = be32(&ram[rq.front().second]); rq.pop_front(); }
                if (!mq.empty() && mq.front().first <= cyc) { t->m_rvalid = 1; t->m_rdata = be32(&ram[mq.front().second]); mq.pop_front(); }
                t->eval();
                uint32_t ra = ((uint32_t)t->p_addr << 2) & (RDRAM_SIZE - 1), ma = ((uint32_t)t->m_addr << 2) & (RDRAM_SIZE - 1);
                bool rgo = t->p_valid && t->p_ready, wr = rgo && t->p_we, mgo = t->m_valid && t->m_ready;
                if (rgo && !t->p_we) rq.push_back({ cyc + 1 + rand() % 9, ra });
                if (mgo) mq.push_back({ cyc + 1 + rand() % 9, ma });
                if (rq.size() > 1 && rq.back().first < rq[rq.size() - 2].first) rq.back().first = rq[rq.size() - 2].first;
                if (mq.size() > 1 && mq.back().first < mq[mq.size() - 2].first) mq.back().first = mq[mq.size() - 2].first;
                uint32_t wd = t->p_wdata;
                bool cgo = t->c_valid && t->c_ready; uint32_t cd = t->c_data;
                done = t->s_h_done; hrd = t->s_h_rdata;
                if (wr) wbe32(&ram[ra], wd);
                if (cgo) rtl_rdp.push_back(cd);
            }
            t->clk = (k % 6) < 3; t->clk_c = (k % 4) < 2;
            t->eval();
        }
        cyc++;
    }
    bool done = false; uint32_t hrd = 0;
    // a host access: held until the crossing answers
    uint32_t acc(int reg, bool we, uint32_t v) {
        t->s_h_req = 1; t->s_h_we = we; t->s_h_addr = reg; t->s_h_wdata = v;
        for (int n = 0; n < 100000; n++) { tick(); if (done) break; }
        t->s_h_req = 0; tick();
        return hrd;
    }
    void hw(int reg, uint32_t v) { acc(reg, true, v); }
    uint32_t hr(int reg) { return acc(reg, false, 0); }
    void dma_in(uint32_t sp, uint32_t rdram, uint32_t bytes) {
        hw(0, sp); hw(1, rdram); hw(2, bytes - 1);
        for (int n = 0; hr(6) && n < 1000000; n++) tick();
    }
};

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    int bad = 0;
    for (int ai = 1; ai < argc; ai++) {
        std::vector<uint8_t> mram(RDRAM_SIZE);
        uint8_t hdr[16];
        FILE *f = fopen(argv[ai], "rb");
        if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "RSPT", 4) || fread(mram.data(), 1, RDRAM_SIZE, f) != RDRAM_SIZE) {
            fprintf(stderr, "%s: not a task dump\n", argv[ai]); return 2;
        }
        fclose(f);
        uint32_t ta = be32(hdr + 4) & (RDRAM_SIZE - 1u), scratch = RDRAM_SIZE - 64u;
        uint8_t task[64];
        memcpy(task, &mram[ta], 64);
        static const int ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };
        for (int p : ptrs) wbe32(task + 4 * p, be32(task + 4 * p) & 0x1FFFFFFFu);
        memcpy(&mram[scratch], task, 64);
        uint32_t boot = be32(task + 8) & (RDRAM_SIZE - 1u), boot_size = (be32(task + 12) + 7u) & ~7u;
        if (boot_size > 4096 || boot_size == 0) boot_size = 4096;

        Tb tb;
        tb.t = new Vrsp_oc_wrap;
        tb.ram = mram;
        // the model
        static rsp_t m;
        std::vector<uint8_t> mm = mram;
        rsp_reset(&m, mm.data(), RDRAM_SIZE);
        m.rdp_cmd = on_rdp;
        model_rdp.clear(); rtl_rdp.clear();
        memcpy(m.dmem + 0xFC0, task, 64);
        memcpy(m.imem, &mm[boot], boot_size);
        if (rsp_run(&m, 400000000ull)) { fprintf(stderr, "%s: the task did not end on the model\n", argv[ai]); return 2; }
        // the RTL: loaded as osSpTaskLoad does
        tb.t->rst = 1; tb.t->rst_c = 1; for (int i = 0; i < 8; i++) tb.tick(); tb.t->rst = 0; tb.t->rst_c = 0; for (int i = 0; i < 4; i++) tb.tick();
        tb.dma_in(0x0FC0, scratch, 64);
        tb.dma_in(0x1000, boot, boot_size);
        tb.hw(8, 0);                         // the PC
        tb.hw(4, 0x00000001 | 0x4 | 0x100);  // clear halt and broke, interrupt on break
        long guard = 0;
        while ((!tb.t->halted || tb.t->dpc_busy || !tb.mq.empty()) && guard++ < 2000000000L) tb.tick();
        for (int i = 0; i < 200; i++) tb.tick();
        size_t n = std::min(model_rdp.size(), rtl_rdp.size()), k = 0;
        while (k < n && model_rdp[k] == rtl_rdp[k]) k++;
        bool ok = tb.t->halted && !tb.t->unimpl && k == model_rdp.size() && k == rtl_rdp.size();
        printf("%s: %s -- %zu command words (the model's %zu), %ld cycles", argv[ai], ok ? "the same commands" : "DIFFERENT",
               rtl_rdp.size(), model_rdp.size(), tb.cyc);
        if (!ok && getenv("TB_DUMP")) for (size_t i = (k > 6 ? k - 6 : 0); i < k + 8 && i < std::max(model_rdp.size(), rtl_rdp.size()); i++) printf("\n   [%zu] model %08x rtl %08x", i, i < model_rdp.size() ? model_rdp[i] : 0u, i < rtl_rdp.size() ? rtl_rdp[i] : 0u);
        if (!ok) printf("; first difference at word %zu%s%s", k, tb.t->halted ? "" : ", the RSP did not halt", tb.t->unimpl ? ", unimplemented instruction" : "");
        printf("\n");
        bad |= !ok;
        delete tb.t;
    }
    return bad;
}
