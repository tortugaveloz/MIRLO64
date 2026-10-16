// MIRLO's MIPS SoC (rtl/soc/mirlo_mips.sv) under Verilator: the game CPU,
// the geom core, the audio core, MRDP, the scan-out, the SDRAM controller +
// PHY -- all RTL -- against the SDR chip model of tb_sdr.cpp (with its
// protocol checks). The program is put where the Pocket loads data slot 0
// (SDRAM 0x4000_0000); the Pocket's reset exit comes LOAD_AT sys cycles in
// (NOLOAD=1: with nothing loaded -- the boot ROM's serial-boot path).
//
//   tb_soc prog.bin [-c Mcycles] [-o dir] [-e every_n_frames]
// The UART's bytes go to stdout; a 0x04 byte ends the run. Every n-th
// frame goes to dir/fNNNNNN.ppm (read back from the chip model's memory at
// the scan-out's base).
//
// The Pocket's side of the data slots (the addresses are data.json's, read
// from pkg/pocket/Cores/tortuga.Mirlo64 -- DATA_JSON=<file>: another):
//   prog.bin         slot 0, at the bridge's address 0: the program, or a
//                    game file with the program and a ROM in it
//   ROM=<file.z64>   the two files of an N64 game: the ROM is slot 0, as it
//                    is, at the bridge's 1 MiB, and prog.bin a slot of its
//                    own (3) at 0. The core's table has the ROM's size for
//                    slot 0 and the program's for slot 3
//   SLOT<n>=<file>   slot n's file (1 the save, 2 the Controller Pak, 4 the save-type database), after
//                    the others --
//                    SLOTS_FIRST=1: before them
//   INTERACT=<address>:<value>[,...]  the Pocket writes the core's settings (interact.json addresses, 0x10000108 is "Controller 1 pak")
//   SLOTS_OUT=<dir>  when the run ends the Pocket quits the core: each
//                    nonvolatile slot is read back through the port, as many
//                    bytes as the program left in the core's table for it,
//                    into <dir>/slot<n>.bin
// What the Pocket writes below 49 MiB is the SDRAM's as it is (docs/mips.md,
// the bridge's map) and is put into the chip model at once; the rest -- a
// big ROM's end, the slots' files -- goes through the bridge's port, a word
// at a time. BRIDGE_ALL=1: all of it through the port, as the Pocket does it
// (some 6 clocks a word).
//   VERIFY=1         when the load is over, and again at the end (then less
//                    the program's megabyte and the saves), the chips'
//                    models are compared with the files, each byte where
//                    docs/mips.md says it is
#include "Vsoc_sim_top.h"
#include "Vsoc_sim_top___024root.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <array>
#include <deque>
#include <string>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <csignal>

static Vsoc_sim_top *t;
// The clocks. clk_sys is TB_SYS_HZ, clk_sys2x twice that; the game CPU's own
// clock (the SoC's MIRLO_CPU_CLK=32) is clk_sys
// x TB_CPU_N / TB_CPU_M, the three in the phase the PLL gives them: their
// rising edges together every TB_CPU_M cycles of clk_sys. Time is counted in
// units of a 4 x TB_CPU_N-th of clk_sys's period.
#ifndef TB_SYS_HZ
#define TB_SYS_HZ 62500000L
#endif
#ifndef TB_CPU_N
#define TB_CPU_N 3
#define TB_CPU_M 2
#endif
#define TB_SYS_KHZ (TB_SYS_HZ / 1000.0)
// the SDRAM's clock period (ps), for the chip model's checks
static const long SDR_PS = (long)(1e12 / (2.0 * TB_SYS_HZ) + 0.5);
static long errors;
#define ERR(...) do { if (errors++ < 20) { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)

// ---- the chip (sys2x cycles): 4 banks x 8192 rows x 1024 columns x 16 bits
static std::vector<uint16_t> mem(1u << 25);
#include <map>
#include <algorithm>
static std::map<uint32_t, uint32_t> pc_hist;   // PCPROF
static std::map<uint32_t, uint32_t> gpc_hist;  // GPCPROF
static std::map<uint32_t, uint32_t> pct_hist;  // PCTOP
static uint32_t gpc_idle;
static int rows[4], opened[4];
static long t_act[4], t_pre[4], t_wr[4], t_ref = -1000;
static long cyc;
static uint16_t pend_v[8]; static long pend_c[8];      // read beats due (a small ring)
static int pend_b[8];
static long t_actany = -1000;
static long wburst = -1;
static uint32_t key(int ba, int row, int col) { return (uint32_t)ba << 23 | (uint32_t)row << 10 | (col & 0x3ff); }
// the controller's word address -> the halves' keys (sdr_ctrl: row a[23:11], bank a[10:9], column pair a[8:0])
static uint32_t wkey(uint32_t w, int half) { return key((w >> 9) & 3, (w >> 11) & 0x1fff, ((w & 0x1ff) << 1) | half); }
static uint32_t rd_word(uint32_t w) { return mem[wkey(w, 0)] | (uint32_t)mem[wkey(w, 1)] << 16; }
static void wr_word(uint32_t w, uint32_t v) { mem[wkey(w, 0)] = v & 0xffff; mem[wkey(w, 1)] = v >> 16; }

// MEMWATCH=lo,hi (SDRAM byte addresses, 0x4xxxxxxx): every write to a word in
// the range -- the 16 bits, their mask, the game CPU's pc and which ports
// have words on the bus (mem_stat's low six bits) -- who writes what there
static void store(uint32_t k, uint16_t dq, int dm)
{
    static int wn = -1; static uint32_t wlo, whi; static long wcount;
    if (wn < 0) { const char *e = getenv("MEMWATCH"); wn = e != nullptr; if (wn) { wlo = strtoul(e, 0, 0) & 0x3FFFFFFu; whi = strchr(e, ',') ? strtoul(strchr(e, ',') + 1, 0, 0) & 0x3FFFFFFu : wlo + 4; } }
    if (wn) {
        uint32_t ba = k >> 23, row = (k >> 10) & 0x1fff, col = k & 0x3ff;
        uint32_t addr = (row << 11 | ba << 9 | (col >> 1)) << 2;
        if (addr >= wlo && addr < whi && wcount++ < 200000)
            printf("[watch %08x %s = %04x mask %d; cpu pc %08x ports %02x]\n", 0x40000000u | addr, (col & 1) ? "high" : "low ", dq, dm, t->cpu_pc, t->mem_stat & 0x3f);
    }
    uint16_t v = mem[k];
    if (!(dm & 1)) v = (v & 0xff00) | (dq & 0x00ff);
    if (!(dm & 2)) v = (v & 0x00ff) | (dq & 0xff00);
    mem[k] = v;
}
// The chip's timing, checked in time and not in cycles, so that it means
// something at another clock -- the figures sdr_ctrl.sv was written to
// (AS4C32M16MSA-6: tRCD, tRP 18 ns, tRAS 48 ns, tRC their sum, tRFC 80 ns,
// tRRD 12 ns, tWR 15 ns after the last beat; a clock of 6 ns at least at CL
// 3). cyc_of(ns): the clock periods a command must be behind.
static long cyc_of(double ns) { return (long)((ns * 1000.0 + SDR_PS - 1) / SDR_PS); }
static void chip_step()
{
    static const long N_RCD = cyc_of(18), N_RP = cyc_of(18), N_RAS = cyc_of(48), N_RC = cyc_of(66), N_RFC = cyc_of(80),
                      N_RRD = cyc_of(12), N_WR = cyc_of(15);
    static bool first = true;
    if (first) {
        first = false;
        if (SDR_PS < 6000) ERR("the SDRAM's clock period is %ld ps: under the chip's 6 ns", SDR_PS);
    }
    int ba = t->sdram_ba, a = t->sdram_a, dm = t->sdram_dm;
    uint16_t dq = t->dq_out;
    bool oe = t->dq_oe;
    if (wburst >= 0) { store((uint32_t)wburst, dq, dm); if (!oe) ERR("cyc %ld: write beat 2 without DQ", cyc); wburst = -1; }
    for (int i = 0; i < 8; i++) if (pend_c[i] == cyc && oe) ERR("cyc %ld: DQ contention", cyc);
    if (!t->sdram_cke) return;
    int c = t->sdram_ras_n << 2 | t->sdram_cas_n << 1 | t->sdram_we_n;
    if (c != 7 && cyc - t_ref < N_RFC) ERR("cyc %ld: command %d inside tRFC", cyc, c);
    switch (c) {
    case 3:
        if (opened[ba]) ERR("cyc %ld: ACT to open bank %d", cyc, ba);
        if (cyc - t_pre[ba] < N_RP) ERR("cyc %ld: tRP", cyc);
        if (cyc - t_actany < N_RRD) ERR("cyc %ld: tRRD", cyc);
        if (cyc - t_act[ba] < N_RC) ERR("cyc %ld: tRC", cyc);
        if (cyc - t_ref < N_RFC) ERR("cyc %ld: tRFC", cyc);
        opened[ba] = 1; rows[ba] = a; t_act[ba] = cyc; t_actany = cyc; break;
    case 2:
        for (int b = 0; b < 4; b++) if ((a & 0x400) || b == ba) {
            if (opened[b] && cyc - t_act[b] < N_RAS) ERR("cyc %ld: tRAS", cyc);
            if (opened[b] && cyc - t_wr[b] < N_WR) ERR("cyc %ld: tWR", cyc);          // (t_wr: the burst's second beat)
            for (int i = 0; i < 8; i++) if (pend_c[i] >= cyc + 3 && pend_b[i] == b) ERR("cyc %ld: PRECHARGE cuts a read short", cyc);
            opened[b] = 0; t_pre[b] = cyc;
        }
        break;
    case 4: {
        if (!opened[ba]) ERR("cyc %ld: WRITE to closed bank", cyc);
        if (cyc - t_act[ba] < N_RCD) ERR("cyc %ld: tRCD (write)", cyc);
        int col = a & 0x3ff;
        store(key(ba, rows[ba], col), dq, dm);
        wburst = key(ba, rows[ba], col ^ 1);
        t_wr[ba] = cyc + 1;
        break;
    }
    case 5: {
        if (!opened[ba]) ERR("cyc %ld: READ from closed bank", cyc);
        if (cyc - t_act[ba] < N_RCD) ERR("cyc %ld: tRCD (read)", cyc);
        int col = a & 0x3ff;
        for (int k = 0; k < 2; k++) {
            int s = (int)((cyc + 3 + k) & 7);
            pend_c[s] = cyc + 3 + k; pend_v[s] = mem[key(ba, rows[ba], col ^ k)]; pend_b[s] = ba;
        }
        break;
    }
    case 1:
        for (int b = 0; b < 4; b++) {
            if (opened[b]) ERR("cyc %ld: REF with a bank open", cyc);
            if (cyc - t_pre[b] < N_RP) ERR("cyc %ld: REF inside tRP", cyc);
        }
        t_ref = cyc; break;
    default: break;
    }
}


// CPUTRACE=from,count: the game CPU's instructions as it retires them (pc,
// word, the register written) and the exceptions it takes, `count` of them
// from the `from`-th
// (CPUTRACE, EXCSTAT, CPUSTAT, XTRACE, PCTOP: read by opts_load, at the start and again by a run resumed from a checkpoint -- CKPT)
static long ct_from = -1, ct_count, ct_n, o_exc_per = 60, o_xtrace, o_pctop;
static bool o_cstat, o_ctrace, o_excstat, o_pcprof;
static long o_pcprof_from, o_rcpstat;
static uint32_t o_rcp_ilo, o_rcp_ihi;
static bool ck_run;                     // this is a run from a checkpoint
static bool o_pad_load = true;          // PAD.., PAD_MASH are to be read (at the start, and again by a run from a checkpoint)
static FILE *o_ailog; static bool o_ailog_load = true;   // AILOG likewise (a run from a checkpoint: its own file, or none)
static void opts_load()
{
    o_cstat = getenv("CPUSTAT") != nullptr; o_ctrace = getenv("CPUTRACE") != nullptr; o_excstat = getenv("EXCSTAT") != nullptr;
    ct_from = -1; ct_n = 0;
    o_exc_per = o_excstat && atoi(getenv("EXCSTAT")) > 1 ? atoi(getenv("EXCSTAT")) : 60;
    o_xtrace = getenv("XTRACE") ? atol(getenv("XTRACE")) : 0;
    o_pctop = getenv("PCTOP") ? atol(getenv("PCTOP")) : 0;
    o_pcprof = getenv("PCPROF") != nullptr; o_pcprof_from = getenv("PCPROF_FROM") ? atol(getenv("PCPROF_FROM")) : 0;
    o_rcpstat = getenv("RCPSTAT") ? atol(getenv("RCPSTAT")) : 0;
    o_rcp_ilo = o_rcp_ihi = 0;
    if (ck_run) { o_pad_load = true; o_ailog = nullptr; o_ailog_load = true; }
    if (const char *e = getenv("RCPSTAT_IDLE")) { o_rcp_ilo = strtoul(e, 0, 0); o_rcp_ihi = strchr(e, ',') ? strtoul(strchr(e, ',') + 1, 0, 0) : o_rcp_ilo + 8; }
}
static void cpu_trace_step()
{
    long &from = ct_from, &count = ct_count, &n = ct_n;
    if (from < 0) { const char *e = getenv("CPUTRACE"); from = atol(e); count = strchr(e, ',') ? atol(strchr(e, ',') + 1) : 1000; }
    if (t->tr_exc && n >= from && n < from + count) printf("[cpu      exception %u at %08x]\n", t->tr_exc_code, t->tr_exc_pc);
    if (!t->tr_retire) return;
    if (n >= from && n < from + count) {
        if (t->tr_wr) printf("[cpu %ld %08x: %08x  r%u = %08x]\n", n, t->tr_pc, t->tr_insn, t->tr_rd, t->tr_val);
        else printf("[cpu %ld %08x: %08x]\n", n, t->tr_pc, t->tr_insn);
    }
    n++;
}

// EXCSTAT=1: the exceptions the game CPU takes (the monitor's entries in an OWN=1 build: an interrupt, a trap on a device access,
// a 64-bit instruction...), by cause code, a line every 60 fields (EXCSTAT=<n>: every n fields). Instrumentation only.
static void exc_stat_step()
{
    static long hist[32], tot, f_n; static bool vb_was; const long per = o_exc_per;
    static long retired, ex_total;
    if (t->tr_exc) { hist[t->tr_exc_code & 31]++; tot++; ex_total++; }
    if (t->tr_retire) retired++;
    if (t->vblank && !vb_was && ++f_n % per == 0) {
        printf("[excstat fields %ld..%ld: %ld exceptions (%.1f a field)", f_n - per + 1, f_n, tot, (double)tot / per);
        static const char *nm[32] = { "int", "mod", "tlbl", "tlbs", "adel", "ades", "ibe", "dbe", "sys", "bp", "ri", "cpu", "ov", "tr", "", "fpe" };
        for (int i = 0; i < 32; i++) if (hist[i]) printf(" %s %ld", i < 16 && nm[i][0] ? nm[i] : "?", hist[i]);
        printf("; %ld instructions retired]\n", retired);
        fflush(stdout);
        for (int i = 0; i < 32; i++) hist[i] = 0;
        tot = retired = 0;
    }
    vb_was = t->vblank;
}

// CPUSTAT (see main): one cycle of the game CPU's clock
static void cpu_stat_step(long sys)
{
    enum { C_RUN, C_DMISS, C_SLOW, C_FPU, C_MD, C_WBFULL, C_TLB, C_MOTHER, C_IMISS, C_ISLOW, C_FOTHER, C_HAZ, C_N };
    static const char *nm[C_N] = { "running", "D-cache line", "slow path", "FPU", "mult/div", "write buffer full", "translation", "M other",
                                   "I-cache line", "uncached fetch", "F empty", "D hazard" };
    static const double khz = TB_SYS_KHZ * TB_CPU_N / TB_CPU_M;        // the CPU's clock
    static long c[C_N], n, ret, idle;
    static long f_busy, f_n, f_sum, f_max;                              // a field's cycles outside the idle loop; fields, their sum, the longest
    static uint32_t ilo, ihi; static int iinit, every;
    static bool marks, m0_was, m1_was, vb_was;
    if (!iinit) {
        iinit = 1; every = atoi(getenv("CPUSTAT")) > 1;
        if (const char *e = getenv("CPUSTAT_IDLE")) { ilo = strtoul(e, 0, 0); ihi = strchr(e, ',') ? strtoul(strchr(e, ',') + 1, 0, 0) : ilo + 8; }
    }
    unsigned s = t->cpu_stat;
    unsigned ms = (s >> 2) & 7, fs = (s >> 5) & 7;
    int k = C_RUN;
    if (s & 2) {
        if (ms == 1 || ms == 2) k = C_DMISS;
        else if (ms == 3 || ms == 4 || ms == 5) k = C_SLOW;
        else if (ms == 6 || ms == 7) k = C_TLB;
        else if (s & 0x400) k = C_FPU;
        else if (s & 0x800) k = C_MD;
        else if (s & 0x1000) k = C_WBFULL;
        else k = C_MOTHER;
    } else if (s & 0x200) k = C_HAZ;
    else if (!(s & 0x100)) {
        if (fs == 1) k = C_IMISS;
        else if (fs == 2 || fs == 4 || fs == 5) k = C_ISLOW;
        else k = C_FOTHER;
    }
    bool in_idle = ihi && t->cpu_pc >= ilo && t->cpu_pc < ihi;
    if (in_idle) idle++;
    else { c[k]++; f_busy++; if (s & 1) ret++; }
    n++;
    // (the marks and vblank are the SoC's clock's: their rising edges)
    bool m0 = t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 24) == 0xA5u;
    bool m1 = t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 24) == 0xA6u;
    bool mark0 = m0 && !m0_was, mark1 = m1 && !m1_was;
    m0_was = m0; m1_was = m1;
    if (t->vblank && !vb_was) {
        if (every) printf("[cpustat: a field's CPU time %.2f ms]\n", f_busy / khz);
        f_n++; f_sum += f_busy; if (f_busy > f_max) f_max = f_busy;
        f_busy = 0;
    }
    vb_was = t->vblank;
    static long last_out;
    bool out = marks ? mark1 : sys / TB_SYS_HZ != last_out;
    if (mark0) marks = true;
    if (out) {
        last_out = sys / TB_SYS_HZ;
        long busy = n - idle;
        printf("[cpustat %ld cycles: idle %.1f %%, a field's CPU time %.2f ms (the longest %.2f); of the rest %ld instructions, %.3f cycles each:", n,
               100.0 * idle / (n ? n : 1), f_n ? f_sum / khz / f_n : 0.0, f_max / khz, ret, ret ? (double)busy / ret : 0.0);
        for (int i = 0; i < C_N; i++) printf(" %s %.1f %%%s", nm[i], 100.0 * c[i] / (busy ? busy : 1), i + 1 < C_N ? "," : "]\n");
        fflush(stdout);
    }
    if (out || mark0) { for (int i = 0; i < C_N; i++) c[i] = 0; n = ret = idle = 0; f_n = f_sum = f_max = 0; }
}

// ---- the Pocket's data slots (data.json) and its side of the bridge
struct Slot { uint32_t addr = 0, size = 0; bool there = false, nv = false; std::vector<uint8_t> data, back; };
static Slot slots[8];
static std::vector<uint8_t> &img = slots[0].data;   // slot 0: the program / the game file (with ROM=: the ROM)
static const uint32_t FILE_SDRAM_END = 49u << 20;   // (lang/mips/include/layout.h's LAYOUT_FILE_SDRAM
static const uint32_t FILE_DROP_END = 65u << 20;    // and LAYOUT_FILE_MAX)
static const int PROG_SLOT = 3;                     // with ROM=: the program's (data.json's "Runtime")
static bool read_file(const char *path, std::vector<uint8_t> &d)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    uint8_t b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    return true;
}
static uint32_t be32(const std::vector<uint8_t> &d, size_t i)
{
    uint32_t v = 0;
    for (size_t k = 0; k < 4; k++) v = v << 8 | (i + k < d.size() ? d[i + k] : 0);
    return v;
}
// data.json's slots: each one's "id", "address" and "nonvolatile"
static bool read_data_json(const std::string &path)
{
    std::vector<uint8_t> d;
    if (!read_file(path.c_str(), d)) return false;
    std::string j(d.begin(), d.end());
    for (size_t at = j.find("\"id\""); at != std::string::npos; at = j.find("\"id\"", at + 1)) {
        size_t lo = j.rfind('{', at), hi = j.find('}', at);
        if (lo == std::string::npos || hi == std::string::npos) continue;
        std::string o = j.substr(lo, hi - lo);
        size_t a = o.find("\"address\"");
        unsigned id = (unsigned)strtoul(o.c_str() + o.find(':', o.find("\"id\"")) + 1, nullptr, 0);
        if (a == std::string::npos || id >= 8) continue;
        slots[id].addr = (uint32_t)strtoul(o.c_str() + o.find('"', o.find(':', a)) + 1, nullptr, 0);
        slots[id].nv = o.find("\"nonvolatile\": true") != std::string::npos;
        slots[id].there = true;
    }
    return slots[0].there;
}
// one access after the other, a word each, each one's ack awaited
struct BrJob { int slot; bool write; uint32_t addr; size_t off, len; };
static std::deque<BrJob> br_jobs;
static size_t br_pos;
// The chips' models against the files, each byte where the bridge's map puts
// its address: the SDRAM below 49 MiB, nothing from there up to 65 MiB (the bridge drops it),
// else the SDRAM at the address's low 26 bits. `end`: less what a program
// changes -- the first megabyte and the saves.
static long verify(const char *when, bool end)
{
    long bad = 0, n = 0;
    for (int k = 0; k < 8; k++) {
        const std::vector<uint8_t> &d = slots[k].data;
        if (d.empty() || (end && slots[k].nv)) continue;
        for (size_t i = 0; i < d.size(); i += 4) {
            uint32_t a = slots[k].addr + (uint32_t)i, got, want = be32(d, i);
            if (end && a < (1u << 20)) continue;
            if (a < FILE_SDRAM_END || a >= FILE_DROP_END) got = rd_word((a & 0x3FFFFFFu) / 4);
            else {
                continue;
            }
            if (d.size() - i < 4) got &= ~0u << 8 * (4 - (d.size() - i));
            n++;
            if (got != want && bad++ < 8) printf("[verify %s: slot %d's file at bridge %08x: %08x, the file has %08x]\n", when, k, a, got, want);
        }
    }
    printf("[verify %s: %ld words compared, %ld differ]\n", when, n, bad);
    fflush(stdout);
    return bad;
}

// VGA=1: every frame the scan-out's pads send -- a pixel on each rising
// clk_vid with DE high and skip low, a row to DE's fall, a frame to vsync's rise -- to
// dir/vNNNNNN.ppm (one in `every`): 640 x 240, or 640 x 480 for a serrated picture's woven frame. The scaler slot the
// last line's end said (the word outside DE: 1 for a woven frame) goes in the VGAT log.
static long g_sys;                                   // (sys cycles, for the bench's own log)
static void vga_sample(const std::string &dir, long every)
{
    static std::vector<std::vector<uint8_t>> rows;
    static std::vector<uint8_t> row;
    static int pde, pvs;
    static long nf;
    if (t->vga_de && !t->vga_skip) { row.push_back((uint8_t)t->vga_r); row.push_back((uint8_t)t->vga_g); row.push_back((uint8_t)t->vga_b); }   // (8:8:8)
    static int slot;                                 // the slot the last line's end said
    const uint32_t word = (uint32_t)t->vga_r << 16 | (uint32_t)t->vga_g << 8 | t->vga_b;
    if (pde && !t->vga_de && !t->vga_skip) { rows.push_back(row); row.clear(); if ((word & 7u) == 0) slot = (word >> 13) & 0x7FF; }
    static long outside;                             // pads outside DE that are not 0 (the slot words)
    if (!t->vga_de && !t->vga_skip && (t->vga_r | t->vga_g | t->vga_b)) outside++;
    if (!pvs && t->vga_vsync) {
        static const bool vgat = getenv("VGAT") != nullptr;      // VGAT=1: each frame's size, slot and period (sys cycles) in the log
        static long last_sys;
        if (vgat) { printf("[vga frame %ld: %zu rows, %ld sys cycles since the last; %ld pad words outside DE not 0; slot %d]\n", nf, rows.size(), g_sys - last_sys, outside, slot); last_sys = g_sys; }
        size_t w = 0;
        for (auto &r : rows) if (r.size() > w) w = r.size();
        w /= 3;
        if (w && !dir.empty() && nf % every == 0) {
            char path[512];
            snprintf(path, sizeof path, "%s/v%06ld.ppm", dir.c_str(), nf);
            if (FILE *f = fopen(path, "wb")) {
                fprintf(f, "P6 %zu %zu 255\n", w, rows.size());
                for (auto &r : rows) { r.resize(3 * w, 0); fwrite(r.data(), 1, 3 * w, f); }
                fclose(f);
            }
        }
        rows.clear(); nf++;
    }
    if (!t->vga_skip) pde = t->vga_de;
    pvs = t->vga_vsync;
}

static void dump_frame(const std::string &dir, long n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%06ld.ppm", dir.c_str(), n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    // FB_STRIDE=<pixels>[,<rows>] (with FB_N64): the game's own framebuffer as it is, a row of <pixels> pixels (the VI's WIDTH:
    // 440 of a 440 x 330 picture), for a game whose picture the scan-out does not show whole yet; the file is that size
    if (const char *fs = getenv("FB_STRIDE")) {
        int w = atoi(fs), h = strchr(fs, ',') ? atoi(strchr(fs, ',') + 1) : 240;
        fprintf(f, "P6 %d %d 255\n", w, h);
        uint32_t w0 = (t->fb_base & 0x3FFFFFF) >> 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t v = rd_word(w0 + ((uint32_t)y * w + x) / 2);
                uint16_t p = (x & 1) ? v & 0xffff : v >> 16;
                uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) << 3), (uint8_t)(((p >> 6) & 31) << 3), (uint8_t)(((p >> 1) & 31) << 3) };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        return;
    }
    bool hi = t->fb_width >= 512;                    // a hi-res picture: 640 wide, 240 rows or (interlaced) 480
    int w = hi ? 640 : 320, h = hi && t->fb_tall ? 480 : 240;
    fprintf(f, "P6 %d %d 255\n", w, h);
    uint32_t w0 = (t->fb_base & 0x3FFFFFF) >> 2;
    // the scan-out's window (n64_video.sv): a framebuffer narrower than the screen
    // (Banjo-Kazooie: 292 wide) is read at ITS width, in the screen's middle with
    // black around it; width 0 / >= 320 / hi-res: the screen's own 320 x 240
    int vw = t->fb_width & ~1, vx0 = 0, vy0 = 0, vrows = 240;
    bool win = !hi && vw >= 2 && vw < 320;
    if (win) { vx0 = 0; vy0 = t->fb_y0; if (t->fb_rows && t->fb_rows <= 240) vrows = t->fb_rows; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2) {
            uint32_t v;
            if (win) {
                int fx = x - vx0, fy = y - vy0;
                bool in = fx >= 0 && fx < vw && fy >= 0 && fy < vrows;
                static const bool fbraw = getenv("FB_RAW") != nullptr;
                v = ((t->fb_on || fbraw) && in) ? rd_word(w0 + (fy * vw + fx) / 2) : 0;
            } else
                v = (t->fb_on || getenv("FB_RAW")) ? rd_word(w0 + (y * w + x) / 2) : 0;    // (the scan-out off: black)
            // FB_N64=1 (the SoC built with MIRLO_RSP): the game's own framebuffer --
            // RGBA5551, the left pixel in the word's high half
            static const bool n64fb = getenv("FB_N64") != nullptr;
            for (int h = 0; h < 2; h++) {
                uint16_t p = (h != n64fb) ? v >> 16 : v & 0xffff;
                uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
                if (n64fb) { rgb[1] = (uint8_t)(((p >> 6) & 31) << 3); rgb[2] = (uint8_t)(((p >> 1) & 31) << 3); }
                if (t->fb_gamma) {                    // as the scan-out shows it (n64_video's table)
                    static const uint8_t gam[32] = { 0, 44, 64, 78, 90, 100, 110, 118, 128, 134, 142, 150, 156, 162, 168, 174,
                                                     180, 186, 192, 196, 202, 206, 212, 216, 220, 226, 230, 234, 238, 242, 246, 250 };
                    for (int c = 0; c < 3; c++) rgb[c] = gam[rgb[c] >> 3];
                }
                fwrite(rgb, 1, 3, f);
            }
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: tb_soc prog.bin [-c Mcycles] [-o dir] [-e every]\n"); return 2; }
    long mcycles = 30, every = 1;
    std::string outdir;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) mcycles = atol(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) every = atol(argv[++i]);
    }
    // the program, where the Pocket puts slot 0: SDRAM 0x4000_0000, the file's
    // big-endian words as their values (the bridge does not swap: the CPU is big-endian)
    bool noload = getenv("NOLOAD") != nullptr;
    {   // data.json, beside the sources: sim/mips_soc/../../pkg
        std::string here(__FILE__), dj = getenv("DATA_JSON") ? getenv("DATA_JSON") :
            here.substr(0, here.rfind('/') + 1) + "../../pkg/pocket/Cores/tortuga.Mirlo64/data.json";
        if (!read_data_json(dj)) { fprintf(stderr, "%s: no slot 0 in it\n", dj.c_str()); return 2; }
    }
    // the slots' files: the program (ROM=: and the ROM, which is then slot 0
    // at 1 MiB whatever this data.json says), SLOT<n>
    int prog = 0;
    if (const char *rom = getenv("ROM")) {
        prog = PROG_SLOT;
        slots[0].addr = 1u << 20; slots[PROG_SLOT].addr = 0; slots[PROG_SLOT].there = true; slots[PROG_SLOT].nv = false;
        if (!read_file(rom, slots[0].data)) return 2;
        printf("ROM: %zu bytes at bridge 0x%08x\n", slots[0].data.size(), slots[0].addr);
    }
    if (!read_file(argv[1], slots[prog].data)) return 2;
    for (int k = 1; k < 8; k++) {
        char name[16]; snprintf(name, sizeof name, "SLOT%d", k);
        const char *f = getenv(name);
        if (!f) continue;
        if (!slots[k].there || k == prog) { fprintf(stderr, "%s: data.json has no such slot\n", name); return 2; }
        if (!read_file(f, slots[k].data)) return 2;
        printf("slot %d: %zu bytes at bridge 0x%08x\n", k, slots[k].data.size(), slots[k].addr);
    }
    // the Pocket's writes, in their order. What is the SDRAM's as it is goes
    // into the chip model at once, unless BRIDGE_ALL
    const bool bridge_all = getenv("BRIDGE_ALL") != nullptr, slots_first = getenv("SLOTS_FIRST") != nullptr;
    const bool verify_on = getenv("VERIFY") != nullptr;
    for (int pass = 0; pass < 2 && !noload; pass++)
        for (int k = 0; k < 8; k++) {
            const std::vector<uint8_t> &d = slots[k].data;
            if (d.empty() || ((k == 0 || k == prog) != (pass == (slots_first ? 1 : 0)))) continue;
            slots[k].size = (uint32_t)d.size();
            size_t len = (d.size() + 3) & ~(size_t)3, direct = 0;
            if (!bridge_all && slots[k].addr < FILE_SDRAM_END) {
                direct = len < FILE_SDRAM_END - slots[k].addr ? len : FILE_SDRAM_END - slots[k].addr;
                for (size_t i = 0; i < direct; i += 4) wr_word((slots[k].addr + (uint32_t)i) / 4, be32(d, i));
            }
            if (direct < len) br_jobs.push_back({k, true, slots[k].addr + (uint32_t)direct, direct, len - direct});
        }
    const std::vector<uint8_t> &program = slots[prog].data;
    printf("program: %zu bytes at 0x40000000%s\n", program.size(), noload ? " (NOLOAD: not given)" : "");
    // PRELOAD=file@0xADDR[@0xMARK]: a file's big-endian words straight into SDRAM
    // (e.g. a ROM where a program would read it; MARK: the n64 port's "already
    // there" stamp, the header CRC words, 8 bytes below)
    if (const char *pl = getenv("PRELOAD")) {
        std::string spec(pl);
        size_t at = spec.find('@');
        uint32_t addr = (uint32_t)strtoul(spec.c_str() + at + 1, nullptr, 0);
        FILE *pf = fopen(spec.substr(0, at).c_str(), "rb");
        if (!pf) { perror("PRELOAD"); return 2; }
        std::vector<uint8_t> d; { uint8_t b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, pf)) > 0) d.insert(d.end(), b, b + n); }
        fclose(pf);
        d.resize((d.size() + 3) & ~3u);
        for (size_t i = 0; i < d.size(); i += 4)
            wr_word(((addr & 0x3FFFFFF) >> 2) + (uint32_t)(i / 4), (uint32_t)d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | (uint32_t)d[i + 3]);
        if (d.size() >= 24) {        // the stamp: CRC1 ^ "ROM0", CRC2
            uint32_t c1 = (uint32_t)d[16] << 24 | d[17] << 16 | d[18] << 8 | d[19], c2 = (uint32_t)d[20] << 24 | d[21] << 16 | d[22] << 8 | d[23];
            wr_word(((addr - 8) & 0x3FFFFFF) >> 2, c1 ^ 0x524F4D30u);
            wr_word(((addr - 4) & 0x3FFFFFF) >> 2, c2);
        }
        printf("PRELOAD: %zu bytes at 0x%08x\n", d.size(), addr);
    }

    t = new Vsoc_sim_top;
    long load_at = getenv("LOAD_AT") ? atol(getenv("LOAD_AT")) : 3000;
    t->host_reset_n = 0; t->host_loaded = 0; t->cont1_key = 0; t->cont2_key = 0; t->cont3_key = 0; t->cont4_key = 0;
    t->cont1_joy = 0; t->cont2_joy = 0; t->cont3_joy = 0; t->cont4_joy = 0;
    t->ia_address = 0; t->ia_data = 0; t->ia_wr = 0;
    // INTERACT=<address>:<value>[,...] the Pocket writes the core's settings: <address> is interact.json's (0x10000108) or the interact
    // bus' own (mirlo_regs.sv: 2 is `interact0[4:0]` = 0x1000_0100 + 4 x 2)
    // once, 200 sys cycles after the core's reset ends
    std::vector<std::pair<int, uint32_t>> ia_list;
    if (const char *e = getenv("INTERACT")) for (const char *q = e; *q; ) { int a = (int)strtol(q, (char **)&q, 0); if (*q == ':') q++; uint32_t v = (uint32_t)strtoul(q, (char **)&q, 0); ia_list.push_back({a, v}); if (*q == ',') q++; }
    t->file_size = slots[0].size; t->br_complete = 0;
    long br_done_at = -1;                           // the bridge's read completes then (sys cycles)
    t->reset = 1;
    t->wbm_cyc = 0; t->wbm_we = 0; t->wbm_adr = 0; t->wbm_dat_w = 0;
    bool quit = false;                              // the run is over: the Pocket reads the nonvolatile slots back
    long bad = 0;
    for (int i = 0; i < 8; i++) pend_c[i] = -1;
    for (int b = 0; b < 4; b++) t_act[b] = t_pre[b] = t_wr[b] = -1000;
    long sys = 0, frames = 0;
    static long rs_cnt[4][6]; static unsigned rs_type = 0;
    long max_sys = mcycles * 1000000;
    opts_load();
    time_t t0 = time(nullptr);
    int vid_phase = 0;                              // clk_vid: sys / 2.5 (25 MHz)
    int pvb = 0;
    long audio_words = 0;
    bool done = false;
    // the game CPU's clock: its edges every 2 x TB_CPU_M units, clk_sys2x's every TB_CPU_N
    long cpu_t = 0;                                 // its next edge
    const bool &cstat = o_cstat, &ctrace = o_ctrace, &excstat = o_excstat;
    auto cpu_to = [&](long now) {                   // its edges before `now`, each on its own; one at `now` with what follows
        for (; cpu_t <= now; cpu_t += 2 * TB_CPU_M) {
            bool rise = (cpu_t / (2 * TB_CPU_M)) % 2 == 0;
            if (rise && cstat) cpu_stat_step(sys);
            if (rise && ctrace) cpu_trace_step();
            if (rise && excstat) exc_stat_step();
            t->clk_cpu = rise;
            if (cpu_t < now) t->eval();
        }
    };
    for (cyc = 0; ; cyc++) {
        bool sys_edge = (cyc & 1) == 0;
        if (sys_edge && (sys >= max_sys || done) && !quit) {
            quit = true;
            if (getenv("SLOTS_OUT"))
                for (int k = 1; k < 8; k++) if (slots[k].nv && slots[k].size) {
                    slots[k].back.assign((slots[k].size + 3) & ~3u, 0);
                    br_jobs.push_back({k, false, slots[k].addr, 0, slots[k].back.size()});
                }
        }
        if (sys_edge && quit && br_jobs.empty() && !t->wbm_cyc) break;
        cpu_to(cyc * 2L * TB_CPU_N);
        t->clk_sys2x = 1;
        if (sys_edge) {
            t->clk_sys = 1;
            if (sys == 20) t->reset = 0;
            t->file_size = slots[t->br_slot & 7].size;         // the core's table, by the slot the program selected
            { const long k = (sys - 200) / 4; t->ia_wr = 0;                            // (the settings: one write each, from 200 cycles in)
              if (sys >= 200 && (sys & 3) == 0 && k < (long)ia_list.size()) { t->ia_wr = 1; t->ia_address = (uint32_t)(ia_list[k].first >= 0x10000000 ? (ia_list[k].first - 0x10000100) >> 2 : ia_list[k].first) & 15u; t->ia_data = ia_list[k].second; } }
            // the bridge: an access (as apf_wishbone_master hands it on: the address's
            // bits 26-2, bit 28 set), its ack awaited; the reset's end after the load
            if (sys >= load_at && !br_jobs.empty()) {
                BrJob &j = br_jobs.front();
                if (!t->boot_ready) { }
                else if (t->wbm_cyc && t->wbm_ack) {
                    if (!j.write) for (int k = 0; k < 4; k++) slots[j.slot].back[br_pos + k] = (uint8_t)(t->wbm_dat_r >> (24 - 8 * k));
                    t->wbm_cyc = 0; br_pos += 4;
                    if (br_pos >= j.len) {
                        if (!j.write) printf("[bridge: slot %d read back, %u bytes from 0x%08x]\n", j.slot, slots[j.slot].size, j.addr);
                        br_jobs.pop_front(); br_pos = 0;
                    }
                } else if (!t->wbm_cyc && !t->wbm_ack) {
                    uint32_t a = j.addr + (uint32_t)br_pos;
                    t->wbm_cyc = 1; t->wbm_we = j.write; t->wbm_adr = 0x10000000u | ((a >> 2) & 0x1FFFFFFu);
                    t->wbm_dat_w = j.write ? be32(slots[j.slot].data, j.off + br_pos) : 0;
                    if (j.slot == 0 && (a & 0xFFFFF) == 0) { printf("[bridge: at %u MiB]\n", a >> 20); fflush(stdout); }
                }
            } else if (sys >= load_at && !t->host_reset_n && !t->wbm_cyc) {
                static int settle;                  // (the last word acknowledged is in the chip a few cycles on)
                if (++settle == 64) {
                    if (verify_on && !noload) bad += verify("after the load", false);
                    t->host_reset_n = 1; t->host_loaded = !noload;
                }
            }
            // PAD="from-to:keys[/joy],...": controller 1's keys (hex, the Pocket's: 0010 A, 0020 B,
            // 8000 Start, 0001/2/4/8 the D-pad; KEY_TYPE in the top nibble: 30000000 a dock pad with
            // analog) held over those fields, and optionally its four stick bytes (hex: LX, LY, RX, RY from
            // the low byte up); PAD2, PAD3, PAD4 the same for the dock's other pads (no keys, no pad)
            {
                struct Seg { long a, b; unsigned k, j; };
                static std::vector<Seg> pad[4];
                const bool pad_load = o_pad_load;
                if (pad_load) {
                    o_pad_load = false;
                    for (int c = 0; c < 4; c++) pad[c].clear();
                    static const char *const names[4] = { "PAD", "PAD2", "PAD3", "PAD4" };
                    for (int c = 0; c < 4; c++) if (const char *e = getenv(names[c])) {
                        long a, b; unsigned k, j; int n;
                        while (*e) {
                            if (sscanf(e, "%ld-%ld:%x%n", &a, &b, &k, &n) != 3) break;
                            e += n; j = 0;
                            if (*e == '/') { if (sscanf(e + 1, "%x%n", &j, &n) != 1) break; e += 1 + n; }
                            pad[c].push_back({a, b, k, j});
                            if (*e == ',') e++;
                        }
                    }
                }
                unsigned keys[4] = { 0, 0, 0, 0 }, joy[4] = { 0, 0, 0, 0 };
                for (int c = 0; c < 4; c++) for (auto &q : pad[c]) if (frames >= q.a && frames <= q.b) { keys[c] |= q.k; joy[c] = q.j; }
                // PAD_FILE=path: a file with one hex word (the keys) re-read once per frame -- interactive driving
                static long pf_frame = -1; static unsigned pf_keys;
                if (const char *pf = getenv("PAD_FILE")) {
                    if (frames != pf_frame) { pf_frame = frames; if (FILE *f = fopen(pf, "r")) { unsigned v; if (fscanf(f, "%x", &v) == 1) { if (v != pf_keys) printf("[pad %ld: %x]\n", frames, v); pf_keys = v; } fclose(f); } }
                    keys[0] |= pf_keys;
                }
                // PAD_MASH=from,period,hold,keys: controller 1's keys (hex) held `hold` fields in every `period`, from the field `from` on
                // (PAD_MASH_TO=n: up to the field n)
                { static long mf = -1, mp, mh, mto; static unsigned mk;
                  if (pad_load) mf = -1;
                  if (mf == -1) { mf = -2; if (const char *e = getenv("PAD_MASH")) { if (sscanf(e, "%ld,%ld,%ld,%x", &mf, &mp, &mh, &mk) != 4) mf = -2; } mto = getenv("PAD_MASH_TO") ? atol(getenv("PAD_MASH_TO")) : -1; }
                  if (mf >= 0 && frames >= mf && (mto < 0 || frames <= mto) && (frames - mf) % mp < mh) keys[0] |= mk; }
                t->cont1_key = keys[0]; t->cont2_key = keys[1]; t->cont3_key = keys[2]; t->cont4_key = keys[3];
                t->cont1_joy = joy[0]; t->cont2_joy = joy[1]; t->cont3_joy = joy[2]; t->cont4_joy = joy[3];
            }
        }
        {   // clk_vid: its period in clk_sys2x cycles -- 5 (25 MHz: sys / 2.5); VID_DIV2=10 the 12.5 MHz of old
            static const int vdiv2 = getenv("VID_DIV2") ? atoi(getenv("VID_DIV2")) : 5;
            if (++vid_phase == vdiv2) vid_phase = 0;
            t->clk_vid = vid_phase < (vdiv2 + 1) / 2;
        }
        t->eval();
        { static const bool vga_on = getenv("VGA") != nullptr; if (vga_on && vid_phase == 0) { g_sys = sys; vga_sample(outdir, every); } }
        // VITRACE=1: the swap's timing -- the field's pulse (mon_field: the VI interrupt), the scan-out's latch of its base (the
        // frame boundary) and the runtime's writes of the base, in sys cycles from the last field pulse
        if (sys_edge) {
            static const bool vit = getenv("VITRACE") != nullptr;
            static long f0 = -1; static uint32_t pb = 0; static int pf = 0, pl = 0;
            if (vit) {
                int fpl = t->rootp->soc_sim_top__DOT__soc__DOT__mon_field, lat = t->rootp->soc_sim_top__DOT__soc__DOT__video__DOT__fb_sys;
                if (fpl && !pf) { f0 = sys; printf("[vi %ld: field pulse, base %08x]\n", sys, t->fb_base); }
                // (MIRLO_VI: the VI's V_INTR interrupt, with the line counter then, and the scan-out's latch of the base)
                { static int pi = 0, pll = 0; static long li = -1;
                  int irq = t->rootp->soc_sim_top__DOT__soc__DOT__vi_irq, ll = t->rootp->soc_sim_top__DOT__soc__DOT__video__DOT__fb_sys;
                  if (ll && !pll) li = sys;
                  if (irq && !pi) printf("[vi %ld: V_INTR irq, line %u, %ld cycles after the last latch, base %08x]\n", sys, (unsigned)t->rootp->soc_sim_top__DOT__soc__DOT__vi__DOT__nl, li < 0 ? -1L : sys - li, t->fb_base);
                  pi = irq; pll = ll; }
                { static int pon = -1; if (t->fb_on != pon) { printf("[vi %ld: fb_on %d -> %d]\n", sys, pon, (int)t->fb_on); pon = t->fb_on; } }
                if (lat && !pl) printf("[vi %ld: latch (+%ld after the field pulse), base %08x]\n", sys, sys - f0, t->fb_base);
                if (t->fb_base != pb) printf("[vi %ld: fb_base %08x -> %08x (+%ld after the field pulse)]\n", sys, pb, t->fb_base, sys - f0);
                if (t->tr_exc && f0 >= 0 && sys - f0 < 40000) printf("[vi %ld: exception %u at %08x (+%ld after the field pulse)]\n", sys, t->tr_exc_code, t->tr_exc_pc, sys - f0);
                pf = fpl; pl = lat; pb = t->fb_base;
            }
            // TEAR=1: a tear detector -- the displayed buffer's rows at the scan-out's latch, and, as the scan-out's reads pass each
            // row, whether it still has what it had (a row that changed between the latch and its being read is a mix of two frames
            // on the screen). One line per scan frame: its base, the rows changed, the first, and when the first was
            static const bool tear = getenv("TEAR") != nullptr;
            if (tear) {
                static bool have; static uint32_t w0, rw = 160; static int rd, nrows = 240, torn, first = -1, lat_n; static long lat_sys, first_sys; static std::vector<uint32_t> h;
                static uint32_t pv;
                auto rowhash = [&](int r) { uint32_t x = 2166136261u; for (uint32_t i = 0; i < rw; i++) x = (x ^ rd_word(w0 + (uint32_t)r * rw + i)) * 16777619u; return x; };
                auto finish = [&](void) {
                    if (!have) return;
                    int rp = nrows;
                    for (; rd < rp; rd++) if (rowhash(rd) != h[rd]) { if (!torn++) { first = rd; first_sys = sys; } }
                    printf("[tear frame %d: base %08x, rows changed under the scan %d of %d%s, first row %d at +%ld cycles from the latch]\n", lat_n, w0 * 4, torn, nrows, torn ? " TORN" : "", first, torn ? first_sys - lat_sys : 0L);
                };
                int latp = t->rootp->soc_sim_top__DOT__soc__DOT__video__DOT__fb_sys;
                static int pl2;
                if (latp && !pl2) {
                    finish();
                    have = t->fb_on && t->fb_width < 512; w0 = (t->fb_base & 0x3FFFFFFu) >> 2; rw = 160; nrows = 240; rd = 0; torn = 0; first = -1; lat_sys = sys; lat_n++;
                    if (have) { h.assign(nrows, 0); for (int r = 0; r < nrows; r++) h[r] = rowhash(r); }
                }
                pl2 = latp;
                uint32_t vc = t->rootp->soc_sim_top__DOT__soc__DOT__video__DOT__vcount;
                if (have && vc != pv) {
                    pv = vc;
                    uint32_t ra = t->rootp->soc_sim_top__DOT__soc__DOT__video__DOT__raddr;
                    int rp = (int)(((ra - w0) & 0xFFFFFFu) / rw); if (rp > nrows) rp = nrows;
                    for (; rd < rp; rd++) if (rowhash(rd) != h[rd]) { if (!torn++) { first = rd; first_sys = sys; } }
                }
            }
        }
        chip_step();
        uint16_t v = 0;
        for (int i = 0; i < 8; i++) if (pend_c[i] == cyc) { v = pend_v[i]; pend_c[i] = -1; }
        t->dq_in = v;
        t->eval();
        cpu_to(cyc * 2L * TB_CPU_N + TB_CPU_N);
        t->clk_sys2x = 0; if (!sys_edge) t->clk_sys = 0;
        t->eval();
        if (!sys_edge) {
            static long ut = getenv("UTRACE") ? atol(getenv("UTRACE")) : 0;
            if (ut && t->uart_we) { ut--; printf("[u %ld pc %08x xreq %d xaddr %08x byte %02x]\n", sys, t->cpu_pc, t->x_req, t->x_addr, t->uart_byte); }
            if (t->uart_we) {
                if (t->uart_byte == 4) done = true;
                else { putchar(t->uart_byte); if (t->uart_byte == '\n') fflush(stdout); }
            }
            if (t->file_size_wr) {
                slots[t->br_slot & 7].size = t->file_size_new;
                printf("[bridge: slot %u's file size set to %u]\n", t->br_slot, t->file_size_new);
            }
            if (t->audio_wr) {
                audio_words++;
                // AUDIO_RAW=<file>: every word the SoC pushes to the APF audio
                // FIFO ([31:16] left, [15:0] right, 48 kHz), as written
                static FILE *araw = getenv("AUDIO_RAW") ? fopen(getenv("AUDIO_RAW"), "wb") : nullptr;
                if (araw) { uint32_t w = t->audio_out; fwrite(&w, 4, 1, araw); if ((audio_words & 4095) == 0) fflush(araw); }
            }
            // AILOG=<file> (MIRLO_AI, rtl/soc/rcp_ai.sv): the AI's own events, one a line: F <word> a frame the reader fetched,
            // S <step> the runtime's resampler step, O <word> <silence> a word the AI wrote to the FIFO, I its interrupt,
            // R the reader started over (dry for a while), W <addr> <value> a game's write to an AI register (sys cycle first)
            {
                if (o_ailog_load) { o_ailog_load = false; o_ailog = getenv("AILOG") ? fopen(getenv("AILOG"), "w") : nullptr; }
                FILE *const al = o_ailog;
                static int prst = 0;
                if (al) {
                    auto *r = t->rootp;
                    if (r->soc_sim_top__DOT__soc__DOT__ai_ack) fprintf(al, "F %08x\n", r->soc_sim_top__DOT__soc__DOT__pa_dat_r);
                    if (r->soc_sim_top__DOT__soc__DOT__ai_step_we) fprintf(al, "S %u\n", (unsigned)r->soc_sim_top__DOT__soc__DOT__ai_step_wd);
                    if (r->soc_sim_top__DOT__soc__DOT__ai_fw) fprintf(al, "O %08x %d\n", t->audio_out, (int)r->soc_sim_top__DOT__soc__DOT__ai__DOT__sil);
                    if (r->soc_sim_top__DOT__soc__DOT__ai_irq) fprintf(al, "I\n");
                    int hr = r->soc_sim_top__DOT__soc__DOT__ai__DOT__have_rst;
                    if (hr != prst) { fprintf(al, "R\n"); prst = hr; }
                    if (t->x_ack && t->x_we && ((t->x_addr >> 20) & 0x1FF) == 0x045)
                        fprintf(al, "W %ld %02x %08x\n", sys, t->x_addr & 0xFF, t->x_wdata);
                    // B <addr> <bytes> <words...>: the buffer as it is in the RDRAM when its length is written (the hardware starts or
                    // queues it: tools/ai_check.py --snap compares them with the frames the reader fetched)
                    {
                        static uint32_t baddr;
                        if (t->x_ack && t->x_we && ((t->x_addr >> 20) & 0x1FF) == 0x045) {
                            uint32_t reg = t->x_addr & 0x1C;
                            if (reg == 0x00) baddr = t->x_wdata & 0x7FFFF8u;
                            else if (reg == 0x04 && (t->x_wdata & 0x3FFF8u)) {
                                uint32_t n = t->x_wdata & 0x3FFF8u;
                                fprintf(al, "B %06x %u", baddr, n);
                                for (uint32_t i = 0; i < n; i += 4) fprintf(al, " %08x", rd_word((0x03800000u + baddr + i) >> 2));
                                fprintf(al, "\n");
                            }
                        }
                    }
                }
            }
            // the Pocket's bridge: slot 0 is the program's file; a read lands in SDRAM
            // at ~40 MB/s (the APF bridge's rate), then the completion toggles high
            if (t->br_req_read) {
                uint32_t off = t->br_offset, len = t->br_length, a = t->br_addr;
                if (t->br_slot == 0 && off + len <= img.size() && (a & 3) == 0 && (len & 3) == 0)
                    for (uint32_t i = 0; i < len; i += 4)
                        wr_word(((a & 0x3FFFFFF) >> 2) + i / 4, (uint32_t)img[off + i] << 24 | img[off + i + 1] << 16 |
                                img[off + i + 2] << 8 | (uint32_t)img[off + i + 3]);
                else printf("[bridge: read slot %u off %u len %u -> %08x not modelled]\n", t->br_slot, off, len, a);
                t->br_complete = 0;
                br_done_at = sys + 200 + len * (double)TB_SYS_HZ / 40e6;
            }
            if (br_done_at >= 0 && sys >= br_done_at) { t->br_complete = 1; br_done_at = -1; }
            // AIDUMP=<file>: the buffers the game gives the AI (n64rt_rsp.c's marks, a port
            // built with RSPDUMP=1), one after the other as the N64's bytes
            if (t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 28) == 0xAu) {
                static const char *ad = getenv("AIDUMP");
                static uint32_t aaddr;
                if ((t->x_wdata >> 24) == 0xA1u) aaddr = t->x_wdata & 0x7FFFFFu;
                else if ((t->x_wdata >> 24) == 0xA3u || (t->x_wdata >> 24) == 0xA4u) {
                    rs_type = (t->x_wdata >> 24) == 0xA3u ? (t->x_wdata & 3u) : 0u;
                    if (ad) printf("[%.3f ms: task type %u %s]\n", sys / TB_SYS_KHZ, t->x_wdata & 0xFFu,
                                   (t->x_wdata >> 24) == 0xA3u ? "starts" : (t->x_wdata & 0x100u) ? "yielded" : "ended");
                }
                else if (ad) {
                    // <file>: each buffer as it is handed over; <file>.late: the same buffer
                    // again when the next one is (had it still been written to?)
                    static FILE *af = fopen(ad, "wb");
                    static FILE *al = fopen((std::string(ad) + ".late").c_str(), "wb");
                    static uint32_t paddr, pn;
                    uint32_t n = t->x_wdata & 0xFFFFFFu;
                    auto put = [&](FILE *f, uint32_t a, uint32_t len) {
                        for (uint32_t i = 0; f && i < len; i += 4) {
                            uint32_t w = rd_word((0x03800000u + a + i) >> 2);
                            uint8_t b[4] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
                            fwrite(b, 1, 4, f);
                        }
                        if (f) fflush(f);
                    };
                    put(af, aaddr, n);
                    put(al, paddr, pn);
                    paddr = aaddr; pn = n;
                    printf("[%.3f ms: ai buffer %06x, %u bytes]\n", sys / TB_SYS_KHZ, aaddr, n);
                }
            }
            // RSPDUMP=<dir> [RSPDUMP_MAX=n] [RSPDUMP_EVERY=k] [RSPDUMP_FROM=n] [RSPDUMP_FRAME_FROM=a]: on n64rt's mark (CTRL.SCRATCH =
            // 0xD000_0000 | the OSTask's RDRAM address, N64RT_RSPDUMP) the 8 MiB
            // RDRAM (SDRAM 0x4380_0000, as the N64's bytes) with the task's
            // address in front: sim/rsp runs it on the RSP reference model
            if (t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 28) == 0xDu) {
                static const char *rd = getenv("RSPDUMP");
                static long rmax = getenv("RSPDUMP_MAX") ? atol(getenv("RSPDUMP_MAX")) : 16;
                static long rn[4], seen[4];
                static long revery = getenv("RSPDUMP_EVERY") ? atol(getenv("RSPDUMP_EVERY")) : 1;
                uint32_t ta = t->x_wdata & 0x7FFFFFu;
                uint32_t ty = rd_word((0x03800000u + ta) >> 2);
                static long rfrom = getenv("RSPDUMP_FROM") ? atol(getenv("RSPDUMP_FROM")) : 0;   // RSPDUMP_FROM=n: from the n-th task of its kind
                static long ffrom = getenv("RSPDUMP_FRAME_FROM") ? atol(getenv("RSPDUMP_FRAME_FROM")) : 0;   // RSPDUMP_FRAME_FROM=a: from the VI frame a
                // RSPDUMP_CTL=path: "from max" (the VI frame to start at, how many gfx tasks), re-read once per frame;
                // a new pair restarts the count
                if (const char *cf = getenv("RSPDUMP_CTL")) {
                    static long cf_frame = -1, la = -1, lb = -1;
                    if (frames != cf_frame) {
                        cf_frame = frames;
                        if (FILE *f = fopen(cf, "r")) { long a, b; if (fscanf(f, "%ld %ld", &a, &b) == 2 && (a != la || b != lb)) { la = ffrom = a; lb = rmax = b; rn[1] = 0; } fclose(f); }
                    }
                }
                if (rd && ty < 4 && seen[ty]++ >= rfrom && (seen[ty] - 1 - rfrom) % revery == 0 && rn[ty] < rmax && frames >= ffrom) {
                    char path[512];
                    snprintf(path, sizeof path, "%s/task_%s_%03ld.bin", rd, ty == 1 ? "gfx" : ty == 2 ? "audio" : "other", rn[ty]++);
                    if (FILE *f = fopen(path, "wb")) {
                        uint8_t hdr[16] = { 'R', 'S', 'P', 'T', (uint8_t)(ta >> 24), (uint8_t)(ta >> 16), (uint8_t)(ta >> 8), (uint8_t)ta };
                        fwrite(hdr, 1, 16, f);
                        std::vector<uint8_t> ram(8u << 20);
                        for (uint32_t i = 0; i < (8u << 20) / 4; i++) {
                            uint32_t w = rd_word((0x03800000u >> 2) + i);
                            ram[4 * i] = w >> 24; ram[4 * i + 1] = w >> 16; ram[4 * i + 2] = w >> 8; ram[4 * i + 3] = w;
                        }
                        fwrite(ram.data(), 1, ram.size(), f);
                        fclose(f);
                        printf("[rspdump %s, task at %06x]\n", path, ta);
                    }
                }
            }
            {   // RSPDUMP with the game's own libultra: the RSP leaves halt with a gfx task in DMEM[0xFC0..]; its OSTask goes
                // to the dump's last 64 bytes (the header's address), the RDRAM as it is
                static int phalt = 1;
                int halt = t->audio_state & 1;
                const char *rd2 = getenv("RSPDUMP");
                static long n2;
                if (rd2 && phalt && !halt) {
                    auto &dm = t->rootp->soc_sim_top__DOT__soc__DOT__rsp__DOT__dmem__DOT__mem;
                    uint32_t task[16];
                    for (int i = 0; i < 16; i++) task[i] = dm[0x3F0 + i];
                    // (the CTL file as above)
                    static long cf_frame = -1, la = -1, lb = -1, ffrom2 = getenv("RSPDUMP_FRAME_FROM") ? atol(getenv("RSPDUMP_FRAME_FROM")) : 0, rmax2 = getenv("RSPDUMP_MAX") ? atol(getenv("RSPDUMP_MAX")) : 16;
                    if (const char *cf = getenv("RSPDUMP_CTL")) if (frames != cf_frame) {
                        cf_frame = frames;
                        if (FILE *f = fopen(cf, "r")) { long a, b; if (fscanf(f, "%ld %ld", &a, &b) == 2 && (a != la || b != lb)) { la = ffrom2 = a; lb = rmax2 = b; n2 = 0; } fclose(f); }
                    }
                    if (getenv("RSPDUMP_LOG")) printf("[rsp start at frame %ld: task type %u flags %08x]\n", frames, task[0], task[1]);
                    if (task[0] == 1 && !(task[1] & 1) && frames >= ffrom2 && n2 < rmax2) {
                        char path[512];
                        snprintf(path, sizeof path, "%s/task_gfx_%03ld.bin", rd2, n2++);
                        if (FILE *f = fopen(path, "wb")) {
                            uint32_t ta = (8u << 20) - 64;
                            uint8_t hdr[16] = { 'R', 'S', 'P', 'T', (uint8_t)(ta >> 24), (uint8_t)(ta >> 16), (uint8_t)(ta >> 8), (uint8_t)ta };
                            fwrite(hdr, 1, 16, f);
                            std::vector<uint8_t> ram(8u << 20);
                            for (uint32_t i = 0; i < (8u << 20) / 4; i++) {
                                uint32_t w = rd_word((0x03800000u >> 2) + i);
                                ram[4 * i] = w >> 24; ram[4 * i + 1] = w >> 16; ram[4 * i + 2] = w >> 8; ram[4 * i + 3] = w;
                            }
                            for (int i = 0; i < 16; i++) { uint32_t w = task[i]; uint8_t *q = &ram[ta + 4 * i]; q[0] = w >> 24; q[1] = w >> 16; q[2] = w >> 8; q[3] = w; }
                            fwrite(ram.data(), 1, ram.size(), f);
                            fclose(f);
                            printf("[rspdump %s at frame %ld, task type %u]\n", path, frames, task[0]);
                        }
                    }
                }
                phalt = halt;
            }
            long &xt = o_xtrace;
            static const bool xvi = getenv("XTRACE_VI") != nullptr;   // XTRACE_VI=1: only the VI's registers (0x044xxxxx), with the frame and the cycle
            if (xt && t->x_ack && (!xvi || (t->x_addr >> 20) == 0x044u)) { xt--; if (xvi) printf("[x f%ld c%ld %s %08x = %08x]\n", frames, sys, t->x_we ? "W" : "R", t->x_addr, t->x_we ? t->x_wdata : t->x_rdata); else printf("[x %s %08x = %08x]\n", t->x_we ? "W" : "R", t->x_addr, t->x_we ? t->x_wdata : t->x_rdata); }
            if (t->vblank && !pvb) {
                // FBLOG=1: each frame boundary where the scan-out's base is not the one of the last (a frame the game showed): the
                // frame count and the time (sys cycles) -- the game's own rate (30 / 25 a second) is these, counted
                static const bool fblog = getenv("FBLOG") != nullptr;
                static uint32_t last_b; static long last_f;
                if (fblog && t->fb_base != last_b) { printf("[fb frame %ld sys %ld base %08x, %ld frames since the last]\n", frames, sys, t->fb_base, frames - last_f); last_b = t->fb_base; last_f = frames; }
                static const bool vgat = getenv("VGAT") != nullptr;
                if (vgat && frames % every == 0)
                    printf("[vi frame %ld: base %08x on %d width %u tall %d window %u/%u/%u; VI: H_START 0 %d black %d hw %d origin %06x thr %05x WIDTH %u tv %s (V_SYNC %s)]\n",
                           frames, t->fb_base, t->fb_on, t->fb_width, t->fb_tall, t->fb_px0, t->fb_y0, t->fb_rows,
                           t->vi_dbg0 >> 31, (t->vi_dbg0 >> 30) & 1, (t->vi_dbg0 >> 29) & 1, t->vi_dbg0 & 0x7FFFFF, (t->vi_dbg1 >> 12) & 0x1FFFF, t->vi_dbg1 & 0xFFF,
                           (t->vi_dbg0 >> 28) & 1 ? "PAL" : "NTSC", (t->vi_dbg0 >> 27) & 1 ? ((t->vi_dbg0 >> 26) & 1 ? "625" : "525") : "-");
                if (!outdir.empty() && frames % every == 0) dump_frame(outdir, frames);
                frames++;
                // CKPT=<dir>,<n>: every n fields a copy of the run is left waiting (a process forked from it, asleep). A file
                // <dir>/go_<field> starts a run from that copy, as many times as wanted: its lines are NAME=value -- LOG (its
                // output), OUT and EVERY (-o, -e), MCYCLES (how many more million cycles), and any of the options read again
                // then (CPUTRACE, counted from there, EXCSTAT, CPUSTAT, XTRACE, PCTOP, PCPROF, PAD.., PAD_MASH, AILOG and RSPLOG: the run's own file, none
                // unless named) or at each use (PAD_FILE, RAMDUMP, RSPDUMP and its RSPDUMP_CTL ...).
                // <dir>/quit ends the copies.
                if (const char *ck = getenv("CKPT")) {
                    static const std::string cdir = std::string(ck).substr(0, std::string(ck).find(','));
                    static const long cn = strchr(ck, ',') ? atol(strchr(ck, ',') + 1) : 300;
                    if (cn > 0 && frames % cn == 0) {
                        fflush(nullptr);
                        if (fork() == 0) {
                            signal(SIGCHLD, SIG_IGN);
                            std::string go = cdir + "/go_" + std::to_string(frames), q = cdir + "/quit";
                            { FILE *f = fopen((cdir + "/ckpt_" + std::to_string(frames)).c_str(), "w"); if (f) { fprintf(f, "%d %ld\n", (int)getpid(), sys); fclose(f); } }
                            struct stat sb;
                            for (;;) {
                                if (stat(q.c_str(), &sb) == 0) _exit(0);
                                if (stat(go.c_str(), &sb) == 0) {
                                    std::string run = go + ".run";
                                    rename(go.c_str(), run.c_str());
                                    if (fork() == 0) {
                                        unsetenv("CKPT"); unsetenv("AILOG"); unsetenv("RSPLOG");
                                        long more = 30;
                                        if (FILE *f = fopen(run.c_str(), "r")) {
                                            char ln[1024];
                                            while (fgets(ln, sizeof ln, f)) {
                                                ln[strcspn(ln, "\r\n")] = 0;
                                                char *eq = strchr(ln, '='); if (!eq) continue;
                                                *eq = 0; const char *v = eq + 1;
                                                if (!strcmp(ln, "LOG")) { if (!freopen(v, "w", stdout)) _exit(3); }
                                                else if (!strcmp(ln, "OUT")) outdir = v;
                                                else if (!strcmp(ln, "EVERY")) every = atol(v);
                                                else if (!strcmp(ln, "MCYCLES")) more = atol(v);
                                                else setenv(ln, v, 1);
                                            }
                                            fclose(f);
                                        }
                                        max_sys = sys + more * 1000000;
                                        ck_run = true;
                                        opts_load();
                                        printf("[run from the checkpoint at field %ld, sys %ld]\n", frames, sys); fflush(stdout);
                                        break;
                                    }
                                }
                                sleep(1);
                            }
                        }
                    }
                }
                // PCTOP=n: every n fields the game CPU's busiest 32-byte blocks of code (pc sampled every 64 sys cycles), then cleared
                const long pctop = o_pctop;
                if (pctop && frames % pctop == 0) {
                    std::vector<std::pair<uint32_t, uint32_t>> v(pct_hist.begin(), pct_hist.end());
                    std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    uint64_t tot = 0; for (auto &kv : v) tot += kv.second;
                    printf("[pctop fields %ld: %llu samples:", frames, (unsigned long long)tot);
                    for (size_t i = 0; i < v.size() && i < 8; i++) printf(" %08x:%u", v[i].first << 5, v[i].second);
                    printf("]\n"); fflush(stdout);
                    pct_hist.clear();
                }
            }
            pvb = t->vblank;
            if (o_pctop && (sys & 63) == 0) pct_hist[t->cpu_pc >> 5]++;
            // the geom core's busy share: anything but its "waiting for a list"
            // heartbeat (lang/c/geom/main.c HB 0xB0000040), per EVERY window
            static long g_busy, g_win;
            g_win++; if (t->geom_msg != 0xB0000040u) g_busy++;
            long ev = getenv("EVERY") ? atol(getenv("EVERY")) : 0;
            if (ev && sys % ev == 0) {
                printf("[%ld M cycles, %lds] frame %ld cpu pc %08x geom pc %08x hb %08x audio pc %08x st %08x words %ld mrdp syncs %u geom busy %.1f%%\n",
                       sys / 1000000, (long)(time(nullptr) - t0), frames, t->cpu_pc, t->geom_pc, t->geom_msg,
                       t->audio_pc, t->audio_state, audio_words, t->mrdp_sync, g_win ? 100.0 * g_busy / g_win : 0.0);
                g_busy = g_win = 0;
                fflush(stdout);
            }
            // PCPROF=file [PCPROF_FROM=cycles]: the game CPU's PC every 256 cycles
            const bool pcprof = o_pcprof; const long pcprof_from = o_pcprof_from;
            if (pcprof && sys >= pcprof_from && (sys & 255) == 0) pc_hist[t->cpu_pc]++;
            // GPCPROF=file: the geom core's PC likewise (from PCPROF_FROM), and
            // how many of the samples it was waiting for a list (lang/c/geom/main.c HB)
            static const char *gpcprof = getenv("GPCPROF");
            if (gpcprof && sys >= pcprof_from && (sys & 255) == 0) {
                if (t->geom_msg == 0xB0000040u) gpc_idle++; else gpc_hist[t->geom_pc]++;
            }
            // MEMSTAT=1: the SDRAM's use -- the words each port had, the rows
            // opened (and of them, those in a bank nothing had used for 8
            // cycles: a row another request could have had opened behind the
            // one under way), the cycles the controller kept a request for a
            // row or for the bus to turn, the RDP's and the RSP's time. A line
            // a second, or between a program's marks (0xA5 / 0xA6 to the
            // scratch register: rsp_replay's runs).
            static const bool mstat = getenv("MEMSTAT") != nullptr;
            if (mstat) {
                static long mp[6], mact, mact_idle, mwords, mwait, mrow, mturn, mrdp, mrsp, mn, last_use[4], last_dir = -1, mswitch;
                static bool marks;
                unsigned m = t->mem_stat;
                unsigned c = (m >> 6) & 7, ba = (m >> 9) & 3;
                bool mark0 = t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 24) == 0xA5u;
                bool mark1 = t->x_ack && t->x_we && t->x_addr == 0xF0000504u && (t->x_wdata >> 24) == 0xA6u;
                bool out = marks ? mark1 : sys % TB_SYS_HZ == TB_SYS_HZ - 1;
                if (mark0) marks = true;
                for (int i = 0; i < 6; i++) if (m & (1u << i)) mp[i]++;
                if (c == 3) { mact++; if (sys - last_use[ba] >= 8) mact_idle++; }
                if (c == 5 || c == 4) { mwords++; last_use[ba] = sys; if (last_dir >= 0 && last_dir != (long)c) mswitch++; last_dir = c; }
                if (m & 0x2000) mwait++;
                if ((m & 0x1000) && !(m & 0x800)) { if (c == 7 && t->mem_hit) mturn++; else mrow++; }
                if (m & 0x4000) mrdp++;
                if (m & 0x8000) mrsp++;
                mn++;
                if (out) {
                    printf("[memstat %.1f ms: words scan-out %ld, CPU %ld, RDP %ld, RSP %ld, bridge %ld, command fetch %ld; %ld rows opened (%ld in an idle bank); "
                           "a word moved %.0f %% of the cycles, the controller on a row %.0f %%, on the bus's turn %.0f %% (%ld turns), a port asking %.0f %%; "
                           "RDP at work %.1f ms, RSP running %.1f ms]\n", mn / TB_SYS_KHZ, mp[0], mp[1], mp[2], mp[3], mp[4], mp[5],
                           mact, mact_idle, 100.0 * mwords / mn, 100.0 * mrow / mn, 100.0 * mturn / mn, mswitch, 100.0 * mwait / mn, mrdp / TB_SYS_KHZ, mrsp / TB_SYS_KHZ);
                    fflush(stdout);
                }
                if (out || mark0) { for (int i = 0; i < 6; i++) mp[i] = 0; mact = mact_idle = mwords = mwait = mrow = mturn = mrdp = mrsp = mn = mswitch = 0; }
            }
            // CPUSTAT=1 [CPUSTAT_IDLE=lo,hi]: where the game CPU's cycles go, a
            // line a second or between a program's marks (as MEMSTAT's): the
            // instructions retired, and each cycle by what held it -- M waiting
            // (a line for the D-cache, the slow path: uncached, a device; the
            // FPU, a multiply or divide, the write buffer full, a translation),
            // F without an instruction (a line for the I-cache, an uncached
            // fetch, the cycle after a refill or a flush), D waiting on a
            // hazard -- or none. CPUSTAT_IDLE: a range of pc (the game's idle
            // loop) whose cycles are counted apart; what is left of a field
            // is its CPU time (CPUSTAT=2: a line for every field).
            // RCPSTAT=n [RCPSTAT_IDLE=lo,hi]: every n fields, what the RCP and the CPU did in them -- the time the RSP ran, the
            // time the RDP was not idle (a command under way or waiting), the RDP's full syncs (a frame each), and the CPU's
            // time outside the game's idle loop (the range of its pc)
            if (o_rcpstat) {
                static long rsp_run, rdp_busy, cpu_idle, n_cyc, f_last = -1; static uint32_t sync_last;
                n_cyc++;
                if (t->rsp_stat & 1) rsp_run++;
                if (!t->rootp->soc_sim_top__DOT__soc__DOT__mrdp_idle) rdp_busy++;
                if (o_rcp_ihi && t->cpu_pc >= o_rcp_ilo && t->cpu_pc < o_rcp_ihi) cpu_idle++;
                if (frames != f_last && frames % o_rcpstat == 0) {
                    if (f_last >= 0)
                        printf("[rcpstat field %ld: %.2f ms; RSP running %.2f ms, RDP busy %.2f ms, %u full syncs, CPU busy %.2f ms]\n", frames, n_cyc / TB_SYS_KHZ,
                               rsp_run / TB_SYS_KHZ, rdp_busy / TB_SYS_KHZ, (unsigned)(t->mrdp_sync - sync_last), (n_cyc - cpu_idle) / TB_SYS_KHZ);
                    rsp_run = rdp_busy = cpu_idle = n_cyc = 0; sync_last = t->mrdp_sync;
                }
                f_last = frames;
            }
            // RSPLOG=<file>: the RSP's runs, a line each as it halts: the sys cycle it left halt at, the task's type and flags (the
            // OSTask in DMEM 0xFC0: 1 gfx, 2 audio; flags bit 0: a task taken up again after a yield), the cycles it ran, SP_STATUS's
            // signals as it stopped (libultra's: bit 0 a yield asked for, 1 yielded, 2 task done), the instructions, the cycles
            // with a DMA under way, with a run of RDP commands waiting behind the one being fetched, and -- a yield was asked
            // for while it ran -- the cycles from the asking to the halt (else -1). (A run from a checkpoint: its own file.)
            {
                static FILE *rl; static const char *rl_name;
                const char *rn = getenv("RSPLOG");
                if (rn != rl_name) { rl_name = rn; rl = rn ? fopen(rn, "w") : nullptr; }
                if (rl) {
                    static int was_halt = 1, was_sig0; static long t_start, n_ins, n_dma, n_wait, t_yield = -1; static uint32_t ty, fl;
                    const int halt = t->audio_state & 1, sig0 = t->rsp_sig & 1;
                    if (was_halt && !halt) {
                        auto &dm = t->rootp->soc_sim_top__DOT__soc__DOT__rsp__DOT__dmem__DOT__mem;
                        t_start = sys; ty = dm[0x3F0]; fl = dm[0x3F1]; n_ins = n_dma = n_wait = 0; t_yield = sig0 ? sys : -1;
                    }
                    if (!halt) {
                        if (t->rsp_stat & 8) n_ins++;
                        if (t->rsp_stat & 2) n_dma++;
                        if (t->rsp_stat & 16) n_wait++;
                        if (sig0 && !was_sig0) t_yield = sys;
                    }
                    if (!was_halt && halt) {
                        fprintf(rl, "%ld %u %x %ld %x %ld %ld %ld %ld\n", t_start, ty, fl, sys - t_start, (unsigned)t->rsp_sig, n_ins, n_dma, n_wait,
                                t_yield >= 0 ? sys - t_yield : -1L);
                        static unsigned n_lines; if ((++n_lines & 63) == 0) fflush(rl);
                    }
                    was_halt = halt; was_sig0 = sig0;
                }
            }
            // RSPSTAT=1: where the RSP's time goes, by the task under way (the
            // port's 0xA3 / 0xA4 marks: built with RSPDUMP=1), a line a second
            static const bool rstat = getenv("RSPSTAT") != nullptr;
            if (rstat) {
                unsigned st = t->rsp_stat;
                long *c = rs_cnt[rs_type];
                if (st & 1) { c[0]++; if (st & 2) c[1]++; if (st & 4) c[2]++; if (st & 16) c[4]++; if ((st & 32) && !(st & 2)) c[5]++; }
                if (st & 8) c[3]++;
                if (sys % TB_SYS_HZ == TB_SYS_HZ - 1) {
                    for (int k = 1; k <= 2; k++) {
                        long *d = rs_cnt[k];
                        printf("[rspstat second %ld %s: running %.1f ms, %ld instructions (%.2f cycles each); DMA %.1f ms (its requests waiting %.1f); "
                               "a command run waiting %.1f ms]\n", sys / TB_SYS_HZ, k == 1 ? "gfx" : "audio", d[0] / TB_SYS_KHZ, d[3],
                               d[3] ? (double)d[0] / d[3] : 0.0, d[1] / TB_SYS_KHZ, d[2] / TB_SYS_KHZ, d[4] / TB_SYS_KHZ);
                        for (int i = 0; i < 6; i++) d[i] = 0;
                    }
                    fflush(stdout);
                }
            }
            sys++;
        }
    }
    // RAMDUMP=<file>: the game's 8 MiB of RDRAM (SDRAM 0x4380_0000) as the run ends, the N64's bytes
    if (const char *f = getenv("RAMDUMP")) {
        if (FILE *pf = fopen(f, "wb")) {
            std::vector<uint8_t> ram(8u << 20);
            for (uint32_t a = 0; a < (8u << 20); a += 4) { uint32_t w = rd_word((0x03800000u + a) >> 2); ram[a] = w >> 24; ram[a + 1] = w >> 16; ram[a + 2] = w >> 8; ram[a + 3] = w; }
            fwrite(ram.data(), 1, ram.size(), pf); fclose(pf);
        }
    }
    if (const char *f = getenv("GPCPROF")) {
        FILE *pf = fopen(f, "w");
        if (pf) { fprintf(pf, "idle %u\n", gpc_idle); for (auto &kv : gpc_hist) fprintf(pf, "%08x %u\n", kv.first, kv.second); fclose(pf); }
    }
    if (const char *f = getenv("PCPROF")) {
        FILE *pf = fopen(f, "w");
        if (pf) { for (auto &kv : pc_hist) fprintf(pf, "%08x %u\n", kv.first, kv.second); fclose(pf); }
    }
    fflush(stdout);
    printf("\n%ld sys cycles, %ld frames, %ld SDRAM protocol errors; cpu pc %08x cause %08x epc %08x\n",
           sys, frames, errors, t->cpu_pc, t->cpu_cause, t->cpu_epc);
    if (const char *so = getenv("SLOTS_OUT"))
        for (int k = 1; k < 8; k++) if (!slots[k].back.empty()) {
            std::string path = std::string(so) + "/slot" + std::to_string(k) + ".bin";
            if (FILE *f = fopen(path.c_str(), "wb")) { fwrite(slots[k].back.data(), 1, slots[k].size, f); fclose(f); }
            else perror(path.c_str());
        }
    if (verify_on && !noload) bad += verify("at the end", true);
    if (bad) errors += bad;
    if (const char *d = getenv("DUMP")) {               // DUMP=addr,len: SDRAM words out (hex)
        uint32_t a = strtoul(d, 0, 0), n = strchr(d, ',') ? strtoul(strchr(d, ',') + 1, 0, 0) : 16;
        for (uint32_t i = 0; i < n; i++) printf("%08x: %08x\n", a + 4 * i, rd_word(((a & 0x3FFFFFF) >> 2) + i));
    }
    if (ck_run) { fflush(nullptr); _exit(errors ? 1 : 0); }     // (a forked copy: the model's threads are not there to be joined)
    t->final();
    delete t;
    return errors ? 1 : 0;
}
