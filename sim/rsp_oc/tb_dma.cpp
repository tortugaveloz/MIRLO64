// The RSP's host-side DMA behaviour (rsp_top_oc alone), after the console's RSP DMA behaviour; started from the audit's benches
// (~/n64-fidelity/rsp_tests):
//   A  a second DMA written while the first is busy is queued (SP_DMA_FULL), not mixed into it: the registers it writes
//      are the next one's, the running one's pointers go on; both transfers are right
//   C  a third command while FULL replaces the queued one
//   B  SP_SEMAPHORE: a host read returns the bit and sets it (0 1 1); a write clears it
//   D  a host DMA into DMEM while the RSP runs a load / store loop: no RSP access is lost (it waits), no DMA word is wrong
//   E  ... and one out of DMEM (RDRAM <- DMEM) while it runs
//      tb_dma [gap cycles]  (the cycles the RSP runs before the host's DMA in D / E; default: a list of them)
#include "Vrsp_top_oc.h"
#include "Vrsp_top_oc___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <deque>
static std::vector<uint32_t> ram(1 << 21);
static Vrsp_top_oc *t; static long cyc;
static std::deque<std::pair<long, uint32_t>> rdq;
static int fails;
static uint32_t dmem(uint32_t a) { return t->rootp->rsp_top_oc__DOT__dmem__DOT__mem[(a >> 2) & 1023]; }
static void tick()
{
    t->r_ready = 1; t->r_rvalid = 0;
    if (!rdq.empty() && rdq.front().first <= cyc) { t->r_rvalid = 1; t->r_rdata = ram[rdq.front().second >> 2]; rdq.pop_front(); }
    t->dpc_start = t->dpc_end = t->dpc_current = 0; t->dpc_status = 0x80; t->dx_addr = 0;
    t->eval();
    uint32_t ra = ((uint32_t)t->r_addr << 2) & 0x7FFFFF; bool go = t->r_valid && t->r_ready;
    if (go && !t->r_we) rdq.push_back({ cyc + 4, ra });
    uint32_t wd = t->r_wdata; bool wr = go && t->r_we;
    t->clk = 1; t->eval(); t->clk = 0; t->eval(); cyc++;
    if (wr) ram[ra >> 2] = wd;
}
static void hw(int r, uint32_t v) { t->h_addr = r; t->h_wdata = v; t->h_we = 1; tick(); t->h_we = 0; }
#ifdef OLD_RTL        // (the RTL before the h_rd port: build_dma.sh's second argument)
#define H_RD(v)
#else
#define H_RD(v) t->h_rd = (v)
#endif
static uint32_t hr(int r) { t->h_addr = r; H_RD(1); t->eval(); uint32_t v = t->h_rdata; tick(); H_RD(0); return v; }
static void wait_idle() { for (int n = 0; n < 100000 && hr(6); n++) ; }
static void dma_in(uint32_t sp, uint32_t dr, uint32_t bytes) { hw(0, sp); hw(1, dr); hw(2, bytes - 1); wait_idle(); }
static void check(const char *what, long got, long want)
{
    if (got != want) { printf("FAIL %s: %ld, want %ld\n", what, got, want); fails++; }
    else printf("ok   %s\n", what);
}
static void reset()
{
    t->rst = 1; t->clk = 0; t->h_we = 0; H_RD(0); for (int i = 0; i < 5; i++) { t->clk = 1; t->eval(); t->clk = 0; t->eval(); } t->rst = 0;
    rdq.clear();
}
static int wrong(uint32_t sp, uint32_t base, uint32_t n) { int b = 0; for (uint32_t i = 0; i < n; i++) if (dmem(sp + i * 4) != base + i) b++; return b; }

// D / E: the RSP loops over lw / sw while the host's DMA runs; returns the number of wrong things
static void rsp_loop(int gap, bool out)
{
    reset();
    static const uint32_t prog[] = { 0x20010000, 0x20030000, 0x8C650C00, 0xAC610800, 0xAC650400, 0x20210001, 0x20630004, 0x282200C8, 0x1440FFF9, 0, 0x0000000D, 0 };
    for (int i = 0; i < 12; i++) ram[(0x8000 >> 2) + i] = prog[i];
    for (uint32_t i = 0; i < 256; i++) ram[(0x9000 >> 2) + i] = 0x5A5A0000u + i;
    for (uint32_t i = 0; i < 256; i++) ram[(0xA000 >> 2) + i] = 0xD0D00000u + i;
    for (uint32_t i = 0; i < 256; i++) ram[(0xB000 >> 2) + i] = 0;
    dma_in(0x1000, 0x8000, 48);
    dma_in(0xC00, 0x9000, 800);
    if (out) dma_in(0x000, 0xA000, 1024);                  // (the source of the DMA out: DMEM 0..0x3FF)
    hw(8, 0); hw(4, 1);                                    // PC 0, clear halt
    for (int i = 0; i < gap; i++) tick();
    if (out) { hw(0, 0x000); hw(1, 0xB000); hw(3, 1023); } // the host's DMA: RDRAM 0xB000 <- DMEM 0..0x3FF
    else     { hw(0, 0x000); hw(1, 0xA000); hw(2, 1023); } // the host's DMA into DMEM 0..0x3FF while the RSP runs
    for (int n = 0; n < 200000 && !(hr(4) & 1); n++) ;
    wait_idle(); for (int i = 0; i < 20; i++) tick();
    int b1 = 0, b2 = 0, b3 = 0;
    for (uint32_t i = 0; i < 200; i++) { if (dmem(0x800 + i * 4) != i) b1++; if (dmem(0x400 + i * 4) != 0x5A5A0000u + i) b2++; }
    if (out) { for (uint32_t i = 0; i < 256; i++) if (ram[(0xB000 >> 2) + i] != 0xD0D00000u + i) b3++; }
    else     { for (uint32_t i = 0; i < 256; i++) if (dmem(i * 4) != 0xD0D00000u + i) b3++; }
    printf("%s gap %3d: RSP stores wrong %d/200 (sw r1), %d/200 (lw->sw); host DMA words wrong %d/256\n", out ? "E out" : "D in ", gap, b1, b2, b3);
    if (b1 || b2 || b3) fails++;
}

// F: vector loads and stores (LQV, LTV, STV, SQV: each beat of the transposed ones a port access) in a loop while the host's DMA
// runs: the results (DMEM 0x900..0xEFF) must be those of the same program without one
static std::vector<uint32_t> vec_run(int gap, int mode)    // mode: 0 no DMA, 1 into DMEM 0..0x3FF, 2 out of it
{
    static const uint32_t prog[] = { 0x20010000, 0x20020800, 0x20030900, 0x20040028, 0x20050C00, 0xC8412000, 0xC8422001, 0x4A0208D0, 0xE8632000, 0xC8485802,
                                     0xE8A85800, 0x20630010, 0x20A50010, 0x2084FFFF, 0x1480FFF6, 0x00000000, 0x0000000D, 0x00000000 };
    reset();
    for (int i = 0; i < 18; i++) ram[(0x8000 >> 2) + i] = prog[i];
    for (uint32_t i = 0; i < 256; i++) ram[(0x9000 >> 2) + i] = 0x5A5A0000u * (i + 1) + 0x01230045u * i;
    for (uint32_t i = 0; i < 256; i++) ram[(0xA000 >> 2) + i] = 0xD0D00000u + i;
    dma_in(0x1000, 0x8000, 72);
    dma_in(0x800, 0x9000, 1024);
    if (mode == 2) dma_in(0x000, 0xA000, 1024);
    hw(8, 0); hw(4, 1);
    for (int i = 0; i < gap; i++) tick();
    if (mode == 1) { hw(0, 0x000); hw(1, 0xA000); hw(2, 1023); }
    if (mode == 2) { hw(0, 0x000); hw(1, 0xB000); hw(3, 1023); }
    for (int n = 0; n < 200000 && !(hr(4) & 1); n++) ;
    wait_idle(); for (int i = 0; i < 20; i++) tick();
    std::vector<uint32_t> r;
    for (uint32_t a = 0x900; a < 0xF00; a += 4) r.push_back(dmem(a));
    if (mode == 1) for (uint32_t i = 0; i < 256; i++) r.push_back(dmem(i * 4) == 0xD0D00000u + i ? 0 : 1);
    if (mode == 2) for (uint32_t i = 0; i < 256; i++) r.push_back(ram[(0xB000 >> 2) + i] == 0xD0D00000u + i ? 0 : 1);
    return r;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    t = new Vrsp_top_oc;
    reset();
    for (uint32_t i = 0; i < ram.size(); i++) ram[i] = 0xA0000000u + i;   // word i
    // ---- A: a second DMA while the first is busy
    hw(0, 0x000); hw(1, 0x1000); hw(2, 0xFF);               // DMEM 0 <- RDRAM 0x1000, 256 bytes
    check("DMA_BUSY right after RD_LEN", hr(6), 1);
    hw(0, 0x400); hw(1, 0x2000); hw(2, 0x7F);               // the second: DMEM 0x400 <- RDRAM 0x2000, 128 bytes (queued)
    check("SP_DMA_FULL with one queued", hr(5), 1);
    check("SP_STATUS: busy (bit 2) and full (bit 3)", hr(4) & 0xC, 0xC);
    check("SP_MEM_ADDR reads the running DMA's pointer (not the queued one's 0x400)", hr(0) < 0x100, 1);
    wait_idle(); for (int i = 0; i < 50; i++) tick();
    check("A: the first DMA's words wrong", wrong(0, 0xA0000000u + (0x1000 >> 2), 64), 0);
    check("A: the second DMA's words wrong", wrong(0x400, 0xA0000000u + (0x2000 >> 2), 32), 0);
    check("SP_DMA_FULL after both", hr(5), 0);
    check("SP_DMA_BUSY after both", hr(6), 0);
    // ---- C: a third command while FULL replaces the queued one
    for (uint32_t a = 0; a < 0x1000; a += 4) { /* (DMEM as it is) */ }
    hw(0, 0x800); hw(1, 0x3000); hw(2, 0xFF);               // a long one running
    hw(0, 0xC00); hw(1, 0x4000); hw(2, 0x3F);               // queued ...
    hw(0, 0xE00); hw(1, 0x5000); hw(2, 0x3F);               // ... and replaced
    wait_idle(); for (int i = 0; i < 50; i++) tick();
    check("C: the running one (0x800)", wrong(0x800, 0xA0000000u + (0x3000 >> 2), 64), 0);
    check("C: the replaced command did not run (0xC00 still the second DMA's data or the old content)", wrong(0xC00, 0xA0000000u + (0x4000 >> 2), 16) == 16, 1);
    check("C: the replacement ran (0xE00)", wrong(0xE00, 0xA0000000u + (0x5000 >> 2), 16), 0);
    // ---- B: the semaphore
    uint32_t s1 = hr(7), s2 = hr(7), s3 = hr(7);
    printf("     host semaphore reads: %u %u %u\n", s1, s2, s3);
    check("B: semaphore reads 0 1 1", s1 * 100 + s2 * 10 + s3, 11);
    hw(7, 0); uint32_t w1 = hr(7), w2 = hr(7);
    check("B: after a write: 0 1", w1 * 10 + w2, 1);
    // ---- D / E
    if (argc > 1) { rsp_loop(atoi(argv[1]), false); rsp_loop(atoi(argv[1]), true); }
    else {
        const int gaps[] = { -1, 0, 5, 15, 30, 60 };
        for (int g : gaps) if (g >= 0) rsp_loop(g, false);
        for (int g : gaps) if (g >= 0) rsp_loop(g, true);
    }
    {
        auto ref = vec_run(0, 0);
        int nz = 0; for (size_t i = 0; i < ref.size(); i++) nz += ref[i] != 0;
        printf("     the vector loop's reference (no DMA): %d of %zu words non-zero\n", nz, ref.size());
        for (int mode = 1; mode <= 2; mode++)
            for (int g : { 0, 3, 10, 25, 50, 100, 200 }) {
                auto r = vec_run(g, mode); int bad = 0;
                for (size_t i = 0; i < ref.size(); i++) if (r[i] != ref[i]) bad++;
                for (size_t i = ref.size(); i < r.size(); i++) bad += r[i] != 0;
                printf("F %s gap %3d: vector results / DMA words wrong: %d\n", mode == 1 ? "in " : "out", g, bad);
                if (bad) fails++;
            }
    }
    printf(fails ? "tb_dma: FAIL (%d)\n" : "tb_dma: PASS\n", fails);
    return fails != 0;
}
