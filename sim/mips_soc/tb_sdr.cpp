// sdr_arb + sdr_ctrl + sdr_phy against a behavioural SDR SDRAM (AS4C32M16:
// 4 banks, CL 3, BL 2) -- a cycle-level C++ chip model of the Pocket's
// SDRAM, where the PHY only works at read-data
// offset 0: a READ seen on the pads in sys2x cycle c puts its two beats on DQ
// during cycles c+3+OFS and c+4+OFS. Also checks what a read-data-only model
// would miss: commands to closed/open banks, tRCD/tRP/tRAS/tRFC/tWR, DQ
// contention.
//
// Traffic: four ports asking random reads and writes (byte enables) over a
// small area (row hits, misses, bank conflicts) and sequential bursts; every
// read is checked against a golden memory updated in the order requests were
// taken (the controller runs them in that order).
//
//   tb_sdr [cycles] [offset]
#include "Vsdr_test_top.h"
#include "Vsdr_test_top___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <deque>
#include <random>

static Vsdr_test_top *t;
static long errors;
#define ERR(...) do { if (errors++ < 20) { printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the chip (sys2x cycles)
static std::map<uint32_t, uint16_t> mem;       // (ba, row, col) -> 16 bits
static int rows[4], opened[4];
static long t_act[4], t_pre[4], t_wr[4], t_ref = -1000;
static std::map<long, uint16_t> pending;
static std::map<long, int> pending_bank;
static long t_actany = -1000, n_act, n_cmd1;
static long wburst_key = -1;
static long cyc;
static int OFS = 0;
static uint32_t key(int ba, int row, int col) { return (uint32_t)ba << 24 | (uint32_t)row << 10 | (col & 0x3ff); }

static void store(uint32_t k, uint16_t dq, int dm)
{
    uint16_t old = mem.count(k) ? mem[k] : 0, v = 0;
    for (int b = 0; b < 2; b++) v |= ((dm >> b) & 1 ? old : dq) & (0xff << (8 * b));
    mem[k] = v;
}

static void chip_step()    // after the sys2x edge that made cycle `cyc`
{
    int ras = t->sdram_ras_n, cas = t->sdram_cas_n, we = t->sdram_we_n;
    int ba = t->sdram_ba, a = t->sdram_a, dm = t->sdram_dm;
    uint16_t dq = t->dq_out;
    bool oe = t->dq_oe;
    if (wburst_key >= 0) { store((uint32_t)wburst_key, dq, dm); if (!oe) ERR("cyc %ld: write beat 2 without DQ enable", cyc); wburst_key = -1; }
    if (oe && pending.count(cyc)) ERR("cyc %ld: DQ contention (the PHY drives while read data is due)", cyc);
    int c = ras << 2 | cas << 1 | we;
    bool cmd_ok = t->sdram_cke;
    if (!cmd_ok) return;
    if (c != 7 && cyc - t_ref < 11) ERR("cyc %ld: command %d inside tRFC", cyc, c);
    switch (c) {
    case 3:  // ACT
        if (opened[ba]) ERR("cyc %ld: ACT to open bank %d", cyc, ba);
        if (cyc - t_pre[ba] < 3) ERR("cyc %ld: tRP (bank %d, %ld)", cyc, ba, cyc - t_pre[ba]);
        if (cyc - t_actany < 2) ERR("cyc %ld: tRRD (%ld)", cyc, cyc - t_actany);
        if (cyc - t_act[ba] < 8) ERR("cyc %ld: tRC (bank %d, %ld)", cyc, ba, cyc - t_act[ba]);
        opened[ba] = 1; rows[ba] = a; t_act[ba] = cyc; t_actany = cyc; n_act++; break;
    case 2:  // PRE
        for (int b = 0; b < 4; b++) if ((a & 0x400) || b == ba) {
            if (opened[b] && cyc - t_act[b] < 7) ERR("cyc %ld: tRAS (bank %d, %ld)", cyc, b, cyc - t_act[b]);
            if (opened[b] && cyc - t_wr[b] < 3) ERR("cyc %ld: tWR (bank %d, %ld)", cyc, b, cyc - t_wr[b]);
            // (the outputs go off CL after it: a beat of this bank due then is lost)
            for (auto &pb : pending_bank) if (pb.second == b && pb.first >= cyc + 3 + OFS) ERR("cyc %ld: PRECHARGE cuts a read of bank %d short", cyc, b);
            opened[b] = 0; t_pre[b] = cyc;
        }
        break;
    case 4: {  // WRITE
        if (!opened[ba]) ERR("cyc %ld: WRITE to closed bank %d", cyc, ba);
        if (cyc - t_act[ba] < 3) ERR("cyc %ld: tRCD (write)", cyc);
        if (!oe) ERR("cyc %ld: WRITE without DQ enable", cyc);
        int col = a & 0x3ff;
        store(key(ba, rows[ba], col), dq, dm);
        wburst_key = key(ba, rows[ba], col ^ 1);
        t_wr[ba] = cyc + 1;
        break;
    }
    case 5: {  // READ
        if (!opened[ba]) ERR("cyc %ld: READ from closed bank %d", cyc, ba);
        if (cyc - t_act[ba] < 3) ERR("cyc %ld: tRCD (read)", cyc);
        int col = a & 0x3ff;
        for (int k = 0; k < 2; k++) {
            uint32_t kk = key(ba, rows[ba], col ^ k);
            pending[cyc + 3 + OFS + k] = mem.count(kk) ? mem[kk] : 0;
            pending_bank[cyc + 3 + OFS + k] = ba;
        }
        break;
    }
    case 1:  // REF
        for (int b = 0; b < 4; b++) if (opened[b]) ERR("cyc %ld: REF with bank %d open", cyc, b);
        t_ref = cyc; break;
    case 0: break;  // MRS
    default: break;
    }
}

// ---- traffic
struct Port { bool busy; bool we; uint32_t addr, wdata, be; int burst; uint32_t next; };
static Port port[4];
static std::map<uint32_t, uint32_t> gold;      // word -> value
static std::deque<uint32_t> expect[4];
static std::mt19937 rng(1);
static long reads_ok;

static bool g_stream;     // STREAM=1: every port reads its own sequential area, no pauses
static void new_req(int i)
{
    Port &p = port[i];
    if (g_stream) { p.addr = p.next = (p.next + 1) & 0xffffff; p.we = false; p.be = 0xf; p.busy = true; return; }
    if (p.burst > 0) { p.addr = p.next; p.next = (p.next + 1) & 0xffffff; p.burst--; }
    else {
        uint32_t base = (i * 0x2345 + (rng() % 5) * 0x800) & 0xfffff;   // a few rows, shared between ports
        p.addr = (base + rng() % 64) & 0xffffff;
        if (getenv("TB_HID")) p.addr = 0xDC0000u + (p.addr & 0x3FFFFu);      // TB_HID=1: in the RDP's hidden plane (sdr_ctrl.sv remaps its banks); a wide range, bits 10:5 to 17 all vary
        if (getenv("TB_HID")) p.addr = 0xDC0000u + ((p.addr * 2654435761u >> 7) & 0x3FFFFu);
        if (rng() % 4 == 0) { p.burst = 7; p.next = (p.addr + 1) & 0xffffff; }
    }
    p.we = rng() % 3 == 0;
    p.wdata = rng();
    p.be = p.we ? (rng() % 2 ? 0xf : (rng() % 15) + 1) : 0xf;
    p.busy = true;
}

// a field of a wide (packed-array) port
template <class W> static void setf(W &w, int lsb, int bits, uint32_t v)
{
    for (int b = 0; b < bits; b++) {
        int k = lsb + b;
        uint32_t m = 1u << (k & 31);
        w[k >> 5] = ((v >> b) & 1) ? (w[k >> 5] | m) : (w[k >> 5] & ~m);
    }
}
static void set_inputs()
{
    uint32_t v = 0, w = 0, be = 0;
    for (int i = 0; i < 4; i++) {
        if (port[i].busy) v |= 1 << i;
        if (port[i].we) w |= 1 << i;
        setf(t->p_addr, 24 * i, 24, port[i].addr);
        t->p_wdata[i] = port[i].wdata;
        be |= port[i].be << (4 * i);
    }
    t->p_be = be;
    t->p_valid = v; t->p_we = w; t->p_urgent = 0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    long cycles = argc > 1 ? atol(argv[1]) : 200000;
    if (argc > 2) OFS = atoi(argv[2]);
    g_stream = getenv("STREAM") != nullptr;
    if (g_stream) for (int i = 0; i < 4; i++) port[i].next = 0x100000u * (i + 1) - 1;
    t = new Vsdr_test_top;
    t->rst = 1;
    for (int i = 0; i < 4; i++) { t_act[i] = t_pre[i] = t_wr[i] = -1000; }
    long sys = 0, words = 0;
    for (cyc = 0; sys < cycles; cyc++) {
        bool sys_edge = (cyc & 1) == 0;
        // the rising edge (sys2x, and sys every other one)
        t->clk2x = 1; if (sys_edge) t->clk = 1;
        t->eval();
        if (sys_edge) {                             // the sys cycle's inputs, just after its edge
            if (sys == 10) t->rst = 0;
            set_inputs();
            t->eval();
        }
        if (getenv("TRACE") && sys > 180 && sys < 215) {
            auto *r = t->rootp;
            printf("cyc %ld clk %d ps %d rd_en %d f_rden %d sr %02x rv %d ctrl_rd %d ras%d cas%d we%d\n", cyc, t->clk,
                   r->sdr_test_top__DOT__phy__DOT__phase_sel, r->sdr_test_top__DOT__phy__DOT__r_rddata_en,
                   r->sdr_test_top__DOT__phy__DOT__f_rden, r->sdr_test_top__DOT__phy__DOT__rden_sr,
                   r->sdr_test_top__DOT__phy__DOT__rddata_valid, r->sdr_test_top__DOT__ctrl__DOT__cmd_rd,
                   t->sdram_ras_n, t->sdram_cas_n, t->sdram_we_n);
        }
        chip_step();
        t->dq_in = pending.count(cyc) ? pending[cyc] : 0; pending.erase(cyc); pending_bank.erase(cyc);
        t->eval();
        t->clk2x = 0; if (!sys_edge) t->clk = 0;
        t->eval();
        if (!sys_edge) {
            // mid sys cycle: the handshakes that complete at the next sys edge
            if (t->ready) {
                for (int i = 0; i < 4; i++) {
                    if ((t->p_ready >> i) & 1) {
                        Port &p = port[i];
                        if (p.we) { uint32_t o = gold.count(p.addr) ? gold[p.addr] : 0, n = o;
                                    for (int b = 0; b < 4; b++) if ((p.be >> b) & 1) n = (n & ~(0xffu << 8 * b)) | (p.wdata & (0xffu << 8 * b));
                                    gold[p.addr] = n; }
                        else expect[i].push_back(gold.count(p.addr) ? gold[p.addr] : 0);
                        p.busy = false; words++;
                    }
                    if ((t->p_rvalid >> i) & 1) {
                        if (expect[i].empty()) ERR("sys %ld: port %d read data nobody asked for", sys, i);
                        else {
                            uint32_t want = expect[i].front(); expect[i].pop_front();
                            if (want != 0xdeaddeadu && t->p_rdata != want) ERR("sys %ld: port %d got %08x want %08x", sys, i, t->p_rdata, want);
                            else reads_ok++;
                        }
                    }
                }
                for (int i = 0; i < 4; i++) if (!port[i].busy && (g_stream || rng() % 4 != 0)) new_req(i);   // applied after the edge
            }
            sys++;
        }
    }
    int pend = 0;
    for (int i = 0; i < 4; i++) pend += expect[i].size();
    printf("offset %+d: %ld sys cycles, %ld words (%.2f/cycle after init), %ld reads checked, %d outstanding, %ld errors\n",
           OFS, sys, words, (double)words / sys, reads_ok, pend, errors);
    printf("%s\n", errors == 0 && pend < 16 ? "PASS" : "FAIL");
    return errors ? 1 : 0;
}
