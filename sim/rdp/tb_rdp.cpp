// rtl/rdp against the C model (lang/c/mrdp/mrdp.c built with MRDP_N64, the
// contract), on real graphics tasks.
//   tb_rdp [-o out_prefix] task_gfx_*.bin ...
// A task dump (sim/rsp) is run on the RSP's reference model; the RDP commands
// it gives go to both the RTL and the model, each with its own copy of the
// N64's memory as the RSP left it; then every byte of the two must match.
// The RTL's memory is a LiteDRAM-native-style port with random back-pressure
// and read latency.
//   tb_rdp -r SEED [N commands]
// A random command stream instead (random state, loads of every kind from a
// random texture image at any alignment, triangles and rectangles in every
// cycle type): what real tasks do not reach.
//   TB_STEP=1   compare after every command, stop at the first that differs
#include "Vrdp_top.h"
#include "Vrdp_top___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <set>
#include <vector>
extern "C" {
#include "rsp.h"
}
#include "mrdp.h"   // mrdp.c is built as C++ here too

static const uint32_t SIZE = 8u << 20;                     // the RDRAM (the RSP's)
static const uint32_t MSIZE = 16u << 20;                   // the RTL's and the model's: and the hidden plane at 8 MiB (mrdp.h)
static uint8_t *rsp_mem, *rtl_mem, *mdl_mem;
static uint16_t mdl_rd(void *, uint32_t a) { a &= MSIZE - 2; return (uint16_t)(mdl_mem[a] << 8 | mdl_mem[a + 1]); }
static void mdl_wr(void *, uint32_t a, uint16_t v) { a &= MSIZE - 2; mdl_mem[a] = (uint8_t)(v >> 8); mdl_mem[a + 1] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static Vrdp_top *top;
static uint64_t cycles;
static std::deque<uint32_t> cmdq;                           // words for the RTL
struct Rd { uint64_t due; uint32_t addr; };
static std::deque<Rd> reads;
static std::deque<uint32_t> wr_addrs;                        // accepted write commands
static unsigned rng = 12345;
static unsigned rnd() { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static uint64_t n_rd, n_wr;
// TB_FAST=1: the memory as the SoC's SDRAM port with nothing else on it --
// every request taken, a read's word 6 cycles on -- instead of the random
// stalls and latencies the comparison runs with. TB_PROF=1: the cycles spent
// in each of the sequencer's states.
// TB_LAT=n: a read's shortest latency (6); TB_STALL=k: a request is refused
// one cycle in k (8).
static bool fast_mem, prof;
static unsigned mem_lat = 6, mem_stall = 8;
// TB_SDRAM=1: the memory as the SoC's -- rtl/soc/sdr_ctrl.sv's rules on the
// Pocket's SDRAM: a row open per bank (word address: row a[23:11], bank
// a[10:9]), a word a cycle on a row hit, a miss a precharge and an activate
// (tRP 2, tRCD 2, tRAS 4, tWR 2), 3 cycles from a read to a write and 2 back,
// a refresh every 480 cycles, a read's word 5 cycles on. TB_CPU=p: another
// master asks for a random word p % of the cycles (the game CPU's cache
// misses), served in turn with the RDP.
static bool sdram;
static unsigned cpu_pct;
static struct {
    bool open[4]; uint32_t row[4]; int c_act[4], c_pre[4], c_wr[4]; int c_rd, c_wrany;
    int refi, wait, st;               // st 0 run, 1 precharge for a refresh, 2 refresh
    bool ref_due, cpu_v, cpu_we, turn; uint32_t cpu_a;
    uint64_t n_miss, n_cpu, n_turn;
} sd;
static inline int sat15(int v) { return v >= 15 ? 15 : v + 1; }
// one cycle of the controller with this request at its port: true when the read or write goes
static bool sd_step(bool v, bool we, uint32_t a, bool *is_rdp)
{
    // the arbiter: the RDP and the other master in turn
    bool pick_cpu = sd.cpu_v && (!v || sd.turn);
    bool rv = pick_cpu ? true : v, rwe = pick_cpu ? sd.cpu_we : we; uint32_t ra = pick_cpu ? sd.cpu_a : a;
    int cmd = 0, ba = 0; uint32_t row = (ra >> 11) & 0x1FFF; int bank = (ra >> 9) & 3;   // 1 ACT 2 PRE 3 RD 4 WR 5 PREALL 6 REF
    bool go = false;
    if (sd.wait == 0) {
        if (sd.st == 1) {
            bool ok = true;
            for (int b = 0; b < 4; b++) if (sd.open[b] && !(sd.c_act[b] >= 4 && sd.c_wr[b] >= 2)) ok = false;
            if (ok) cmd = 5;
        } else if (sd.st == 2) cmd = 6;
        else if (!sd.ref_due && rv) {
            ba = bank;
            if (sd.open[bank] && sd.row[bank] == row) {
                if (sd.c_act[bank] >= 2 && (rwe ? sd.c_rd >= 3 : sd.c_wrany >= 2)) { cmd = rwe ? 4 : 3; go = true; }
                else if (sd.c_act[bank] >= 2) sd.n_turn++;
            } else if (sd.open[bank]) { if (sd.c_act[bank] >= 4 && sd.c_wr[bank] >= 2) cmd = 2; }
            else if (sd.c_pre[bank] >= 2) { cmd = 1; sd.n_miss++; }
        }
    }
    sd.c_rd = cmd == 3 ? 1 : sat15(sd.c_rd);
    sd.c_wrany = cmd == 4 ? 1 : sat15(sd.c_wrany);
    for (int b = 0; b < 4; b++) {
        sd.c_act[b] = (cmd == 1 && ba == b) ? 1 : sat15(sd.c_act[b]);
        sd.c_pre[b] = ((cmd == 2 && ba == b) || cmd == 5) ? 1 : sat15(sd.c_pre[b]);
        sd.c_wr[b]  = (cmd == 4 && ba == b) ? 1 : sat15(sd.c_wr[b]);
    }
    if (cmd == 1) { sd.open[ba] = true; sd.row[ba] = row; }
    if (cmd == 2) sd.open[ba] = false;
    if (cmd == 5) for (int b = 0; b < 4; b++) sd.open[b] = false;
    if (++sd.refi >= 480) { sd.refi = 0; sd.ref_due = true; }
    if (sd.wait) sd.wait--;
    else if (sd.st == 0) { if (sd.ref_due && !go) sd.st = 1; }
    else if (sd.st == 1) { if (cmd == 5) { sd.st = 2; sd.wait = 2; } }
    else { sd.st = 0; sd.wait = 6; sd.ref_due = false; }
    if (go) { sd.turn = !pick_cpu; if (pick_cpu) { sd.cpu_v = false; sd.n_cpu++; } }
    *is_rdp = go && !pick_cpu;
    if (!sd.cpu_v && cpu_pct && rnd() % 100 < cpu_pct) { sd.cpu_v = true; sd.cpu_we = rnd() % 5 == 0; sd.cpu_a = (rnd() << 8 ^ rnd()) & 0x1FFFFF; }
    return go && !pick_cpu;
}
static uint64_t st_cyc[32];


static void tick()
{
    top->cmd_valid = !cmdq.empty();
    top->cmd_data = cmdq.empty() ? 0 : cmdq.front();
    top->m_cmd_ready = fast_mem || (rnd() % mem_stall) != 0;
    top->m_wdata_ready = !wr_addrs.empty() && (fast_mem || (rnd() % mem_stall) != 0);
    if (sdram) {
        // (the RDP's command is out of a register: this cycle's is there before its ready)
        bool mine;
        // TB_ZFREE=<hex address>: what a Z buffer out of the SDRAM would give, from
        // below -- the Z image's words (320 x 240 from there) are taken at once and
        // cost the SDRAM nothing (the RDP still spends a cycle of its channel on each)
        static const uint32_t zfree = getenv("TB_ZFREE") ? (uint32_t)strtoul(getenv("TB_ZFREE"), 0, 16) : 0;
        uint32_t ca = top->m_cmd_addr & 0x7FFFFFu;
        if (zfree && top->m_cmd_valid && ca >= zfree && ca < zfree + 320u * 240u * 2u) {
            bool dummy; sd_step(false, false, 0, &dummy);
            top->m_cmd_ready = 1;
            static unsigned long nz[2]; nz[top->m_cmd_we ? 1 : 0]++;      // (how many they are, as it goes)
            if (((nz[0] + nz[1]) & 0x3FFF) == 0) fprintf(stderr, "\r  Z words: %lu read, %lu written", nz[0], nz[1]);
        } else
        top->m_cmd_ready = sd_step(top->m_cmd_valid, top->m_cmd_we, 0xE00000u + ((top->m_cmd_addr >> 2) & 0x1FFFFFu), &mine);
        top->m_wdata_ready = !wr_addrs.empty();
    }
    bool rv = !reads.empty() && reads.front().due <= cycles;
    top->m_rdata_valid = rv;
    if (rv) top->m_rdata = be32(rtl_mem + (reads.front().addr & (MSIZE - 4)));
    top->clk = 0; top->eval();
    bool cmd_take = top->cmd_valid && top->cmd_ready;
    bool mc = top->m_cmd_valid && top->m_cmd_ready;
    uint32_t maddr = top->m_cmd_addr; bool mwe = top->m_cmd_we;
    bool wd = top->m_wdata_valid && top->m_wdata_ready;
    uint32_t wdat = top->m_wdata, wwe = top->m_wdata_we;
    if (prof) st_cyc[top->rootp->rdp_top__DOT__st & 31]++;
    top->clk = 1; top->eval();
    cycles++;
    if (cmd_take) {
        // TB_CMDT=<file>: the cycle each command word is taken (rdp_flow.py)
        static FILE *ct = getenv("TB_CMDT") ? fopen(getenv("TB_CMDT"), "w") : nullptr;
        if (ct) fprintf(ct, "%llu\n", (unsigned long long)cycles);
        cmdq.pop_front();
    }
    if (rv) reads.pop_front();
    if (mc) {
        if (mwe) wr_addrs.push_back(maddr);
        else { reads.push_back({ cycles + (sdram ? 5 : mem_lat + (fast_mem ? 0 : rnd() % 12)), maddr }); n_rd++; }
    }
    if (wd) {
        uint32_t a = wr_addrs.front() & (MSIZE - 4); wr_addrs.pop_front();
        for (int b = 0; b < 4; b++) if (wwe & (8u >> b)) rtl_mem[a + b] = (uint8_t)(wdat >> (24 - 8 * b));   // we[3]: the first byte
        n_wr++;
    }
}
static bool rtl_idle() { return cmdq.empty() && top->idle && reads.empty() && wr_addrs.empty(); }

static std::vector<uint32_t> cmds;
static std::set<unsigned> kinds;
static void on_rdp(void *, const uint32_t *w, int n) { for (int i = 0; i < 2 * n; i++) cmds.push_back(w[i]); }

// ---- a random command stream -------------------------------------------------
static unsigned grng;
static unsigned gr() { grng = grng * 1664525u + 1013904223u; return grng >> 8; }
static unsigned gr(unsigned n) { return gr() % n; }
static int grs(int n) { return (int)gr(2 * n + 1) - n; }
// (the random streams' images: the depth image a multiple of 8 KiB above the colour image -- in the same bank of the SoC's SDRAM,
// word for word, TB_SDRAM's rows -- or TB_ZIMG=<address>: where given, 0x280800 a bank on)
static const uint32_t R_CIMG = 0x200000, R_ZIMG = getenv("TB_ZIMG") ? (uint32_t)strtoul(getenv("TB_ZIMG"), 0, 0) : 0x280000, R_TEX = 0x100000;
static void cmd2(uint32_t a, uint32_t b) { cmds.push_back(a); cmds.push_back(b); }
// TB_CC=1: the combiner's arithmetic under test -- cycle types 0 and 1 only, the
// blender passing the combiner's colour alone (no FORCE_BL, P = the pixel), every
// combiner word random, the colour registers set often
static const bool cc_mode = getenv("TB_CC") != nullptr;
static void rand_modes()
{
    static const unsigned cyc[10] = { 0, 0, 0, 0, 1, 1, 1, 2, 3, 0 };
    uint32_t h = gr() & 0x00FFFFFFu & ~(3u << 20), l = gr() << 8 | gr(256);
    h |= (cc_mode ? gr(2) : cyc[gr(10)]) << 20;
    if (cc_mode) l &= ~(0xF0000000u | 1u << 14);
    if (gr(3)) h &= ~(1u << 15);                       // the TLUT mostly off
    if (gr(4) == 0) l &= ~0x30u;                       // sometimes no Z at all
    if (gr(2)) l &= ~(1u | 1u << 12);                  // alpha compare / coverage-by-alpha half the time
    cmd2(0x2F000000u | h, l);
}
static void rand_tile(unsigned t)
{
    static const unsigned fmts[8] = { 0, 0, 2, 3, 4, 0, 3, 1 };
    unsigned fmt = fmts[gr(8)], size = gr(4), line = gr(4) ? gr(17) : gr(512), tmem = gr(3) ? gr(256) & ~3u : gr(512);
    uint32_t w1 = t << 24 | gr(16) << 20 | gr(2) << 19 | gr(2) << 18 | gr(2) << 9 | gr(2) << 8;
    w1 |= (gr(3) ? gr(8) : gr(16)) << 14 | (gr(3) ? 0 : gr(16)) << 10 | (gr(3) ? gr(8) : gr(16)) << 4 | (gr(3) ? 0 : gr(16));
    cmd2(0x35000000u | fmt << 21 | size << 19 | line << 9 | tmem, w1);
    unsigned uls = gr(3) ? 0 : gr(256), ult = gr(3) ? 0 : gr(256);
    cmd2(0x32000000u | uls << 12 | ult, t << 24 | ((uls + gr(256)) & 0xFFF) << 12 | ((ult + gr(256)) & 0xFFF));
}
static void rand_timg()
{
    uint32_t a = R_TEX + gr(0x60000);
    if (gr(4)) a &= ~7u; else if (gr(2)) a &= ~1u;
    cmd2(0x3D000000u | gr(5) << 21 | gr(4) << 19 | (gr(3) ? gr(64) : gr(1024)), a);
}
static void rand_load()
{
    unsigned t = gr(8), k = gr(10);
    if (k < 5) {                                        // LOAD BLOCK
        unsigned uls = gr(4) ? 0 : gr(64), ult = gr(4) ? 0 : gr(16);
        unsigned lrs = gr(8) ? uls + gr(gr(3) ? 256 : 2048) : gr(4096);
        cmd2(0x33000000u | uls << 12 | ult, t << 24 | (lrs & 0xFFF) << 12 | gr(4096));
    } else if (k < 8) {                                 // LOAD TILE
        unsigned s0 = gr(64), t0 = gr(64), s1 = gr(8) ? s0 + gr(64) : gr(128), t1 = gr(8) ? t0 + gr(gr(2) ? 8 : 40) : gr(64);
        cmd2(0x34000000u | (s0 << 2 | gr(4)) << 12 | (t0 << 2 | gr(4)), t << 24 | ((s1 << 2 | gr(4)) & 0xFFF) << 12 | ((t1 << 2 | gr(4)) & 0xFFF));
    } else {                                            // LOAD TLUT
        unsigned s0 = gr(3) ? 0 : gr(240), s1 = gr(8) ? s0 + gr(gr(2) ? 16 : 256) : gr(256);
        cmd2(0x30000000u | (s0 << 2 | gr(4)) << 12 | (gr(6) ? 0 : gr(8) << 2), t << 24 | ((s1 << 2 | gr(4)) & 0xFFF) << 12);
    }
}
static void put_group(const int32_t v[4], const int32_t dx[4], const int32_t de[4])
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = gr() << 8 | gr(256);     // DaDy: whatever
    for (int k = 0; k < 4; k++) {
        int wi = k >> 1, sh = (k & 1) ? 0 : 16;
        uint32_t m = ~(0xFFFFu << sh);
        w[0 + wi] = (w[0 + wi] & m) | ((uint32_t)v[k] >> 16) << sh;   w[4 + wi] = (w[4 + wi] & m) | ((uint32_t)v[k] & 0xFFFF) << sh;
        w[2 + wi] = (w[2 + wi] & m) | ((uint32_t)dx[k] >> 16) << sh;  w[6 + wi] = (w[6 + wi] & m) | ((uint32_t)dx[k] & 0xFFFF) << sh;
        w[8 + wi] = (w[8 + wi] & m) | ((uint32_t)de[k] >> 16) << sh;  w[12 + wi] = (w[12 + wi] & m) | ((uint32_t)de[k] & 0xFFFF) << sh;
    }
    for (int i = 0; i < 16; i++) cmds.push_back(w[i]);
}
static void rand_tri()
{
    // three vertices, x 16.16 and y in quarter rows, around the image; now and then far outside
    int64_t x[3]; int y[3];
    int big = gr(12) == 0;
    for (int i = 0; i < 3; i++) { x[i] = (int64_t)((int)gr(big ? 1200 : 400) - (big ? 400 : 40)) << 16 | gr(65536); y[i] = (int)gr(big ? 4 * 600 : 4 * 300) - (big ? 600 : 120); }
    if (gr(6) == 0) { int c = (int)gr(320) << 16, d = (int)gr(4 * 240); for (int i = 0; i < 3; i++) { x[i] = c + grs(6 << 16); y[i] = d + grs(24); } }   // a small one
    if (gr(5) == 0) {          // on the edge walker's grid: vertices on quarter pixels, a few LSBs off (the sticky bit's cases)
        for (int i = 0; i < 3; i++) x[i] = (x[i] & ~(int64_t)0x3FFF) | (gr(3) == 0 ? gr(4) : 0);
    }
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++) if (y[j] > y[j + 1]) { std::swap(y[j], y[j + 1]); std::swap(x[j], x[j + 1]); }
    auto slope = [](int64_t dxv, int dyq) -> int32_t {          // 16.16 a row, clamped
        if (dyq == 0) return 0;
        int64_t q = dxv * 4 / dyq;
        return (int32_t)(q > 0x3FFFFFFF ? 0x3FFFFFFF : q < -0x3FFFFFFF ? -0x3FFFFFFF : q);
    };
    int32_t dxh = slope(x[2] - x[0], y[2] - y[0]), dxm = slope(x[1] - x[0], y[1] - y[0]), dxl = slope(x[2] - x[1], y[2] - y[1]);
    int f = y[0] & 3;
    int32_t xh = (int32_t)(x[0] - (int64_t)dxh * f / 4), xm = (int32_t)(x[0] - (int64_t)dxm * f / 4), xl = (int32_t)x[1];
    int64_t xh_y1 = x[0] + (int64_t)dxh * (y[1] - y[0]) / 4;
    unsigned lft = x[1] > xh_y1;
    if (gr(40) == 0) lft ^= 1;
    unsigned fl = gr(8), op = 0x08 | fl;
    cmd2((uint32_t)op << 24 | lft << 23 | gr(8) << 19 | gr(8) << 16 | ((uint32_t)y[2] & 0x3FFF), ((uint32_t)y[1] & 0x3FFF) << 16 | ((uint32_t)y[0] & 0x3FFF));
    cmd2((uint32_t)xl, (uint32_t)dxl); cmd2((uint32_t)xh, (uint32_t)dxh); cmd2((uint32_t)xm, (uint32_t)dxm);
    if (fl & 4) {
        int32_t v[4], dx[4], de[4];
        for (int k = 0; k < 4; k++) { v[k] = (int32_t)(gr(280) << 16 | gr(65536)) - (10 << 16); dx[k] = grs(3 << 16); de[k] = grs(3 << 16); }
        // (TB_CC: now and then a shade that overshoots both ways, 0..511 and past -- the interpolator's 9-bit saturation)
        if (cc_mode && gr(3) == 0) for (int k = 0; k < 4; k++) { v[k] = (int32_t)(gr(1400) << 16 | gr(65536)) - (400 << 16); dx[k] = grs(6 << 16); de[k] = grs(6 << 16); }
        put_group(v, dx, de);
    }
    if (fl & 2) {
        int32_t v[4], dx[4], de[4];
        int wide = gr(4) == 0;
        for (int k = 0; k < 2; k++) { v[k] = grs(wide ? 0x7FFFFFF0 : 64 << 21); dx[k] = grs(wide ? 1 << 24 : 1 << 20); de[k] = grs(wide ? 1 << 24 : 1 << 20); }
        v[2] = gr(8) ? (int32_t)(0x04000000u + gr(0x7B0000) * 256u) : grs(0x7FFFFFF0); dx[2] = grs(1 << (gr(4) ? 16 : 22)); de[2] = grs(1 << (gr(4) ? 16 : 22));
        v[3] = (int32_t)(gr() << 8); dx[3] = (int32_t)gr(); de[3] = (int32_t)gr();
        put_group(v, dx, de);
    }
    if (fl & 1) {
        cmds.push_back(gr(8) ? gr(0x7FFF) << 16 | gr(65536) : gr() << 8 | gr(256));
        cmds.push_back((uint32_t)grs(1 << (gr(4) ? 18 : 24))); cmds.push_back((uint32_t)grs(1 << (gr(4) ? 18 : 24))); cmds.push_back(gr() << 8);
    }
}
static void rand_rect(int tex)
{
    unsigned x0 = gr(8) ? gr(4 * 300) : gr(4096), y0 = gr(8) ? gr(4 * 230) : gr(4096);
    unsigned x1 = gr(8) ? x0 + gr(4 * 120) : gr(4096), y1 = gr(8) ? y0 + gr(4 * 60) : gr(4096);
    if (!tex) { cmd2(0x36000000u | (x1 & 0xFFF) << 12 | (y1 & 0xFFF), (x0 & 0xFFF) << 12 | (y0 & 0xFFF)); return; }
    cmd2((gr(5) ? 0x24000000u : 0x25000000u) | (x1 & 0xFFF) << 12 | (y1 & 0xFFF), gr(8) << 24 | (x0 & 0xFFF) << 12 | (y0 & 0xFFF));
    static const unsigned steps[6] = { 0x0400, 0x1000, 0x0200, 0x0800, 0xFC00, 0x0155 };
    cmd2(gr(4) ? gr(64 << 5) << 16 | gr(64 << 5) : gr() << 8 | gr(256), (gr(4) ? steps[gr(6)] : gr(65536)) << 16 | (gr(4) ? steps[gr(6)] : gr(65536)));
}
static void rand_combine()
{
    unsigned k = cc_mode ? 9 : gr(10);
    if (k < 4)      cmd2(0x3C000000u | 0xFFFFFFu, 0xFF000000u | 1u << 15 | 7u << 12 | 1u << 9 | 1u << 6 | 7u << 3 | 1u | 7u << 21 | 7u << 18);   // TEXEL0
    else if (k < 6) cmd2(0x3C000000u | 1u << 20 | 4u << 15 | 1u << 12 | 4u << 9 | 1u << 5 | 4u, 0xFF000000u | 7u << 15 | 7u << 12 | 7u << 9 | 7u << 6 | 7u << 3 | 7u | 1u << 21 | 4u << 18);   // TEXEL0 * SHADE
    else {
        uint32_t hi = gr() & 0xFFFFFF, lo = gr() << 8 | gr(256);
        if (cc_mode) {         // the second cycle takes the first's result (COMBINED) as A, as C (its alpha), as D
            if (gr(2)) { hi &= ~(15u << 5); lo &= ~(7u << 21); }
            if (gr(4) == 0) { hi = (hi & ~31u) | 7u; }
            if (gr(5) == 0) { hi &= ~31u; }                    // ... and the RGB as C (selection 0)
            if (gr(4) == 0) { lo &= ~(7u << 6 | 7u); }
            if (gr(4) == 0) { hi &= ~(15u << 5); lo = (lo & ~(15u << 24)) | 0u; }
            // SET CONVERT's constants: K5 as C and K4 as B, in either cycle
            if (gr(5) == 0) hi = (hi & ~(31u << 15)) | 15u << 15;
            if (gr(5) == 0) lo = (lo & ~(15u << 28)) | 7u << 28;
            if (gr(8) == 0) hi = (hi & ~31u) | 15u;
            if (gr(8) == 0) lo = (lo & ~(15u << 24)) | 7u << 24;
        }
        cmd2(0x3C000000u | hi, lo);
    }
}
static void rand_stream(unsigned seed, int n)
{
    // TB_CW=<w>: the colour image's width (320; Banjo-Kazooie's is 292: rows share hidden-plane groups)
    static const unsigned cw1 = getenv("TB_CW") ? (unsigned)atoi(getenv("TB_CW")) - 1u : 319u;
    grng = seed * 2654435761u + 12345u;
    for (uint32_t i = 0; i < SIZE; i++) rsp_mem[i] = (uint8_t)gr(256);
    cmds.clear();
    cmd2(0x3F000000u | 2u << 19 | cw1, R_CIMG);
    cmd2(0x3E000000u, R_ZIMG);
    cmd2(0x2D000000u, 320u << 14 | 240u << 2);
    for (int t = 0; t < 8; t++) rand_tile((unsigned)t);
    rand_timg(); rand_modes(); rand_combine();
    for (int i = 0; i < n; i++) {
        unsigned k = gr(100);
        if (k < 30) rand_tri();
        else if (k < 38) rand_rect(1);
        else if (k < 42) rand_rect(0);
        else if (k < 52) rand_modes();
        else if (k < 60) rand_combine();
        else if (k < 68) rand_tile(gr(8));
        else if (k < 74) rand_timg();
        else if (k < 84) rand_load();
        else if (k < 90) cmd2((cc_mode && gr(2) ? (gr(3) ? 0x3Au + gr(2) : 0x2Cu) : 0x37u + gr(5)) << 24 | gr(256), gr() << 8 | gr(256));        // fill, fog, blend, prim, env (TB_CC: and convert)
        else if (k < 91) cmd2(0x2E000000u, gr() << 8 | gr(256));                              // prim depth
        else if (k < 92) cmd2(0x2C000000u | gr(1 << 22), gr() << 8 | gr(256));                // convert: K4, K5 (the combiner's)
        else if (k < 94) {                                                                    // scissor
            unsigned x0 = gr(4 * 100), y0 = gr(4 * 80);
            cmd2(0x2D000000u | x0 << 12 | y0, (x0 + gr(4 * 320 - x0 + 1)) << 12 | (y0 + gr(4 * 240 - y0 + 1)));
        }
        else if (k < 95) cmd2(0x2D000000u, 320u << 14 | 240u << 2);
        else if (k < 96) {                                                                    // (now and then not a 16-bit image)
            // TB_RAND_CZ=1: and, one time in three, the depth image as the colour image,
            // as a game clears its Z buffer: fill mode, a colour, a few rectangles (or a
            // triangle), then the colour image again. TB_RAND_CZ=2: the depth image stays
            // the colour image for whatever comes
            static const int cz = getenv("TB_RAND_CZ") ? atoi(getenv("TB_RAND_CZ")) : 0;
            uint32_t w = 0x3F000000u | (gr(6) ? 2u : gr(4)) << 19 | cw1;
            if (cz == 1 && gr(3) == 0) {
                cmd2(0x3F000000u | 2u << 19 | cw1, R_ZIMG);
                cmd2(0x2F000000u | 3u << 20 | gr(1u << 20), gr());
                cmd2(0x37000000u, gr());
                for (int j = 1 + (int)gr(3); j > 0; j--) { if (gr(5)) rand_rect(0); else rand_tri(); }
                cmd2(w, R_CIMG);
                rand_modes();
            } else
                cmd2(w, cz == 2 && gr(3) == 0 ? R_ZIMG : R_CIMG);
        }
        else if (k < 97) cmd2(0x3E000000u, R_ZIMG);
        else if (k < 98) cmd2((0x26u + gr(4)) << 24, 0);                                      // syncs
        else cmd2(gr(4) ? (0x2Au + gr(3)) << 24 | gr(0xFFFFFF) : gr(8) << 24 | gr(0xFFFFFF), gr() << 8);   // not drawn: key, convert, 0..7
    }
    cmd2(0x3F000000u | 2u << 19 | cw1, R_CIMG);
    cmd2(0x29000000u, 0);
}

static void write_ppm(const char *path, const uint8_t *mem, uint32_t cimg, uint32_t width)
{
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u 240\n255\n", width);
    for (uint32_t i = 0; i < width * 240u; i++) {
        uint32_t a = (cimg + 2 * i) & (SIZE - 2);
        unsigned v = mem[a] << 8 | mem[a + 1], r5 = v >> 11, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
        unsigned char c[3] = { (unsigned char)(r5 << 3 | r5 >> 2), (unsigned char)(g5 << 3 | g5 >> 2), (unsigned char)(b5 << 3 | b5 >> 2) };
        fwrite(c, 1, 3, o);
    }
    fclose(o);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    const char *prefix = nullptr;
    int ai = 1;
    if (ai + 1 < argc && !strcmp(argv[ai], "-o")) { prefix = argv[ai + 1]; ai += 2; }
    if (ai >= argc) { fprintf(stderr, "usage: tb_rdp [-o out_prefix] task.bin ... | -r SEED [N]\n"); return 2; }
    const bool step = getenv("TB_STEP") != nullptr;
    fast_mem = getenv("TB_FAST") != nullptr; prof = getenv("TB_PROF") != nullptr;
    sdram = getenv("TB_SDRAM") != nullptr; if (getenv("TB_CPU")) cpu_pct = (unsigned)atoi(getenv("TB_CPU"));
    if (getenv("TB_LAT")) mem_lat = (unsigned)atoi(getenv("TB_LAT"));
    if (getenv("TB_STALL")) mem_stall = (unsigned)atoi(getenv("TB_STALL"));
    rsp_mem = (uint8_t *)malloc(SIZE); rtl_mem = (uint8_t *)malloc(MSIZE); mdl_mem = (uint8_t *)malloc(MSIZE);
    static rsp_t s;
    static mrdp_t mdl;
    int bad = 0;
    const bool rnd_mode = !strcmp(argv[ai], "-r");
    char rname[64];
    for (int k = 0; ai < argc; ai++, k++) {
        if (rnd_mode) {
            if (k) break;
            unsigned seed = ai + 1 < argc ? (unsigned)strtoul(argv[ai + 1], 0, 0) : 1;
            rand_stream(seed, ai + 2 < argc ? atoi(argv[ai + 2]) : 400);
            snprintf(rname, sizeof rname, "random %u", seed);
            argv[ai] = rname;
        } else {
        FILE *f = fopen(argv[ai], "rb");
        uint8_t hdr[16];
        if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "RSPT", 4) || fread(rsp_mem, 1, SIZE, f) != SIZE) {
            fprintf(stderr, "%s: not a task dump\n", argv[ai]); return 2;
        }
        fclose(f);
        // the RSP's model: the task as osSpTaskLoad starts it
        uint32_t ta = be32(hdr + 4) & (SIZE - 1u);
        rsp_reset(&s, rsp_mem, SIZE);
        s.rdp_cmd = on_rdp;
        cmds.clear();
        uint8_t task[64];
        memcpy(task, rsp_mem + ta, 64);
        static const int ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };
        for (unsigned i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++) wbe32(task + 4 * ptrs[i], be32(task + 4 * ptrs[i]) & 0x1FFFFFFFu);
        memcpy(s.dmem + 0xFC0, task, 64);
        uint32_t boot = be32(task + 8) & (SIZE - 1u), bs = be32(task + 12);
        memcpy(s.imem, rsp_mem + boot, bs > 4096 ? 4096 : bs);
        if (rsp_run(&s, 200000000ull)) { fprintf(stderr, "%s: the task did not end\n", argv[ai]); return 2; }
        }
        memcpy(rtl_mem, rsp_mem, SIZE);
        memcpy(mdl_mem, rsp_mem, SIZE);
        memset(rtl_mem + SIZE, 0xFF, MSIZE - SIZE);         // (the hidden plane: all ones, as the runtime sets it)
        memset(mdl_mem + SIZE, 0xFF, MSIZE - SIZE);

        mrdp_init(&mdl, nullptr, mdl_rd, mdl_wr);
        if (top) delete top;
        top = new Vrdp_top;
        cmdq.clear(); reads.clear(); wr_addrs.clear();
        top->rst = 1;
        for (int i = 0; i < 8; i++) tick();
        top->rst = 0;
        uint64_t c0 = cycles, ncmd = 0;
        uint32_t cimg = 0, width = 320;
        std::vector<uint32_t> cur;
        if (const char *dp = getenv("TB_DUMP")) {          // TB_DUMP=file: the command words, big-endian (to replay on the model alone)
            if (FILE *df = fopen(dp, "wb")) { for (uint32_t x : cmds) { uint8_t b4[4]; wbe32(b4, x); fwrite(b4, 1, 4, df); } fclose(df); }
        }
        for (uint32_t x : cmds) {
            cmdq.push_back(x); mrdp_push(&mdl, x);
            cur.push_back(x);
            if (mdl.ncmd != 0) continue;                    // command not complete yet
            ncmd++;
            if (getenv("TB_MODES")) {          // TB_MODES=1: each primitive's command and the other modes it is drawn with
                unsigned o = (cur[0] >> 24) & 63;
                if ((o >= 0x08 && o <= 0x0F) || o == 0x24 || o == 0x25 || o == 0x36)
                    fprintf(stderr, "PRIM #%llu op %02x other %06x %08x combine %06x %08x blend %08x prim %08x env %08x fill %08x w0 %08x w1 %08x\n", (unsigned long long)ncmd, o,
                            mdl.other_h, mdl.other_l, mdl.combine_hi, mdl.combine_lo, *(uint32_t *)&mdl.blend, *(uint32_t *)&mdl.prim, *(uint32_t *)&mdl.env, mdl.fill, cur[0], cur.size() > 1 ? cur[1] : 0);
            }
            {
                unsigned o = (cur[0] >> 24) & 63;
                if (((o & 0x3A) == 0x0A) || o == 0x24 || o == 0x25) {       // a textured primitive: its texel's kind
                    const mrdp_tile_t &t = mdl.tile[o < 0x10 ? (cur[0] >> 16) & 7 : (cur[1] >> 24) & 7];
                    kinds.insert(((mdl.other_h >> 20) & 3) << 12 | ((mdl.other_h >> 14) & 3) << 8 | t.fmt << 4 | t.size);
                }
            }
            if (((cur[0] >> 24) & 63) == MRDP_OP_SET_CIMG && mdl.cimg != mdl.zimg) { cimg = mdl.cimg; width = mdl.cimg_width; }
            if (step) {
                while (!rtl_idle()) tick();
                for (int i = 0; i < 16; i++) tick();
                if (getenv("TB_LFSR") && top->rootp->rdp_top__DOT__pix__DOT__lfsr != mdl.lfsr) {
                    fprintf(stderr, "LFSR differs after command #%llu op %02x: rtl %06x model %06x (other %06x %08x)\n", (unsigned long long)ncmd, (cur[0] >> 24) & 63,
                            top->rootp->rdp_top__DOT__pix__DOT__lfsr, mdl.lfsr, mdl.other_h, mdl.other_l);
                    exit(1);
                }
                if (memcmp(rtl_mem, mdl_mem, MSIZE)) {
                    uint32_t o = 0; while (!memcmp(rtl_mem + o, mdl_mem + o, 2)) o += 2;
                    uint32_t ci = mdl.cimg & (SIZE - 2), zi = mdl.zimg & (SIZE - 2), w = mdl.cimg_width;
                    const char *what = "memory"; uint32_t px = 0, py = 0;
                    if (o >= ci && o < ci + w * 480u) { what = "color"; px = (o - ci) / 2 % w; py = (o - ci) / 2 / w; }
                    else if (o >= zi && o < zi + w * 480u) { what = "depth"; px = (o - zi) / 2 % w; py = (o - zi) / 2 / w; }
                    printf("FIRST DIVERGENCE after command #%llu op %02x (%zu words): %s at %06x x=%u y=%u rtl %02x%02x model %02x%02x\n",
                           (unsigned long long)ncmd, (cur[0] >> 24) & 0x3F, cur.size(), what, o, px, py,
                           rtl_mem[o], rtl_mem[o + 1], mdl_mem[o], mdl_mem[o + 1]);
                    printf("  other %06x %08x  combine %06x %08x  fill %08x  cimg %08x w %u zimg %08x timg %08x w %u size %u\n",
                           mdl.other_h, mdl.other_l, mdl.combine_hi, mdl.combine_lo, mdl.fill, mdl.cimg, mdl.cimg_width, mdl.zimg,
                           mdl.timg, mdl.timg_width, mdl.timg_size);
                    for (int t = 0; t < 8; t++)
                        printf("  tile %d: fmt %u size %u line %u tmem %u pal %u  s %u..%u t %u..%u  cs %u ms %u masks %u shifts %u  ct %u mt %u maskt %u shiftt %u\n", t,
                               mdl.tile[t].fmt, mdl.tile[t].size, mdl.tile[t].line, mdl.tile[t].tmem, mdl.tile[t].palette,
                               mdl.tile[t].uls, mdl.tile[t].lrs, mdl.tile[t].ult, mdl.tile[t].lrt, mdl.tile[t].cs, mdl.tile[t].ms,
                               mdl.tile[t].masks, mdl.tile[t].shifts, mdl.tile[t].ct, mdl.tile[t].mt, mdl.tile[t].maskt, mdl.tile[t].shiftt);
                    for (size_t i = 0; i < cur.size(); i++) printf("  w%zu %08x\n", i, cur[i]);
                    uint64_t nd = 0;
                    for (uint32_t q = 0; q < MSIZE; q += 2) if (memcmp(rtl_mem + q, mdl_mem + q, 2)) nd++;
                    printf("  %llu 16-bit words differ\n", (unsigned long long)nd);
                    return 1;
                }
            }
            cur.clear();
            while (cmdq.size() > 64) tick();              // keep the RTL fed, not flooded
        }
        uint64_t guard = 0;
        while (!rtl_idle() && guard++ < 400000000ull) tick();
        for (int i = 0; i < 64; i++) tick();

        uint64_t diff = 0; uint32_t first = 0;
        for (uint32_t o = 0; o < MSIZE; o += 2)
            if (memcmp(rtl_mem + o, mdl_mem + o, 2)) { if (!diff) first = o; diff++; }
        printf("%s: %llu commands, RTL %llu cycles (%llu reads, %llu writes), sync %u/%u loads %u/%u unknown %u/%u, %llu pixels: %s",
               argv[ai], (unsigned long long)ncmd, (unsigned long long)(cycles - c0), (unsigned long long)n_rd, (unsigned long long)n_wr,
               top->sync_count, mdl.sync_count, top->load_count, mdl.load_count, top->unknown_ops, mdl.unknown_ops,
               (unsigned long long)mdl.pixels_drawn, diff ? "MISMATCH" : "RTL == model\n");
        if (diff) printf(", %llu 16-bit words differ, first at %06x\n", (unsigned long long)diff, first);
        if (sdram) { printf("\n    SDRAM: %llu row misses, %llu cycles lost turning between reads and writes, %llu of the other master's accesses", (unsigned long long)sd.n_miss, (unsigned long long)sd.n_turn, (unsigned long long)sd.n_cpu); sd.n_miss = sd.n_turn = sd.n_cpu = 0; }
        if (prof) {
            static const char *nm[20] = { "idle (waiting for commands)", "triangle start", "row", "setup", "read the span's Z", "read the span's colour", "pixels", "drain the pipeline",
                                          "write colour", "write Z", "next row", "wait for memory", "fill", "texture load", "sync", "rectangle start", "load start", "span", "attributes", "XL" };
            printf("\n");
            for (int i = 0; i < 20; i++) if (st_cyc[i]) printf("    %-28s %9llu cycles  %5.1f %%\n", nm[i], (unsigned long long)st_cyc[i], 100.0 * st_cyc[i] / cycles);
            for (int i = 0; i < 32; i++) st_cyc[i] = 0;
        }
        if (getenv("TB_KINDS")) {
            printf("  textured primitives' kinds (cycle type, TLUT bits, format, size):");
            for (unsigned v : kinds) printf(" %04x", v);
            printf("\n");
        }
        if (diff || top->sync_count != mdl.sync_count || !rtl_idle()) bad = 1;
        if (prefix && cimg) {
            char path[1024];
            snprintf(path, sizeof path, "%s_%d_rtl.ppm", prefix, k); write_ppm(path, rtl_mem, cimg, width);
            snprintf(path, sizeof path, "%s_%d_model.ppm", prefix, k); write_ppm(path, mdl_mem, cimg, width);
        }
    }
    if (top) delete top;
    return bad;
}
