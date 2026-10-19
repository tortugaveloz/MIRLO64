// Mirlo-N64's FPU: the VR4300's COP1 as N64 games use it, single precision.
//
// Bit for bit what sim/n64/cpu.c (n64_cop1_exec) does:
//  - 32 registers of 32 bits, FR=0: a double is an even/odd pair. A double
//    operand is taken to float (the model's d2f: rounded half up, below the
//    float normals 0, above them infinity) and a double result is the float
//    widened back (f2d) -- doubles are computed as floats.
//  - Inputs and results below the normals are 0 (the VR4300 with FCR31.FS,
//    as libultra sets it); a NaN result is 0x7FBFFFFF; arithmetic rounds to
//    nearest even with an unbounded exponent, then flushes.
//  - cvt.w / round / trunc / ceil / floor .w saturate (NaN: INT32_MAX).
//  - dmfc1 / dmtc1 move a 64-bit register through an even/odd pair; cvt.s.l /
//    cvt.d.l convert the pair's 64-bit integer exactly (a word's by cvt.s.w's
//    path, another by a shift of a bit a cycle); the conversions to .l are the
//    ones to .w, the word sign-extended into the pair (a value past a word's
//    range saturates as a word's). No exceptions, no FCR31 cause / flag bits.
//
// One datapath, several cycles per operation (the core's M stage waits on
// `busy`): an operation's operands are unpacked as it enters M, then
//   add/sub   align, add                  -> normalise, round
//   mul       24x24 on a DSP              -> normalise, round
//   div, sqrt 28 restoring iterations (one shared subtractor) -> normalise, round
//   cvt.s.w   the integer                 -> normalise, round
//   to int    align (the add's shifter), round and saturate
// Registers are read a cycle ahead (MLABs register their read address): the
// read address is the instruction entering M (`insn_next`); a write landing
// on the same edge is bypassed.
//
// FAST (the core's, for its faster clock) cuts the longest of these: a
// register is written a cycle after its instruction completes (from the
// bypass's registers, which the next instruction reads as before); the
// operands are taken from the registers as they are and unpacked from there,
// one a cycle; add and sub leave the operands in place (the larger is chosen
// where they are added) and align in a cycle of their own; the conversions
// to an integer round a cycle after they shift.
`default_nettype none

module fpu_n64 #(
    parameter bit FAST = 0
) (
    input  wire         clk,
    input  wire         rst,
    input  wire  [31:0] insn_next,          // M's instruction next cycle (register read address)
    input  wire  [31:0] insn,               // M's instruction
    input  wire  [31:0] insn_e,             // (FAST) the instruction behind it, which M takes when adv
    input  wire         adv,                // (FAST) M takes it
    input  wire         live,               // it is a COP1 instruction (or l/s wc1/dc1), live in M
    input  wire         valid,              // it completes this cycle: write its results
    input  wire         cancel,             // (FAST) the register write of the one that completed last cycle is taken back
    input  wire  [31:0] gpr_in,             // mtc1, ctc1; dmtc1's lower half
    input  wire  [31:0] gpr_in_hi,          // dmtc1's upper half
    input  wire  [63:0] mem_in,             // lwc1: [31:0]; ldc1: {lower address, higher}
    output logic [31:0] gpr_out,            // mfc1, cfc1; dmfc1's lower half
    output logic [31:0] gpr_out_hi,         // dmfc1's upper half
    output logic [63:0] mem_out,            // swc1: [31:0]; sdc1: {lower address, higher}
    output logic        cond,               // FCR31.C after the instruction
    output logic        busy,
    // what the completing instruction wrote (co-simulation trace)
    output logic        tr_we_even, tr_we_odd,
    output logic [3:0]  tr_widx,
    output logic [31:0] tr_weven, tr_wodd,
    output logic [31:0] tr_fcr31
);

// ============================================================ registers
logic [31:0] fe0 [16], fe1 [16], fo0 [16], fo1 [16];   // even / odd banks, one copy per read port
initial for (int i = 0; i < 16; i++) begin fe0[i] = 0; fe1[i] = 0; fo0[i] = 0; fo1[i] = 0; end
logic [31:0] qxe, qxo, qye, qyo;                       // pair fs>>1 (port x), pair ft>>1 (port y)
logic        we_e, we_o; logic [3:0] widx; logic [31:0] wd_e, wd_o;
logic        lw_e, lw_o; logic [3:0] lw_idx; logic [31:0] lw_de, lw_do;
logic        l2_e, l2_o; logic [3:0] l2_idx; logic [31:0] l2_de, l2_do;       // (FAST)
generate if (!FAST) begin : g_rf
    always_ff @(posedge clk) begin
        qxe <= fe0[insn_next[15:12]]; qye <= fe1[insn_next[20:17]];
        qxo <= fo0[insn_next[15:12]]; qyo <= fo1[insn_next[20:17]];
        if (we_e) begin fe0[widx] <= wd_e; fe1[widx] <= wd_e; end
        if (we_o) begin fo0[widx] <= wd_o; fo1[widx] <= wd_o; end
        lw_e <= we_e; lw_o <= we_o; lw_idx <= widx; lw_de <= wd_e; lw_do <= wd_o;
    end
end else begin : g_rff
    // the write from lw_*, a cycle late: what a read may have missed is in
    // lw_* still, or went in at the read's own edge (l2_*)
    always_ff @(posedge clk) begin
        qxe <= fe0[insn_next[15:12]]; qye <= fe1[insn_next[20:17]];
        qxo <= fo0[insn_next[15:12]]; qyo <= fo1[insn_next[20:17]];
        if (lw_e && !cancel) begin fe0[lw_idx] <= lw_de; fe1[lw_idx] <= lw_de; end
        if (lw_o && !cancel) begin fo0[lw_idx] <= lw_do; fo1[lw_idx] <= lw_do; end
        lw_e <= we_e; lw_o <= we_o; lw_idx <= widx; lw_de <= wd_e; lw_do <= wd_o;
        l2_e <= lw_e && !cancel; l2_o <= lw_o && !cancel; l2_idx <= lw_idx; l2_de <= lw_de; l2_do <= lw_do;
    end
end endgenerate
// the pairs M's instruction reads, with the write of the edge that read them
// (FAST: whether the write in lw_* / l2_* is of the register M's instruction reads, found a cycle ahead -- against the
// instruction that M takes at this edge, insn_e if it advances, insn if not: the one the read address just took -- and
// registered, so that the bypass is a select by flip-flops, not a compare of two indexes)
logic hx_e, hx_o, hy_e, hy_o, gx_e, gx_o, gy_e, gy_o;
always_ff @(posedge clk) begin
    hx_e <= we_e && (adv ? widx == insn_e[15:12] : widx == insn[15:12]);
    hx_o <= we_o && (adv ? widx == insn_e[15:12] : widx == insn[15:12]);
    hy_e <= we_e && (adv ? widx == insn_e[20:17] : widx == insn[20:17]);
    hy_o <= we_o && (adv ? widx == insn_e[20:17] : widx == insn[20:17]);
    gx_e <= lw_e && !cancel && (adv ? lw_idx == insn_e[15:12] : lw_idx == insn[15:12]);
    gx_o <= lw_o && !cancel && (adv ? lw_idx == insn_e[15:12] : lw_idx == insn[15:12]);
    gy_e <= lw_e && !cancel && (adv ? lw_idx == insn_e[20:17] : lw_idx == insn[20:17]);
    gy_o <= lw_o && !cancel && (adv ? lw_idx == insn_e[20:17] : lw_idx == insn[20:17]);
end
wire [31:0] xe = FAST ? (hx_e ? lw_de : gx_e ? l2_de : qxe) : (lw_e && lw_idx == insn[15:12]) ? lw_de : qxe;
wire [31:0] xo = FAST ? (hx_o ? lw_do : gx_o ? l2_do : qxo) : (lw_o && lw_idx == insn[15:12]) ? lw_do : qxo;
wire [31:0] ye = FAST ? (hy_e ? lw_de : gy_e ? l2_de : qye) : (lw_e && lw_idx == insn[20:17]) ? lw_de : qye;
wire [31:0] yo = FAST ? (hy_o ? lw_do : gy_o ? l2_do : qyo) : (lw_o && lw_idx == insn[20:17]) ? lw_do : qyo;
wire [31:0] rfs = insn[11] ? xo : xe;                   // f[fs]
wire [31:0] rft = insn[16] ? yo : ye;                   // f[ft]

logic [31:0] fcr31;                                     // RM, C (bit 23), FS and the enables (as written)

// ============================================================ decode
wire [5:0] op = insn[31:26], fn = insn[5:0];
wire [4:0] fmt = insn[25:21];
wire is_cop1 = op == 6'h11;
wire is_mfc1 = is_cop1 && fmt == 5'h00, is_cfc1 = is_cop1 && fmt == 5'h02;
wire is_mtc1 = is_cop1 && fmt == 5'h04, is_ctc1 = is_cop1 && fmt == 5'h06, is_dmtc1 = is_cop1 && fmt == 5'h05;
wire is_dmfc1 = is_cop1 && fmt == 5'h01;
wire is_s = is_cop1 && fmt == 5'h10, is_d = is_cop1 && fmt == 5'h11, is_w = is_cop1 && fmt == 5'h14;
wire is_l = is_cop1 && fmt == 5'h15 && (fn == 6'h20 || fn == 6'h21);   // cvt.s.l, cvt.d.l
// to a 64-bit integer (cvt.l, round.l, trunc.l, ceil.l, floor.l): done as to a word, the word sign-extended into the pair
wire to_l = (is_s || is_d) && ((fn >= 6'h08 && fn <= 6'h0B) || fn == 6'h25);
wire is_mov = (is_s || is_d) && fn == 6'h06;
wire is_arith = ((is_s || is_d || is_w) && !is_mov) || is_l;      // through the datapath
wire is_lwc1 = op == 6'h31, is_ldc1 = op == 6'h35, is_swc1 = op == 6'h39, is_sdc1 = op == 6'h3D;

// ============================================================ unpack
typedef struct packed { logic s; logic [7:0] e; logic [22:0] f; } fbits_t;
function automatic fbits_t d2f(input logic [31:0] lo, input logic [31:0] hi);
    logic [10:0] e; logic signed [12:0] e2; logic [31:0] r;
    e = hi[30:20];
    e2 = $signed({2'b0, e}) - 13'sd896;
    if (e == 11'h7FF) r = {hi[31], 8'hFF, hi[19:0], lo[31:29]} | {31'd0, lo[28:0] != 0};
    else if (e2 <= 0) r = {hi[31], 31'd0};
    else if (e2 >= 255) r = {hi[31], 8'hFF, 23'd0};
    else r = {hi[31], e2[7:0], hi[19:0], lo[31:29]} + {31'd0, lo[28]};   // (a carry may reach the exponent)
    return r;
endfunction
function automatic fbits_t ftz(input fbits_t x);
    return x.e == 8'd0 ? fbits_t'({x.s, 31'd0}) : x;
endfunction

// ============================================================ state
// S_IDLE unpacks fs, S_UNPY ft (one unpacker, a register between the RAM and
// everything else); S_DECIDE takes the special cases and dispatches
typedef enum logic [4:0] { S_IDLE, S_UNPY, S_DECIDE, S_ADD2, S_MUL, S_ITER, S_NORM, S_ROUND, S_F2W, S_DONE,
                           S_UNPX, S_F2W2, S_ADDA, S_PRE, S_NORM2, S_LNEG, S_LNEG2, S_LSH1 } st_t;        // (FAST's)
typedef enum logic [2:0] { K_ADD, K_MUL, K_DIV, K_SQRT, K_CVT, K_F2W, K_CMP, K_MOVE } kind_t;
st_t   st;
logic fnx, fny, fix, fiy, fzx, fzy, fmlt, fmeq;   // (FAST) S_PRE's: the operands' kinds, their magnitudes' order
logic [5:0] nzc;                                   // (FAST) the leading zeros S_NORM counted
kind_t kind;
fbits_t ax, ay;                                          // the operands (add: ax the larger)
logic   dst_d;                                           // the result is a double (a pair)
logic   dst_l;                                           // the result is a 64-bit integer (a word, sign-extended into the pair)
logic [31:0] res;                                        // the result (float bits, or an integer)
logic   rcond;
// the unnormalised result: value = zmag * 2^(zexp - 158); div/sqrt build their
// quotient / root in zmag, S_NORM normalises it in place
logic        zs;
logic signed [11:0] zexp;
logic [31:0] zmag;
logic        zst;                                        // sticky: bits below zmag
logic        sub_eff;                                    // add: the signs differ
logic [7:0]  ad;                                         // add: the exponent difference
logic [31:0] ir;                                         // div / sqrt: the remainder
logic [4:0]  icnt;
logic [55:0] mp;                                         // mul: the product; sqrt: the radicand's bits to come
logic [1:0]  rmode;                                      // to int: the rounding
logic        sw;                                         // (FAST) add: ay is the larger
logic        fg;                                         // (FAST) to int: the guard bit
logic        lcar;                                       // cvt.s.l: the lower word's negation carries

// the unpacker: fs in S_IDLE, ft in S_UNPY (W: the integer as it is)
wire         upy = st == S_UNPY;
// (FAST: from the operands as S_IDLE took them -- fs in ax, a double's upper
// half in zmag; ft in ay, its upper half in ir)
fbits_t up_raw;
assign up_raw = FAST ? (upy ? ay : ax) : upy ? rft : rfs;
wire [31:0] up_lo = FAST ? (upy ? ay : ax) : upy ? ye : xe;
wire [31:0] up_hi = FAST ? (upy ? ir : zmag) : upy ? yo : xo;
fbits_t up;
assign up = (sel_w || sel_l) ? up_raw : ftz(sel_d ? d2f(up_lo, up_hi) : up_raw);

// (FAST: without `live` -- the core asks only for an instruction in M, and
// one that is not live is flushed)
// (FAST: whether M's instruction goes through the datapath, from a register that takes the next
// one's decode as it enters -- the stall that this makes would otherwise have the decode in front of it)
function automatic logic arith_of(input logic [31:0] i);
    logic cop1, s_, d_, w_;
    cop1 = i[31:26] == 6'h11;
    s_ = cop1 && i[25:21] == 5'h10; d_ = cop1 && i[25:21] == 5'h11; w_ = cop1 && i[25:21] == 5'h14;
    return ((s_ || d_ || w_) && !((s_ || d_) && i[5:0] == 6'h06)) || (cop1 && i[25:21] == 5'h15 && (i[5:0] == 6'h20 || i[5:0] == 6'h21));
endfunction
logic ar_q;
always_ff @(posedge clk) if (adv) ar_q <= arith_of(insn_e);
logic isd_q, isw_q, isl_q;                              // (FAST) is_d, is_w and is_l, likewise
always_ff @(posedge clk) if (adv) begin
    isd_q <= insn_e[31:26] == 6'h11 && insn_e[25:21] == 5'h11;
    isw_q <= insn_e[31:26] == 6'h11 && insn_e[25:21] == 5'h14;
    isl_q <= insn_e[31:26] == 6'h11 && insn_e[25:21] == 5'h15;
end
wire sel_d = FAST ? isd_q : is_d, sel_w = FAST ? isw_q : is_w, sel_l = FAST ? isl_q : is_l;
assign busy = FAST ? ar_q && st != S_DONE : live && is_arith && st != S_DONE;

// ------------------------------------------------------------ shared pieces
// right shift with sticky: {out[33:0], sticky}
function automatic logic [34:0] rshift(input logic [33:0] v, input logic [7:0] n);
    logic [33:0] o; logic sk;
    if (n >= 8'd34) begin o = 34'd0; sk = v != 0; end
    else begin o = v >> n; sk = (v & ~(34'h3FFFFFFFF << n)) != 0; end
    return {o, sk};
endfunction
function automatic logic [4:0] lzc32(input logic [31:0] v);
    logic [4:0] n; n = 5'd31;
    for (int i = 0; i < 32; i++) if (v[i]) n = 5'(31 - i);
    return n;
endfunction

// the align shifter: add (the smaller significand, below the larger's at
// [30:7]) or to-int ({1.f, 10'b0} >> (158 - e): the integer, a guard, a sticky)
wire [7:0]  sh_n = (!FAST && st == S_F2W) ? 8'(8'd158 - ax.e) : ad;
wire [22:0] add_lf = (FAST && sw) ? ay.f : ax.f;        // add: the larger's fraction, the smaller's
wire [22:0] add_sf = (FAST && sw) ? ax.f : ay.f;
wire [7:0]  add_le = (FAST && sw) ? ay.e : ax.e;
wire [33:0] sh_v = st == S_F2W ? {1'b1, ax.f, 10'd0} : {1'b0, 1'b1, add_sf, 9'd0};
wire [34:0] sh_o = rshift(sh_v, sh_n);

// the iteration's subtractor (div: r - divisor; sqrt: (r << 2 | next) - (q << 2 | 1))
wire [31:0] it_a = kind == K_DIV ? ir : {ir[29:0], mp[55:54]};
wire [31:0] it_b = kind == K_DIV ? {8'd0, 1'b1, ay.f} : {2'd0, zmag[27:0], 2'b01};
wire [32:0] it_d = {1'b0, it_a} - {1'b0, it_b};
wire        it_ge = !it_d[32];

// round (zmag normalised: bit 31 set) and pack; FTZ, overflow to infinity
wire [24:0] rm = {1'b0, zmag[31:8]} + {24'd0, zmag[7] && (zst || zmag[6:0] != 0 || zmag[8])};
wire signed [11:0] rexp = zexp + 12'(rm[24]);
wire [31:0] rbits = rexp >= 255 ? {zs, 8'hFF, 23'd0}
                  : rexp <= 0   ? {zs, 31'd0}
                  :               {zs, rexp[7:0], rm[24] ? rm[23:1] : rm[22:0]};

// f2d: a float as a double pair {lo, hi}
function automatic logic [63:0] f2d(input logic [31:0] f);
    logic [10:0] e;
    if (f[30:23] == 8'd0) return {32'd0, f[31], 31'd0};
    e = f[30:23] == 8'hFF ? 11'h7FF : 11'(f[30:23]) + 11'd896;
    return {f[2:0], 29'd0, f[31], e, f[22:3]};
endfunction

localparam logic [31:0] QNAN = 32'h7FBFFFFF;
function automatic logic isnan(input fbits_t x); return x.e == 8'hFF && x.f != 0; endfunction
function automatic logic isinf(input fbits_t x); return x.e == 8'hFF && x.f == 0; endfunction
function automatic logic iszero(input fbits_t x); return x.e == 8'd0; endfunction

// ============================================================ the FSM
always_ff @(posedge clk) begin
    if (rst) begin
        st <= S_IDLE; fcr31 <= 32'h01000800;
    end else if (!live) st <= S_IDLE;                     // flushed: its work is dropped
    else case (st)
    S_IDLE: if (FAST ? ar_q : is_arith) begin
        ax <= up; st <= S_UNPY;
        if (!FAST) zmag <= xo;                            // (cvt.s.l: the integer's upper half)
        if (FAST) begin
            ax <= (isd_q || isl_q) ? xe : rfs; zmag <= xo; ay <= isd_q ? ye : rft; ir <= yo;
            st <= S_UNPX;
        end
        dst_d <= (is_d && fn != 6'h20 && !(fn >= 6'h08 && fn <= 6'h0F) && fn != 6'h24 && fn != 6'h25 && fn < 6'h30) ||
                 ((is_s || is_w || is_l) && fn == 6'h21);
        dst_l <= to_l;
        rmode <= (fn == 6'h24 || fn == 6'h25) ? fcr31[1:0] : fn[1:0];      // to int: 0C/08 round, 0D/09 trunc, 0E/0A ceil, 0F/0B floor
    end
    S_UNPX: begin ax <= up; st <= S_UNPY; end
    S_UNPY: begin ay <= up; st <= FAST ? S_PRE : S_DECIDE; end
    // (FAST) what S_DECIDE asks of the two operands, as registers
    S_PRE: begin
        fnx <= isnan(ax); fny <= isnan(ay); fix <= isinf(ax); fiy <= isinf(ay); fzx <= iszero(ax); fzy <= iszero(ay);
        fmlt <= {ax.e, ax.f} < {ay.e, ay.f}; fmeq <= {ax.e, ax.f} == {ay.e, ay.f};
        st <= S_DECIDE;
    end
    S_DECIDE: begin
        logic nx, ny, ix, iy, zx, zy;
        nx = FAST ? fnx : isnan(ax); ny = FAST ? fny : isnan(ay); ix = FAST ? fix : isinf(ax); iy = FAST ? fiy : isinf(ay);
        zx = FAST ? fzx : iszero(ax); zy = FAST ? fzy : iszero(ay);
        st <= S_DONE;                                     // (the special cases: done now)
        if (sel_l) begin                                  // cvt.s.l: the integer {zmag, ax}
            kind <= K_CVT; zst <= 0; zexp <= 12'sd158;
            if (zmag == 32'd0 && ax == 32'd0) res <= 32'd0;
            else if (zmag == 32'd0) begin zs <= 0; zmag <= ax; st <= S_NORM; end                    // an unsigned word
            else if (zmag == {32{ax[31]}}) begin zs <= ax[31]; zmag <= ax[31] ? -ax : ax; st <= S_NORM; end   // a word
            else begin zs <= zmag[31]; ir <= ax; st <= S_LNEG; end                                  // 64 bits
        end else if (sel_w) begin                                  // cvt.s.w, cvt.d.w: ax is the integer
            kind <= K_CVT; zs <= ax.s; zmag <= ax.s ? -ax : ax; zexp <= 12'sd158; zst <= 0;
            if (ax == 32'd0) res <= 32'd0; else st <= S_NORM;
        end else if (fn >= 6'h30) begin                   // c.cond
            logic un, eq, lt;
            un = nx || ny;
            eq = !un && ((zx && zy) || (FAST ? fmeq && ax.s == ay.s : ax == ay));
            lt = !un && !(zx && zy) && (ax.s != ay.s ? ax.s : (ax.s ? (FAST ? !fmlt && !fmeq : {ax.e, ax.f} > {ay.e, ay.f}) : (FAST ? fmlt : {ax.e, ax.f} < {ay.e, ay.f})));
            kind <= K_CMP; rcond <= (fn[0] && un) || (fn[1] && eq) || (fn[2] && lt);
        end else if ((fn >= 6'h08 && fn <= 6'h0F) || fn == 6'h24 || fn == 6'h25) begin   // to int (.w, and .l as .w)
            kind <= K_F2W;
            if (nx) res <= 32'h7FFFFFFF;
            else if (ax.e >= 8'd158) res <= ax.s ? 32'h80000000 : 32'h7FFFFFFF;
            else if (zx) res <= 32'd0;
            else st <= S_F2W;
            if (FAST) ad <= 8'd158 - ax.e;
        end else case (fn)
        6'h00, 6'h01: begin                               // add, sub: order by magnitude
            logic sy; fbits_t y;
            y = ay; y.s = ay.s ^ fn[0]; sy = y.s;
            kind <= K_ADD;
            if (nx || ny || (ix && iy && ax.s != sy)) res <= QNAN;
            else if (ix) res <= ax;
            else if (iy) res <= y;
            else if (zx && zy) res <= {ax.s & sy, 31'd0};
            else if (zx) res <= y;
            else if (zy) res <= ax;
            else begin
                if (FAST) begin
                    sw <= fmlt;
                    if (!fmlt) begin ad <= ax.e - ay.e; zs <= ax.s; end
                    else begin ad <= ay.e - ax.e; zs <= sy; end
                end
                else if ({ax.e, ax.f} >= {ay.e, ay.f}) begin ay <= y; ad <= ax.e - ay.e; zs <= ax.s; end
                else begin ax <= y; ay <= ax; ad <= ay.e - ax.e; zs <= sy; end
                sub_eff <= ax.s != sy;
                st <= FAST ? S_ADDA : S_ADD2;
            end
        end
        6'h02: begin                                      // mul
            kind <= K_MUL;
            if (nx || ny || (ix && zy) || (zx && iy)) res <= QNAN;
            else if (ix || iy) res <= {ax.s ^ ay.s, 8'hFF, 23'd0};
            else if (zx || zy) res <= {ax.s ^ ay.s, 31'd0};
            else st <= S_MUL;
        end
        6'h03: begin                                      // div
            kind <= K_DIV;
            if (nx || ny || (ix && iy) || (zx && zy)) res <= QNAN;
            else if (ix || zy) res <= {ax.s ^ ay.s, 8'hFF, 23'd0};
            else if (zx || iy) res <= {ax.s ^ ay.s, 31'd0};
            else begin
                ir <= {8'd0, 1'b1, ax.f}; zmag <= 0; icnt <= 5'd27;
                zs <= ax.s ^ ay.s; zexp <= 12'(ax.e) - 12'(ay.e) + 12'sd131;
                st <= S_ITER;
            end
        end
        6'h04: begin                                      // sqrt
            kind <= K_SQRT;
            if (nx || (ax.s && !zx)) res <= QNAN;
            else if (zx || ix) res <= ax;
            else begin
                logic signed [11:0] t;
                t = 12'(ax.e) - 12'sd150;                 // value = m * 2^t
                ir <= 0; zmag <= 0; icnt <= 5'd27; zs <= 0;
                // the radicand M * 2^30, M = m (t even) or m << 1 (t odd): a 28-bit root
                if (t[0]) begin mp <= {1'd0, 1'b1, ax.f, 31'd0}; zexp <= ((t - 12'sd1) >>> 1) + 12'sd143; end
                else      begin mp <= {2'd0, 1'b1, ax.f, 30'd0}; zexp <= (t >>> 1) + 12'sd143; end
                st <= S_ITER;
            end
        end
        default: begin                                    // abs, neg, cvt.s.d, cvt.d.s
            kind <= K_MOVE;
            if (nx) res <= QNAN;
            else if (fn == 6'h05) res <= {1'b0, ax.e, ax.f};
            else if (fn == 6'h07) res <= {!ax.s, ax.e, ax.f};
            else res <= ax;
        end
        endcase
    end
    S_ADDA: begin zmag <= sh_o[34:3] | {31'd0, sh_o[2] | sh_o[1] | sh_o[0]}; st <= S_ADD2; end
    S_ADD2: begin                                         // align, add: the larger at [30:7]
        logic [31:0] a, b, sum;
        a = {1'b0, 1'b1, add_lf, 7'd0};
        b = sh_o[34:3] | {31'd0, sh_o[2] | sh_o[1] | sh_o[0]};   // the shifted-out bits as a sticky LSB
        if (FAST) b = zmag;
        sum = sub_eff ? a - b : a + b;
        zmag <= sum; zst <= 0; zexp <= 12'(add_le) + 12'sd1;
        if (FAST) st <= S_NORM;                                   // (x - x: S_NORM's)
        else if (sum == 32'd0) begin res <= 32'd0; st <= S_DONE; end   // x - x: +0
        else st <= S_NORM;
    end
    S_MUL: begin
        mp[47:0] <= {1'b1, ax.f} * {1'b1, ay.f};
        zs <= ax.s ^ ay.s; zexp <= 12'(ax.e) + 12'(ay.e) - 12'sd126;
        st <= S_NORM;
    end
    S_ITER: begin
        zmag <= {zmag[30:0], it_ge};
        if (kind == K_DIV) ir <= (it_ge ? it_d[31:0] : it_a) << 1;
        else begin ir <= it_ge ? it_d[31:0] : it_a; mp <= {mp[53:0], 2'b00}; end
        icnt <= icnt - 5'd1;
        if (icnt == 5'd0) st <= S_NORM;
    end
    S_NORM: begin
        logic [31:0] m; logic sti;
        if (kind == K_MUL) begin m = mp[47:16]; sti = mp[15:0] != 0; end
        else if (kind == K_DIV || kind == K_SQRT) begin m = zmag; sti = ir != 0; end
        else begin m = zmag; sti = zst; end
        if (FAST) begin zmag <= m; nzc <= lzc32(m); zst <= sti; st <= S_NORM2; end         // (the count, then the shift)
        else begin zmag <= m << lzc32(m); zexp <= zexp - 12'(lzc32(m)); zst <= sti; st <= S_ROUND; end
        if (FAST && kind == K_ADD && zmag == 32'd0) begin res <= 32'd0; st <= S_DONE; end
    end
    // cvt.s.l of one that is not a word: the magnitude {zmag, ir} a word at a time (the lower word, then the upper one with
    // its carry), then shifted left a bit a cycle until its top bit is set (at most 32 cycles), the bits below the upper
    // word kept as the sticky
    S_LNEG: begin
        if (zs) begin ir <= -ir; lcar <= ir == 32'd0; end
        st <= S_LNEG2;
    end
    S_LNEG2: begin
        logic [31:0] h;
        h = zs ? ~zmag + 32'(lcar) : zmag;
        if (h == 32'd0) begin zmag <= ir; st <= S_NORM; end
        else begin zmag <= h; zexp <= 12'sd190; st <= S_LSH1; end
    end
    S_LSH1: if (zmag[31]) begin zst <= ir != 32'd0; st <= S_ROUND; end
            else begin zmag <= {zmag[30:0], ir[31]}; ir <= {ir[30:0], 1'b0}; zexp <= zexp - 12'sd1; end
    S_NORM2: begin zmag <= zmag << nzc; zexp <= zexp - 12'(nzc); st <= S_ROUND; end
    S_ROUND: begin res <= rbits; st <= S_DONE; end
    S_F2W: if (FAST) begin zmag <= sh_o[34:3]; fg <= sh_o[2]; zst <= sh_o[1] | sh_o[0]; st <= S_F2W2; end
    else begin                                            // |x| < 2^31
        logic [31:0] i; logic g, sk, inc;
        i = sh_o[34:3]; g = sh_o[2]; sk = sh_o[1] | sh_o[0];
        case (rmode)
        2'd0: inc = g && (sk || i[0]);
        2'd1: inc = 1'b0;
        2'd2: inc = !ax.s && (g || sk);
        default: inc = ax.s && (g || sk);
        endcase
        i = i + {31'd0, inc};
        res <= ax.s ? -i : i;
        st <= S_DONE;
    end
    S_F2W2: begin
        logic [31:0] i; logic inc;
        case (rmode)
        2'd0: inc = fg && (zst || zmag[0]);
        2'd1: inc = 1'b0;
        2'd2: inc = !ax.s && (fg || zst);
        default: inc = ax.s && (fg || zst);
        endcase
        // (the rounded magnitude, negated for a negative: -(m + inc) is ~m + !inc -- one adder)
        i = (zmag ^ {32{ax.s}}) + {31'd0, inc ^ ax.s};
        res <= i;
        st <= S_DONE;
    end
    S_DONE: if (valid) st <= S_IDLE;
    default: st <= S_IDLE;
    endcase

    // ctc1 (FCR31), c.cond (its C bit), as they complete
    if (!rst && valid) begin
        if (is_ctc1 && insn[15:11] == 5'd31) fcr31 <= gpr_in & 32'h0183FFFF;
        if (is_arith && kind == K_CMP) fcr31[23] <= rcond;
    end
end

// ============================================================ outputs, writes
logic [63:0] res_d;
always_comb res_d = f2d(res);
always_comb begin
    gpr_out = is_mfc1 ? rfs : is_dmfc1 ? xe : (insn[15:11] == 5'd31 ? fcr31 : insn[15:11] == 5'd0 ? 32'h00000B00 : 32'd0);
    gpr_out_hi = xo;
    mem_out = is_sdc1 ? {yo, ye} : {32'd0, rft};
    cond = is_ctc1 ? gpr_in[23] : (is_arith && kind == K_CMP) ? rcond : fcr31[23];

    we_e = 0; we_o = 0; widx = 0; wd_e = 0; wd_o = 0;
    if (valid) begin
        if (is_mtc1) begin widx = insn[15:12]; if (insn[11]) begin we_o = 1; wd_o = gpr_in; end else begin we_e = 1; wd_e = gpr_in; end end
        if (is_dmtc1) begin widx = insn[15:12]; we_e = 1; we_o = 1; wd_e = gpr_in; wd_o = gpr_in_hi; end   // (FR=0: the pair)
        if (is_lwc1) begin widx = insn[20:17]; if (insn[16]) begin we_o = 1; wd_o = mem_in[31:0]; end else begin we_e = 1; wd_e = mem_in[31:0]; end end
        if (is_ldc1) begin widx = insn[20:17]; we_e = 1; we_o = 1; wd_e = mem_in[31:0]; wd_o = mem_in[63:32]; end
        if (is_mov) begin
            widx = insn[10:7];
            if (is_d) begin we_e = 1; we_o = 1; wd_e = xe; wd_o = xo; end
            else if (insn[6]) begin we_o = 1; wd_o = rfs; end else begin we_e = 1; wd_e = rfs; end
        end
        if (is_arith && kind != K_CMP) begin
            widx = insn[10:7];
            if (dst_l) begin we_e = 1; we_o = 1; wd_e = res; wd_o = {32{res[31]}}; end
            else if (dst_d) begin we_e = 1; we_o = 1; wd_e = res_d[63:32]; wd_o = res_d[31:0]; end
            else if (insn[6]) begin we_o = 1; wd_o = res; end else begin we_e = 1; wd_e = res; end
        end
    end
end

// the trace: registered with the write, so it lines up with the core's retire (W)
always_ff @(posedge clk) begin
    tr_we_even <= we_e; tr_we_odd <= we_o; tr_widx <= widx; tr_weven <= wd_e; tr_wodd <= wd_o;
    tr_fcr31 <= (valid && is_ctc1 && insn[15:11] == 5'd31) ? gpr_in & 32'h0183FFFF
              : (valid && is_arith && kind == K_CMP) ? {fcr31[31:24], rcond, fcr31[22:0]} : fcr31;
end

endmodule
`default_nettype wire
