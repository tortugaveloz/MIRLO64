// CONMODEL=<n>: a timing model of the console's CPU (a VR4300 at 93.75 MHz), run on the instructions the game CPU
// retires, beside the real thing. Every n fields a line compares the game's own code as this CPU ran it with what the
// model says the console takes for the same instructions:
//   ours    the CPU's cycles, each given to the class of the next instruction to retire: the game's code (KSEG0's
//           window, its mapped pages), the runtime's (the monitor, the helpers -- 0x4000_0000 up), the game's idle loop
//           (a branch to itself and its delay slot)
//   console the model's cycles for the game's instructions: one each, plus the console's interlocks (a load's value
//           used by the next instruction, mfhi / mflo behind a multiply or divide, an FPU operation's), its caches
//           (16 KiB I-cache of 32-byte lines, 8 KiB write-back D-cache of 16-byte lines, both direct mapped, kept
//           here from the addresses: the registers are followed from what each instruction writes), its uncached
//           and device accesses, a likely branch's annulled slot, and an exception's entry; for an instruction the
//           runtime does in the game's place (a device access that traps, a Reserved Instruction), what the
//           console takes for the instruction itself.
// The costs are pcycles and are the model's parameters: CONMODEL_P=name=value,... overrides them (the names are below).
// Instrumentation only; nothing here touches the design.
struct ConParam { const char *name; double v; };
static ConParam con_p[] = {
    {"imiss", 43},      // an I-cache line (8 words) from the RDRAM (measured on consoles: ~43)
    {"dmiss", 41},      // a D-cache line (4 words) from the RDRAM (measured: median 41)
    {"dwb", 0},         // a dirty line written back before the fill (unmeasured: the write buffer drains it behind)
    {"unc_rd", 32},     // an uncached RDRAM word read
    {"unc_wr", 1},      // an uncached store (posted in the write buffer)
    {"rcp_rd", 24},     // a word read of an RCP register (MI VI AI PI RI SI SP DP)
    {"spmem_rd", 28},   // a word read of DMEM / IMEM
    {"rom_rd", 216},    // a word read of the cartridge (the PI's bus)
    {"pif_rd", 2875},    // a word read of PIF RAM
    {"mult", 5}, {"dmult", 8}, {"div", 37}, {"ddiv", 69},   // their latency: what an mfhi / mflo waits for
    {"md_stall", 1},    // 1: the pipeline stalls for the whole multiply / divide as it issues (else only mfhi / mflo wait)
    {"f_add", 3}, {"f_mul", 5}, {"f_muld", 8}, {"f_div", 29}, {"f_divd", 58}, {"f_sqrt", 29}, {"f_sqrtd", 58},
    {"f_cvt", 5}, {"f_mov", 1}, {"f_cmp", 1}, {"f_cvtds", 2},
    {"ld_use", 1},      // a load's value used by the next instruction
    {"likely", 1},      // a likely branch not taken: its slot is annulled, a cycle spent
    {"exc", 5},         // an exception's entry (the pipeline's flush)
    {"eret", 2},
};
static double cp(const char *n) { for (auto &p : con_p) if (!strcmp(p.name, n)) return p.v; return 0; }
static void con_params()
{
    const char *e = getenv("CONMODEL_P");
    if (!e) return;
    std::string s(e);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
        std::string kv = s.substr(i, j - i); size_t eq = kv.find('=');
        if (eq != std::string::npos) for (auto &p : con_p) if (kv.substr(0, eq) == p.name) p.v = atof(kv.c_str() + eq + 1);
        i = j + 1;
    }
}

struct ConModel {
    uint32_t r[32]; bool known[32];
    // the caches: a tag (pa >> 13 / >> 14) per line, valid and dirty
    uint32_t dtag[512]; bool dval[512], ddirty[512];
    uint32_t itag[512]; bool ival[512];
    int ld_dst = -1;            // the last game instruction was a load into this register (-1: not)
    bool ld_fpu = false;        // into an FPU register (ld_dst is its number)
    double md_ready = 0;        // the console's cycle when HI / LO are ready
    double cyc = 0;             // the console's cycles so far (the game's instructions)
    // this window: ours by class, the console's by cause
    enum { O_GAME, O_RT, O_IDLE, O_N };
    long ours[O_N] = {};
    double con[16] = {};        // by cause, below
    long n_game = 0, n_rt = 0, n_exc_mon = 0, n_exc_game = 0, n_imiss = 0, n_dmiss = 0, n_dwb = 0, n_unc = 0, n_dev = 0;
    long fcnt[16] = {}, dcnt[8] = {}, mdcnt[4] = {}, n_lduse = 0, n_likely = 0;   // raw counts, to price again offline
    long pending = 0;           // our cycles since the last retire
    bool exc_pend = false; uint32_t exc_pc = 0; int exc_code = 0;
    long fields = 0, per = 1;
    bool vb_was = false;
    bool active = false;
    // totals over the run
    long t_ours[O_N] = {}; double t_con = 0; long t_fields = 0;
    enum { C_BASE, C_LDUSE, C_MD, C_FPU, C_IMISS, C_DMISS, C_UNC, C_DEV, C_LIKELY, C_EXC, C_EMUL, C_N };
    const char *cname[C_N] = { "base", "load-use", "mult/div", "FPU", "I-miss", "D-miss", "uncached", "device", "likely", "exception", "emulated" };
};
static ConModel cm;

static bool game_pc(uint32_t pc)
{
    uint32_t t = pc >> 28;
    if ((pc >> 23) == 0x100) return true;                   // KSEG0's RDRAM
    return t <= 1 || t == 5 || t == 6 || t == 7 || t == 0xC || t == 0xD || t == 0xE;   // mapped
}
static bool rt_pc(uint32_t pc) { return (pc >> 28) == 4; }
// an instruction word from where the CPU fetched it (KSEG0 / KSEG1 RDRAM; else 0)
static uint32_t con_fetch(uint32_t pc)
{
    if ((pc >> 29) == 4 || (pc >> 29) == 5) { uint32_t pa = pc & 0x7FFFFF; return rd_word(((0x3800000u + pa) >> 2)); }
    return 0;
}
// (the class: 0 uncached store, 1 RDRAM read, 2 RCP register read, 3 DMEM / IMEM, 4 PIF RAM, 5 the cartridge)
static int con_dev_class(uint32_t pa, bool st)
{
    if (st) return 0;
    if (pa < 0x03F00000u) return 1;
    if (pa < 0x04000000u) return 2;
    if (pa < 0x04040000u) return 3;
    if (pa < 0x04900000u) return 2;
    if (pa >= 0x1FC00000u && pa < 0x1FC00800u) return 4;
    return 5;
}
static double con_dev_cost(uint32_t pa, bool st)
{
    static const char *nm[6] = { "unc_wr", "unc_rd", "rcp_rd", "spmem_rd", "pif_rd", "rom_rd" };
    int k = con_dev_class(pa, st);
    cm.dcnt[k]++;
    return cp(nm[k]);
}
static void con_dline(uint32_t pa, bool st)
{
    int i = (pa >> 4) & 511; uint32_t tg = pa >> 13;
    if (cm.dval[i] && cm.dtag[i] == tg) { if (st) cm.ddirty[i] = true; return; }
    double c = cp("dmiss"); cm.n_dmiss++;
    if (cm.dval[i] && cm.ddirty[i]) { c += cp("dwb"); cm.n_dwb++; }
    cm.con[ConModel::C_DMISS] += c; cm.cyc += c;
    cm.dval[i] = true; cm.dtag[i] = tg; cm.ddirty[i] = st;
}
static void con_iline(uint32_t pc)
{
    // (virtually indexed; KSEG0's tag is its physical address, a mapped page's its virtual one here)
    uint32_t pa = (pc >> 29) == 4 ? pc & 0x1FFFFFFF : pc;
    int i = (pc >> 5) & 511; uint32_t tg = pa >> 14;
    if (cm.ival[i] && cm.itag[i] == tg) return;
    cm.ival[i] = true; cm.itag[i] = tg; cm.n_imiss++;
    cm.con[ConModel::C_IMISS] += cp("imiss"); cm.cyc += cp("imiss");
}
// a cache instruction's effect on the model's caches
static void con_cacheop(uint32_t op, uint32_t va)
{
    uint32_t pa = va & 0x1FFFFFFF;
    int ii = (va >> 5) & 511, di = (va >> 4) & 511;
    switch (op) {
    case 0x00: cm.ival[ii] = false; break;                                                   // I index invalidate
    case 0x10: if (cm.ival[ii] && cm.itag[ii] == pa >> 14) cm.ival[ii] = false; break;      // I hit invalidate
    case 0x01:                                                                               // D index writeback invalidate
        if (cm.dval[di] && cm.ddirty[di]) { cm.con[ConModel::C_DMISS] += cp("dwb"); cm.cyc += cp("dwb"); }
        cm.dval[di] = false; break;
    case 0x11: if (cm.dval[di] && cm.dtag[di] == pa >> 13) cm.dval[di] = false; break;      // D hit invalidate
    case 0x15: case 0x19:                                                                    // D hit writeback (invalidate)
        if (cm.dval[di] && cm.dtag[di] == pa >> 13) {
            if (cm.ddirty[di]) { cm.con[ConModel::C_DMISS] += cp("dwb"); cm.cyc += cp("dwb"); cm.ddirty[di] = false; }
            if (op == 0x15) cm.dval[di] = false;
        }
        break;
    case 0x0D: cm.dval[di] = true; cm.dtag[di] = pa >> 13; cm.ddirty[di] = true; break;     // D create dirty exclusive
    case 0x09: cm.dval[di] = false; break;                                                   // D index store tag (TagLo 0 in practice)
    default: break;
    }
}
// an access's cost by the console's view of its address
static void con_access(uint32_t va, bool st, int bytes)
{
    (void)bytes;
    uint32_t seg = va >> 29;
    if (seg == 4) { con_dline(va & 0x1FFFFFFF, st); return; }                 // KSEG0: cached
    if (seg == 5) {                                                           // KSEG1: uncached
        double c = con_dev_cost(va & 0x1FFFFFFF, st);
        bool dev = (va & 0x1FFFFFFF) >= 0x03F00000u;
        cm.con[dev ? ConModel::C_DEV : ConModel::C_UNC] += c - 1; cm.cyc += c - 1;
        if (dev) cm.n_dev++; else cm.n_unc++;
        return;
    }
    con_dline(va, st);                                                        // mapped: cached (C = 3), by its virtual address here
}
static double fpu_cost(uint32_t i)
{
    uint32_t fmt = (i >> 21) & 31, fn = i & 63;
    if (fmt < 0x10) return 1;                                                 // moves, bc1 (fmt 8)
    bool d = fmt == 0x11;
    if (fmt == 0x14 || fmt == 0x15) return cp("f_cvt");                       // cvt from W / L
    switch (fn) {
    case 0x00: case 0x01: return cp("f_add");
    case 0x02: return d ? cp("f_muld") : cp("f_mul");
    case 0x03: return d ? cp("f_divd") : cp("f_div");
    case 0x04: return d ? cp("f_sqrtd") : cp("f_sqrt");
    case 0x05: case 0x06: case 0x07: return cp("f_mov");
    case 0x20: return d ? cp("f_cvtds") : 1;                                  // cvt.s.d / (cvt.s.s)
    case 0x21: return 1;                                                      // cvt.d.s
    default: return fn >= 0x30 ? cp("f_cmp") : cp("f_cvt");                   // compares; round / trunc / ceil / floor / cvt to W / L
    }
}
// the FPU operation's class, for the raw counts: 0 move / bc1, 1 add, 2 mul, 3 mul.d, 4 div, 5 div.d, 6 sqrt, 7 sqrt.d,
// 8 cvt (from W/L, to W/L, round...), 9 abs/neg/mov, 10 compare, 11 cvt.s.d, 12 cvt.d.s
static int fpu_class(uint32_t i)
{
    uint32_t fmt = (i >> 21) & 31, fn = i & 63; bool d = fmt == 0x11;
    if (fmt < 0x10) return 0;
    if (fmt == 0x14 || fmt == 0x15) return 8;
    switch (fn) {
    case 0x00: case 0x01: return 1;
    case 0x02: return d ? 3 : 2;
    case 0x03: return d ? 5 : 4;
    case 0x04: return d ? 7 : 6;
    case 0x05: case 0x06: case 0x07: return 9;
    case 0x20: return 11;
    case 0x21: return 12;
    default: return fn >= 0x30 ? 10 : 8;
    }
}
// the registers an instruction reads (bit mask), for the load-use interlock
static uint32_t con_reads(uint32_t i)
{
    uint32_t op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31;
    switch (op) {
    case 0x00: return (1u << rs) | (1u << rt);
    case 0x01: return 1u << rs;
    case 0x02: case 0x03: return 0;
    case 0x04: case 0x05: case 0x14: case 0x15: return (1u << rs) | (1u << rt);
    case 0x10: return ((i >> 21) & 31) == 4 || ((i >> 21) & 31) == 5 ? 1u << rt : 0;     // mtc0
    case 0x11: { uint32_t f = (i >> 21) & 31; return (f == 4 || f == 5 || f == 6) ? 1u << rt : 0; }
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x38: case 0x3C: case 0x3F:
        return (1u << rs) | (1u << rt);
    case 0x22: case 0x26: case 0x1A: case 0x1B: return (1u << rs) | (1u << rt);           // lwl lwr ldl ldr merge into rt
    default: return 1u << rs;
    }
}
// the FPU registers an FPU instruction reads (approximate: fs, ft), for a lwc1's value used at once
static bool con_fpu_reads(uint32_t i, int f)
{
    uint32_t op = i >> 26;
    if (op == 0x39 || op == 0x3D) return (int)((i >> 16) & 31) == f;           // swc1 / sdc1 ft
    if (op != 0x11) return false;
    uint32_t fmt = (i >> 21) & 31;
    if (fmt == 0 || fmt == 1) return (int)((i >> 11) & 31) == f;              // mfc1 fs
    if (fmt < 0x10) return false;
    return (int)((i >> 11) & 31) == f || (int)((i >> 16) & 31) == f;
}
static void con_retire_game(uint32_t pc, uint32_t i)
{
    double c = 1;
    cm.n_game++;
    cm.con[ConModel::C_BASE] += 1;
    con_iline(pc);
    // the load-use interlock
    if (cm.ld_dst > 0 || (cm.ld_fpu && cm.ld_dst >= 0)) {
        bool use = cm.ld_fpu ? con_fpu_reads(i, cm.ld_dst) : (con_reads(i) >> cm.ld_dst) & 1;
        if (use) { c += cp("ld_use"); cm.con[ConModel::C_LDUSE] += cp("ld_use"); cm.n_lduse++; }
    }
    cm.ld_dst = -1; cm.ld_fpu = false;
    uint32_t op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, fn = i & 63;
    int32_t imm = (int16_t)(i & 0xFFFF);
    uint32_t ea = cm.r[rs] + imm;
    bool ea_ok = cm.known[rs];
    cm.cyc += c;
    // multiply / divide
    if (op == 0) {
        double lat = -1;
        switch (fn) {
        case 0x18: case 0x19: lat = cp("mult"); cm.mdcnt[0]++; break;
        case 0x1A: case 0x1B: lat = cp("div"); cm.mdcnt[1]++; break;
        case 0x1C: case 0x1D: lat = cp("dmult"); cm.mdcnt[2]++; break;
        case 0x1E: case 0x1F: lat = cp("ddiv"); cm.mdcnt[3]++; break;
        case 0x10: case 0x12: case 0x11: case 0x13:                           // mfhi mflo mthi mtlo: wait for it
            if (cm.md_ready > cm.cyc) { double w = cm.md_ready - cm.cyc; cm.con[ConModel::C_MD] += w; cm.cyc += w; }
            break;
        }
        if (lat > 0) {
            if (cm.md_ready > cm.cyc) { double w = cm.md_ready - cm.cyc; cm.con[ConModel::C_MD] += w; cm.cyc += w; }
            if (cp("md_stall") != 0) { cm.con[ConModel::C_MD] += lat - 1; cm.cyc += lat - 1; cm.md_ready = cm.cyc; }
            else cm.md_ready = cm.cyc + lat - 1;
        }
    }
    // the FPU: the pipeline waits for the operation
    if (op == 0x11) { cm.fcnt[fpu_class(i)]++; double f = fpu_cost(i) - 1; cm.con[ConModel::C_FPU] += f; cm.cyc += f; }
    // likely branches not taken: decided at the next retire (whether pc + 4 retires next) -- approximated: the slot never
    // retires, so the next game pc is pc + 8
    // memory
    bool ld = (op >= 0x20 && op <= 0x27) || op == 0x30 || op == 0x31 || op == 0x35 || op == 0x37 || op == 0x1A || op == 0x1B || op == 0x34;
    bool st = (op >= 0x28 && op <= 0x2E) || op == 0x38 || op == 0x39 || op == 0x3D || op == 0x3F || op == 0x3C;
    if ((ld || st) && ea_ok) con_access(ea, st, 4);
    if (op == 0x2F && ea_ok) con_cacheop(rt, ea);
    if (ld) { cm.ld_dst = rt; cm.ld_fpu = op == 0x31 || op == 0x35; }
    if (op == 0x10 && ((i >> 21) & 31) == 0x10 && fn == 0x18) { cm.con[ConModel::C_EXC] += cp("eret") - 1; cm.cyc += cp("eret") - 1; }
}
// the instruction a monitor entry stood for: what the console takes for it
static void con_emulated(uint32_t pc, int code)
{
    uint32_t i = con_fetch(pc);
    uint32_t op = i >> 26, rs = (i >> 21) & 31;
    int32_t imm = (int16_t)(i & 0xFFFF);
    double c = 1;
    if ((code == 4 || code == 5) && cm.known[rs]) {
        uint32_t va = cm.r[rs] + imm;
        bool st = code == 5;
        c = con_dev_cost(va & 0x1FFFFFFF, st);
        cm.n_dev++;
    } else if (code == 10) {
        if (op == 0 && ((i & 63) == 0x1E || (i & 63) == 0x1F)) c = 1;        // ddiv(u): the latency is mflo's wait
        if (op == 0 && ((i & 63) == 0x1E || (i & 63) == 0x1F)) cm.md_ready = cm.cyc + cp("ddiv");
    }
    cm.con[ConModel::C_EMUL] += c; cm.cyc += c;
    (void)op;
}
static void con_line()
{
    long ob = cm.ours[ConModel::O_GAME] + cm.ours[ConModel::O_RT];
    double cb = 0; for (int k = 0; k < ConModel::C_N; k++) cb += cm.con[k];
    printf("[conmodel %ld fields: ours game %.2f ms + runtime %.2f ms = %.2f ms busy a field (idle %.2f); console %.2f ms a field; ours / console %.3f; "
           "%ld game instructions (%.3f cycles each ours, %.3f console); %ld monitor entries, %ld game exceptions; I-misses %ld, D-misses %ld (%ld dirty), "
           "uncached %ld, device %ld; console by cause:",
           cm.per, cm.ours[0] / 93750.0 / cm.per, cm.ours[1] / 93750.0 / cm.per, ob / 93750.0 / cm.per, cm.ours[2] / 93750.0 / cm.per, cb / 93750.0 / cm.per,
           cb > 0 ? ob / cb : 0.0, cm.n_game, cm.n_game ? (double)cm.ours[0] / cm.n_game : 0.0, cm.n_game ? cb / cm.n_game : 0.0,
           cm.n_exc_mon, cm.n_exc_game, cm.n_imiss, cm.n_dmiss, cm.n_dwb, cm.n_unc, cm.n_dev);
    for (int k = 0; k < ConModel::C_N; k++) printf(" %s %.1f%%%s", cm.cname[k], cb > 0 ? 100.0 * cm.con[k] / cb : 0.0, k + 1 < ConModel::C_N ? "," : "]\n");
    printf("[conraw %ld fields: game_cyc %ld rt_cyc %ld idle_cyc %ld ninsn %ld lduse %ld likely %ld excgame %ld excmon %ld imiss %ld dmiss %ld dwb %ld md %ld %ld %ld %ld dev",
           cm.per, cm.ours[0], cm.ours[1], cm.ours[2], cm.n_game, cm.n_lduse, cm.n_likely, cm.n_exc_game, cm.n_exc_mon, cm.n_imiss, cm.n_dmiss, cm.n_dwb,
           cm.mdcnt[0], cm.mdcnt[1], cm.mdcnt[2], cm.mdcnt[3]);
    for (int k = 0; k < 6; k++) printf(" %ld", cm.dcnt[k]);
    printf(" fpu");
    for (int k = 0; k < 13; k++) printf(" %ld", cm.fcnt[k]);
    printf(" mdwait %.0f]\n", cm.con[ConModel::C_MD]);
    for (auto &x : cm.fcnt) x = 0; for (auto &x : cm.dcnt) x = 0; for (auto &x : cm.mdcnt) x = 0; cm.n_lduse = cm.n_likely = 0;
    fflush(stdout);
    for (int k = 0; k < ConModel::O_N; k++) { cm.t_ours[k] += cm.ours[k]; cm.ours[k] = 0; }
    cm.t_con += cb; cm.t_fields += cm.per;
    for (auto &c : cm.con) c = 0;
    cm.n_game = cm.n_rt = cm.n_exc_mon = cm.n_exc_game = cm.n_imiss = cm.n_dmiss = cm.n_dwb = cm.n_unc = cm.n_dev = 0;
}
// one rising edge of the CPU's clock
static void con_step()
{
    static bool init;
    if (!init) {
        init = true; con_params(); cm.per = atol(getenv("CONMODEL")); if (cm.per < 1) cm.per = 1;
        for (int k = 0; k < 32; k++) { cm.known[k] = false; cm.r[k] = 0; }
        cm.known[0] = true;
    }
    if (t->vblank && !cm.vb_was) { if (cm.active && ++cm.fields % cm.per == 0) con_line(); }
    cm.vb_was = t->vblank;
    cm.pending++;
    if (t->tr_exc) { cm.exc_pend = true; cm.exc_pc = t->tr_exc_pc; cm.exc_code = t->tr_exc_code; }
    if (!t->tr_retire) return;
    uint32_t pc = t->tr_pc, i = t->tr_insn;
    if (t->tr_wr && t->tr_rd) { cm.r[t->tr_rd] = t->tr_val; cm.known[t->tr_rd] = true; }
    bool g = game_pc(pc), rt = rt_pc(pc);
    if (g) cm.active = true;
    if (cm.exc_pend) {
        cm.exc_pend = false;
        bool from_game = game_pc(cm.exc_pc);
        if (from_game && rt) {                                                // the monitor took it
            cm.n_exc_mon++;
            if (cm.exc_code != 0 && cm.exc_code != 1 && cm.exc_code != 2 && cm.exc_code != 3) con_emulated(cm.exc_pc, cm.exc_code);
        } else if (from_game && g) {                                          // the game's own handler
            cm.n_exc_game++; cm.con[ConModel::C_EXC] += cp("exc"); cm.cyc += cp("exc");
        }
        cm.ld_dst = -1;
    }
    // (the game's idle loop: a branch to itself, and its delay slot)
    static uint32_t idle_pc = 0;
    if (g && i == 0x1000FFFFu) idle_pc = pc;
    int cls = !g ? ConModel::O_RT : (pc == idle_pc || pc == idle_pc + 4) ? ConModel::O_IDLE : ConModel::O_GAME;
    if (!g && !rt) cls = ConModel::O_RT;
    cm.ours[cls] += cm.pending; cm.pending = 0;
    if (cls == ConModel::O_GAME) {
        // a likely branch whose slot was annulled: the console fetched it
        static uint32_t last_pc, last_i;
        uint32_t lop = last_i >> 26;
        bool likely = lop == 0x14 || lop == 0x15 || lop == 0x16 || lop == 0x17 || (lop == 0x01 && (((last_i >> 16) & 31) & 0x0A) == 0x02) ||
                      (lop == 0x11 && ((last_i >> 21) & 31) == 8 && ((last_i >> 17) & 1));
        if (likely && pc == last_pc + 8) { cm.n_likely++; cm.con[ConModel::C_LIKELY] += cp("likely"); cm.cyc += cp("likely"); }
        last_pc = pc; last_i = i;
        con_retire_game(pc, i);
    }
}
static void con_end()
{
    if (!cm.t_fields) return;
    long ob = cm.t_ours[0] + cm.t_ours[1];
    printf("[conmodel total %ld fields: ours busy %.2f ms a field (game %.2f, runtime %.2f), console %.2f ms a field; ours / console %.3f]\n",
           cm.t_fields, ob / 93750.0 / cm.t_fields, cm.t_ours[0] / 93750.0 / cm.t_fields, cm.t_ours[1] / 93750.0 / cm.t_fields,
           cm.t_con / 93750.0 / cm.t_fields, cm.t_con > 0 ? ob / cm.t_con : 0.0);
}
