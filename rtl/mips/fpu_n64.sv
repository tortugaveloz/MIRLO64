// Mirlo-N64's FPU: the VR4300's COP1 as N64 games use it, single precision.
//
// Bit for bit what sim/n64/cpu.c (n64_cop1_exec) does:
//  - 32 registers of 32 bits, FR=0: a double is an even/odd pair. A double
//    operand is taken to float (the model's d2f: rounded half up, below the
//    float normals 0, above them infinity) and a double result is the float
//    widened back (f2d) -- doubles are computed as floats.
//  - Inputs and results below the normals are 0 (the VR4300 with FCR31.FS,
//    as libultra sets it) -- but a single's compare, which orders them
//    exactly; a NaN result is 0x7FBFFFFF; arithmetic rounds to
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
    logic [10:0] e; logic [31:0] r;
    e = hi[30:20];
    if (e == 11'h7FF) r = {hi[31], 8'hFF, hi[19:0], lo[31:29]} | {31'd0, lo[28:0] != 0};
    else if (e <= 11'd896) r = {hi[31], 31'd0};
    else if (e >= 11'd1151) r = {hi[31], 8'hFF, 23'd0};
    // Subtracting 896 in this range flips bit 7 of the low exponent byte.
    else r = {hi[31], ~e[7], e[6:0], hi[19:0], lo[31:29]} + {31'd0, lo[28]};
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
logic [55:0] mp;                                         // sqrt: the radicand's bits to come
// mul's product, taken every clock and read in S_NORM, the state after S_MUL
// (which took it into mp before): a register with nothing but the multiplier
// in front of it, which the fitter can make the DSP block's output register
logic [47:0] mprod;
always_ff @(posedge clk) mprod <= {1'b1, ax.f} * {1'b1, ay.f};
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
// (a single's compare takes its operands as they are: the VR4300 orders a denormal exactly there, and does not trap on
// it -- software tests a pointer kept in a float register against 0.0, and KSEG0's below 8 MiB are all denormals)
wire cmp_s = !sel_d && fn[5:4] == 2'b11;
assign up = (sel_w || sel_l || cmp_s) ? up_raw : ftz(sel_d ? d2f(up_lo, up_hi) : up_raw);

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
    logic [4:0] n; logic [31:0] a;
    a = v; n = 0;
    if (!(|a[31:16])) begin n[4]=1; a = a << 16; end
    if (!(|a[31:24])) begin n[3]=1; a = a << 8; end
    if (!(|a[31:28])) begin n[2]=1; a = a << 4; end
    if (!(|a[31:30])) begin n[1]=1; a = a << 2; end
    if (!a[31]) n[0]=1;
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
wire [31:0] za_b = FAST ? zmag : sh_o[34:3] | {31'd0, sh_o[2] | sh_o[1] | sh_o[0]};   // (the shifted-out bits as a sticky LSB)

// the iteration's subtractor (div: r - divisor; sqrt: (r << 2 | next) - (q << 2 | 1))
wire [31:0] it_a = kind == K_DIV ? ir : {ir[29:0], mp[55:54]};
wire [31:0] it_b = kind == K_DIV ? {8'd0, 1'b1, ay.f} : {2'd0, zmag[27:0], 2'b01};
wire [32:0] it_d = {1'b0, it_a} - {1'b0, it_b};
wire        it_ge = !it_d[32];

// the one adder of the 32-bit magnitudes (zmag's): x + (y ^ inv) + cin, for the add's sum (S_ADD2), the integer's negation
// (S_DECIDE: cvt.s.w, a word of cvt.s.l; S_LNEG: the lower word of the other cvt.s.l), the upper word's negation with the
// lower one's carry (S_LNEG2) and the rounded integer of a conversion (S_F2W2). ax still holds the lower word in S_LNEG (the
// state before copied it into ir)
logic [31:0] za_x, za_y; logic za_inv, za_cin, f2w_inc;
always_comb begin
    case (rmode)
    2'd0: f2w_inc = fg && (zst || zmag[0]);
    2'd1: f2w_inc = 1'b0;
    2'd2: f2w_inc = !ax.s && (fg || zst);
    default: f2w_inc = ax.s && (fg || zst);
    endcase
    za_x = 32'd0; za_y = zmag; za_inv = 1'b0; za_cin = 1'b0;
    case (st)
    S_ADD2:  begin za_x = {1'b0, 1'b1, add_lf, 7'd0}; za_y = za_b; za_inv = sub_eff; za_cin = sub_eff; end
    S_DECIDE: begin za_y = ax; za_inv = ax[31]; za_cin = ax[31]; end
    S_LNEG:  begin za_y = ax; za_inv = 1'b1; za_cin = 1'b1; end
    S_LNEG2: begin za_inv = zs; za_cin = zs && lcar; end
    S_F2W2:  begin za_inv = ax.s; za_cin = f2w_inc ^ ax.s; end
    default: ;
    endcase
end
wire [32:0] za_t = {za_x, 1'b1} + {za_y ^ {32{za_inv}}, za_cin};      // (the carry in as the low bit: one carry chain)
wire [31:0] za_s = za_t[32:1];
// S_LNEG2's sum is zero (the upper word is gone), found from zmag, not from the adder's sum: ~zmag + lcar, or zmag
wire        za_z = zs ? (lcar ? zmag == 32'd0 : zmag == 32'hFFFFFFFF) : zmag == 32'd0;

// (FAST) zmag's left shifts, one barrel: S_NORM2's by the count of leading zeros, and the shift by one of the quotient / root
// (S_ITER: the bit of the iteration comes in) and of cvt.s.l's loop (S_LSH1: ir's top bit comes in)
wire [5:0]  zsh_n = (st == S_ITER || st == S_LSH1 || !FAST) ? 6'd1 : nzc;
wire        zsh_in = (st == S_ITER && it_ge) || (st == S_LSH1 && ir[31]);
wire [31:0] zsh = (zmag << zsh_n) | {31'd0, zsh_in};

// zmag's next value: one-hot selects (z_*) of the sources AND-ORed, written when any is selected (or cleared: z_clr)
//   xo     S_IDLE: cvt.s.l's upper word, a double's upper half
//   ax     S_DECIDE, cvt.s.l of an unsigned word
//   za     the adder's sum: the add (S_ADD2), an integer's magnitude (S_DECIDE), the upper word's (S_LNEG2)
//   sh     the align shifter's output, with the sticky in bit 0 for the add (S_ADDA), as it is for to-int (S_F2W)
//   zsh    the barrel (above)
//   mp     the product's upper word (S_NORM of mul)
//   ir     S_LNEG2: the lower word when the upper one has none left
//   nf     (!FAST) S_NORM's shift
// S_DECIDE writes it for every operation that is not cvt.s.l (a cleared zmag is what div and sqrt start from; for the others it
// is not read before it is written again)
logic z_xo, z_ax, z_za, z_sh, z_zsh, z_mp, z_ir, z_nf, z_clr;
always_comb begin
    z_xo = 0; z_ax = 0; z_za = 0; z_sh = 0; z_zsh = 0; z_mp = 0; z_ir = 0; z_nf = 0; z_clr = 0;
    if (!rst && live) case (st)
    S_IDLE:   z_xo = FAST ? ar_q : is_arith;
    S_DECIDE: if (!sel_l) begin z_za = sel_w; z_clr = !sel_w; end
              else if (zmag == 32'd0) z_ax = 1;
              else if (zmag == {32{ax[31]}}) z_za = 1;
    S_ADDA:   z_sh = 1;
    S_ADD2:   z_za = 1;
    S_ITER:   z_zsh = 1;
    S_NORM:   if (FAST) z_mp = kind == K_MUL; else z_nf = 1;
    S_LNEG2:  if (za_z) z_ir = 1; else z_za = 1;
    S_LSH1:   z_zsh = !zmag[31];
    S_NORM2:  z_zsh = 1;
    S_F2W:    z_sh = FAST;
    default: ;
    endcase
end
wire [31:0] zm_nf = (kind == K_MUL ? mprod[47:16] : zmag);
wire [31:0] zmag_d = ({32{z_xo}} & xo) | ({32{z_ax}} & ax) | ({32{z_za}} & za_s)
                   | ({32{z_sh}} & {sh_o[34:4], sh_o[3] | (st == S_ADDA && (sh_o[2] | sh_o[1] | sh_o[0]))})
                   | ({32{z_zsh}} & zsh) | ({32{z_mp}} & mprod[47:16]) | ({32{z_ir}} & ir) | ({32{z_nf}} & (zm_nf << lzc32(zm_nf)));
always_ff @(posedge clk) if (z_xo || z_ax || z_za || z_sh || z_zsh || z_mp || z_ir || z_nf || z_clr) zmag <= zmag_d;

// zexp's next value, likewise
//   158, 190   the exponent of an integer's conversion (S_DECIDE of cvt.s.w / cvt.s.l); of cvt.s.l's 64 bits (S_LNEG2)
//   md         mul's and div's: e + e' - 126, e - e' + 131 (S_DECIDE: the special cases do not read it), one adder (the count
//              and the constant of the second are the first's complement and 132)
//   sq         sqrt's: e / 2 rounded down + 68
//   a1         the add's: the larger operand's + 1 (S_ADD2)
//   dec        less the count of the shift (S_NORM2), or by one (S_LSH1): zsh's count
//   nf         (!FAST) S_NORM's: less the leading zeros
logic x_158, x_md, x_sq, x_a1, x_190, x_dec, x_nf;
always_comb begin
    x_158 = 0; x_md = 0; x_sq = 0; x_a1 = 0; x_190 = 0; x_dec = 0; x_nf = 0;
    if (!rst && live) case (st)
    S_DECIDE: if (sel_l || sel_w) x_158 = 1;
              else begin x_md = fn[5:1] == 5'b00001; x_sq = fn == 6'h04; end
    S_ADD2:   x_a1 = 1;
    S_NORM:   x_nf = !FAST;
    S_LNEG2:  x_190 = !za_z;
    S_LSH1:   x_dec = !zmag[31];
    S_NORM2:  x_dec = 1;
    default: ;
    endcase
end
wire [11:0] zx_md = 12'(ax.e) + (12'(ay.e) ^ {12{fn[0]}}) + (fn[0] ? 12'd132 : 12'(-126));
wire [11:0] zexp_d = ({12{x_158}} & 12'd158) | ({12{x_190}} & 12'd190) | ({12{x_md}} & zx_md) | ({12{x_sq}} & (12'(ax.e[7:1]) + 12'd68))
                   | ({12{x_a1}} & (12'(add_le) + 12'd1)) | ({12{x_dec}} & (zexp - 12'(zsh_n))) | ({12{x_nf}} & (zexp - 12'(lzc32(zm_nf))));
always_ff @(posedge clk) if (x_158 || x_md || x_sq || x_a1 || x_190 || x_dec || x_nf) zexp <= zexp_d;

// ir's next value, likewise
//   yo     (FAST) S_IDLE: a double's upper half (ft's)
//   ax     S_DECIDE, cvt.s.l of 64 bits: the lower word
//   dv     div's dividend, {1, fraction} (S_DECIDE of div; the special cases do not read it)
//   it     the iteration's remainder (div: shifted left by one)
//   za     S_LNEG: the lower word's magnitude
//   sh     S_LSH1: shifted left by one, the bit going to zmag
// S_DECIDE clears it for every operation that is not div or cvt.s.l (sqrt starts from zero; the others do not read it again)
logic i_yo, i_ax, i_dv, i_it, i_za, i_sh, i_clr;
always_comb begin
    i_yo = 0; i_ax = 0; i_dv = 0; i_it = 0; i_za = 0; i_sh = 0; i_clr = 0;
    if (!rst && live) case (st)
    S_IDLE:   i_yo = FAST && ar_q;
    S_DECIDE: if (sel_l) i_ax = zmag != 32'd0 && zmag != {32{ax[31]}};
              else if (fn == 6'h03) i_dv = 1;
              else i_clr = 1;
    S_ITER:   i_it = 1;
    S_LNEG:   i_za = zs;
    S_LSH1:   i_sh = !zmag[31];
    default: ;
    endcase
end
wire [31:0] ir_sel = it_ge ? it_d[31:0] : it_a;
wire [31:0] ir_d = ({32{i_yo}} & yo) | ({32{i_ax}} & ax) | ({32{i_dv}} & {8'd0, 1'b1, ax.f})
                 | ({32{i_it}} & (kind == K_DIV ? {ir_sel[30:0], 1'b0} : ir_sel)) | ({32{i_za}} & za_s) | ({32{i_sh}} & {ir[30:0], 1'b0});
always_ff @(posedge clk) if (i_yo || i_ax || i_dv || i_it || i_za || i_sh || i_clr) ir <= ir_d;

// round (zmag normalised: bit 31 set) and pack; FTZ, overflow to infinity
wire [24:0] rm = {1'b0, zmag[31:8]} + {24'd0, zmag[7] && (zst || zmag[6:0] != 0 || zmag[8])};
wire signed [11:0] rexp = zexp + 12'(rm[24]);
wire [31:0] rbits = rexp >= 255 ? {zs, 8'hFF, 23'd0}
                  : rexp <= 0   ? {zs, 31'd0}
                  :               {zs, rexp[7:0], rm[24] ? rm[23:1] : rm[22:0]};

// res's next value. The sources, one-hot as the others' (a field at a time: the sign, the exponent, the fraction):
//   ax, y, abs, neg  an operand as it is (y: the add's second, its sign by sub), its magnitude, its sign changed
//   qnan, satp, satn, inf, zsg  the constants: 7FBFFFFF, 7FFFFFFF, 80000000, infinity and zero of a sign (mul / div: the signs'
//                    xor; add: both operands' zero: their and)
//   rb               S_ROUND's packed result       za   S_F2W2's integer (FAST)      nf   (!FAST) S_F2W's
// and zero (a write with nothing selected: x - x, a zero integer, ...)
wire dnx = FAST ? fnx : isnan(ax), dny = FAST ? fny : isnan(ay), dix = FAST ? fix : isinf(ax), diy = FAST ? fiy : isinf(ay);
wire dzx = FAST ? fzx : iszero(ax), dzy = FAST ? fzy : iszero(ay);
wire d_toint = (fn >= 6'h08 && fn <= 6'h0F) || fn == 6'h24 || fn == 6'h25;
wire dsy = ay.s ^ fn[0];
logic r_ax, r_y, r_abs, r_neg, r_qnan, r_satp, r_satn, r_inf, r_zsg, r_rb, r_za, r_nf, r_zero;
always_comb begin
    r_ax = 0; r_y = 0; r_abs = 0; r_neg = 0; r_qnan = 0; r_satp = 0; r_satn = 0; r_inf = 0; r_zsg = 0; r_rb = 0; r_za = 0; r_nf = 0; r_zero = 0;
    if (!rst && live) case (st)
    S_DECIDE:
        if (sel_l) r_zero = zmag == 32'd0 && ax == 32'd0;
        else if (sel_w) r_zero = ax == 32'd0;
        else if (fn >= 6'h30) ;
        else if (d_toint) begin
            if (dnx) r_satp = 1;
            else if (ax.e >= 8'd158) begin r_satn = ax.s; r_satp = !ax.s; end
            else r_zero = dzx;
        end else case (fn)
        6'h00, 6'h01:
            if (dnx || dny || (dix && diy && ax.s != dsy)) r_qnan = 1;
            else if (dix) r_ax = 1;
            else if (diy) r_y = 1;
            else if (dzx && dzy) r_zsg = 1;
            else if (dzx) r_y = 1;
            else r_ax = dzy;
        6'h02:
            if (dnx || dny || (dix && dzy) || (dzx && diy)) r_qnan = 1;
            else if (dix || diy) r_inf = 1;
            else r_zsg = dzx || dzy;
        6'h03:
            if (dnx || dny || (dix && diy) || (dzx && dzy)) r_qnan = 1;
            else if (dix || dzy) r_inf = 1;
            else r_zsg = dzx || diy;
        6'h04:
            if (dnx || (ax.s && !dzx)) r_qnan = 1;
            else r_ax = dzx || dix;
        default:
            if (dnx) r_qnan = 1;
            else if (fn == 6'h05) r_abs = 1;
            else if (fn == 6'h07) r_neg = 1;
            else r_ax = 1;
        endcase
    S_ADD2:  r_zero = !FAST && za_s == 32'd0;
    S_NORM:  r_zero = FAST && kind == K_ADD && zmag == 32'd0;
    S_ROUND: r_rb = 1;
    S_F2W:   r_nf = !FAST;
    S_F2W2:  r_za = 1;
    default: ;
    endcase
end
wire [31:0] nf_i0 = sh_o[34:3];
logic nf_inc;
always_comb case (rmode)
    2'd0: nf_inc = sh_o[2] && (sh_o[1] || sh_o[0] || nf_i0[0]);
    2'd1: nf_inc = 1'b0;
    2'd2: nf_inc = !ax.s && (sh_o[2] || sh_o[1] || sh_o[0]);
    default: nf_inc = ax.s && (sh_o[2] || sh_o[1] || sh_o[0]);
endcase
wire [31:0] nf_i = nf_i0 + {31'd0, nf_inc};
wire [31:0] nf_res = ax.s ? -nf_i : nf_i;
// (the constants' sign: an infinity or a zero of mul / div has the signs' xor, an add's zero both operands')
wire        r_sg = fn[1] ? ax.s ^ ay.s : ax.s & dsy;
wire        r_axef = r_ax | r_abs | r_neg;
wire [31:0] res_d_n = {({r_ax & ax.s} | {r_y & dsy} | {r_neg & !ax.s} | {r_satn} | {(r_inf | r_zsg) & r_sg} | {r_rb & rbits[31]} | {r_za & za_s[31]} | {r_nf & nf_res[31]}),
                       ({8{r_axef}} & ax.e) | ({8{r_y}} & ay.e) | ({8{r_rb}} & rbits[30:23]) | ({8{r_za}} & za_s[30:23]) | ({8{r_nf}} & nf_res[30:23]) | {8{r_qnan | r_satp | r_inf}},
                       ({23{r_axef}} & ax.f) | ({23{r_y}} & ay.f) | ({23{r_rb}} & rbits[22:0]) | ({23{r_za}} & za_s[22:0]) | ({23{r_nf}} & nf_res[22:0])
                       | {23{r_satp}} | {1'b0, {22{r_qnan}}}};
always_ff @(posedge clk) if (r_ax || r_y || r_abs || r_neg || r_qnan || r_satp || r_satn || r_inf || r_zsg || r_rb || r_za || r_nf || r_zero) res <= res_d_n;


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
function automatic logic iszero(input fbits_t x); return {x.e, x.f} == 31'd0; endfunction   // (operands are flushed but a compare's)

// ============================================================ the FSM
always_ff @(posedge clk) begin
    if (rst) begin
        st <= S_IDLE; fcr31 <= 32'h01000800;
    end else if (!live) st <= S_IDLE;                     // flushed: its work is dropped
    else case (st)
    S_IDLE: if (FAST ? ar_q : is_arith) begin
        ax <= up; st <= S_UNPY;
        if (FAST) begin
            ax <= (isd_q || isl_q) ? xe : rfs; ay <= isd_q ? ye : rft;
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
            kind <= K_CVT; zst <= 0;
            if (zmag == 32'd0 && ax == 32'd0) ;
            else if (zmag == 32'd0) begin zs <= 0; st <= S_NORM; end                    // an unsigned word
            else if (zmag == {32{ax[31]}}) begin zs <= ax[31]; st <= S_NORM; end   // a word
            else begin zs <= zmag[31]; st <= S_LNEG; end                                  // 64 bits
        end else if (sel_w) begin                                  // cvt.s.w, cvt.d.w: ax is the integer
            kind <= K_CVT; zs <= ax.s; zst <= 0;
            if (ax != 32'd0) st <= S_NORM;
        end else if (fn >= 6'h30) begin                   // c.cond
            logic un, eq, lt;
            un = nx || ny;
            eq = !un && ((zx && zy) || (FAST ? fmeq && ax.s == ay.s : ax == ay));
            lt = !un && !(zx && zy) && (ax.s != ay.s ? ax.s : (ax.s ? (FAST ? !fmlt && !fmeq : {ax.e, ax.f} > {ay.e, ay.f}) : (FAST ? fmlt : {ax.e, ax.f} < {ay.e, ay.f})));
            kind <= K_CMP; rcond <= (fn[0] && un) || (fn[1] && eq) || (fn[2] && lt);
        end else if ((fn >= 6'h08 && fn <= 6'h0F) || fn == 6'h24 || fn == 6'h25) begin   // to int (.w, and .l as .w)
            kind <= K_F2W;
            if (!nx && ax.e < 8'd158 && !zx) st <= S_F2W;
            if (FAST) ad <= 8'd158 - ax.e;
        end else case (fn)
        6'h00, 6'h01: begin                               // add, sub: order by magnitude
            logic sy; fbits_t y;
            y = ay; y.s = ay.s ^ fn[0]; sy = y.s;
            kind <= K_ADD;
            if (!(nx || ny || ix || iy || zx || zy)) begin
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
            if (!(nx || ny || ix || iy || zx || zy)) st <= S_MUL;
        end
        6'h03: begin                                      // div
            kind <= K_DIV;
            if (!(nx || ny || ix || iy || zx || zy)) begin
                icnt <= 5'd27;
                zs <= ax.s ^ ay.s;
                st <= S_ITER;
            end
        end
        6'h04: begin                                      // sqrt
            kind <= K_SQRT;
            if (!(nx || (ax.s && !zx) || zx || ix)) begin
                icnt <= 5'd27; zs <= 0;
                // value = m * 2^t, t = e - 150 (as odd as e is); the radicand M * 2^30, M = m (t even) or m << 1 (t odd): a 28-bit
                // root, whose exponent is t / 2 rounded down, + 143: e / 2 rounded down, + 68
                if (ax.e[0]) mp <= {1'd0, 1'b1, ax.f, 31'd0};
                else         mp <= {2'd0, 1'b1, ax.f, 30'd0};
                st <= S_ITER;
            end
        end
        default: begin                                    // abs, neg, cvt.s.d, cvt.d.s
            kind <= K_MOVE;
        end
        endcase
    end
    S_ADDA: st <= S_ADD2;
    S_ADD2: begin                                         // align, add: the larger at [30:7]
        zst <= 0;
        if (FAST) st <= S_NORM;                                   // (x - x: S_NORM's)
        else if (za_s == 32'd0) st <= S_DONE;   // x - x: +0
        else st <= S_NORM;
    end
    S_MUL: begin
        // (the product: mprod, above)
        zs <= ax.s ^ ay.s;
        st <= S_NORM;
    end
    S_ITER: begin
        if (kind != K_DIV) mp <= {mp[53:0], 2'b00};
        icnt <= icnt - 5'd1;
        if (icnt == 5'd0) st <= S_NORM;
    end
    S_NORM: begin
        logic [31:0] m; logic sti;
        if (kind == K_MUL) begin m = mprod[47:16]; sti = mprod[15:0] != 0; end
        else if (kind == K_DIV || kind == K_SQRT) begin m = zmag; sti = ir != 0; end
        else begin m = zmag; sti = zst; end
        if (FAST) begin nzc <= lzc32(m); zst <= sti; st <= S_NORM2; end         // (the count, then the shift)
        else begin zst <= sti; st <= S_ROUND; end
        if (FAST && kind == K_ADD && zmag == 32'd0) st <= S_DONE;
    end
    // cvt.s.l of one that is not a word: the magnitude {zmag, ir} a word at a time (the lower word, then the upper one with
    // its carry), then shifted left a bit a cycle until its top bit is set (at most 32 cycles), the bits below the upper
    // word kept as the sticky
    S_LNEG: begin
        if (zs) lcar <= ir == 32'd0;
        st <= S_LNEG2;
    end
    S_LNEG2: begin
        if (za_z) st <= S_NORM;
        else st <= S_LSH1;
    end
    S_LSH1: if (zmag[31]) begin zst <= ir != 32'd0; st <= S_ROUND; end
    S_NORM2: st <= S_ROUND;
    S_ROUND: st <= S_DONE;
    S_F2W: if (FAST) begin fg <= sh_o[2]; zst <= sh_o[1] | sh_o[0]; st <= S_F2W2; end
    else st <= S_DONE;                                    // |x| < 2^31: nf_res
    S_F2W2: begin
        st <= S_DONE;                                     // (the rounded magnitude, negated for a negative: -(m + inc) is ~m + !inc -- the shared adder)
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
logic [31:0] ld_lo;
always_comb res_d = f2d(res);
always_comb begin
    gpr_out = is_mfc1 ? rfs : is_dmfc1 ? xe : (insn[15:11] == 5'd31 ? fcr31 : insn[15:11] == 5'd0 ? 32'h00000B00 : 32'd0);
    gpr_out_hi = xo;
    mem_out = is_sdc1 ? {yo, ye} : {32'd0, rft};
    cond = is_ctc1 ? gpr_in[23] : (is_arith && kind == K_CMP) ? rcond : fcr31[23];

    we_e = 0; we_o = 0; widx = 0;
    if (valid) begin
        if (is_mtc1) begin widx = insn[15:12]; if (insn[11]) we_o = 1; else we_e = 1; end
        if (is_dmtc1) begin widx = insn[15:12]; we_e = 1; we_o = 1; end   // (FR=0: the pair)
        if (is_lwc1) begin widx = insn[20:17]; if (insn[16]) we_o = 1; else we_e = 1; end
        if (is_ldc1) begin widx = insn[20:17]; we_e = 1; we_o = 1; end
        if (is_mov) begin
            widx = insn[10:7];
            if (is_d) begin we_e = 1; we_o = 1; end
            else if (insn[6]) we_o = 1; else we_e = 1;
        end
        if (is_arith && kind != K_CMP) begin
            widx = insn[10:7];
            if (dst_l || dst_d) begin we_e = 1; we_o = 1; end
            else if (insn[6]) we_o = 1; else we_e = 1;
        end
    end
    // the data of the registers written (what the others hold is not used: the write goes by we_e / we_o; the trace is read by
    // them too). A load of a word is mtc1's data or lwc1's, one selection for the even and the odd register
    ld_lo = (is_lwc1 || is_ldc1) ? mem_in[31:0] : gpr_in;
    if (is_mtc1 || is_dmtc1 || is_lwc1 || is_ldc1) wd_e = ld_lo;
    else if (is_mov) wd_e = (is_d || !insn[11]) ? xe : xo;
    else wd_e = dst_d ? res_d[63:32] : res;
    if (is_mtc1 || is_lwc1) wd_o = ld_lo;
    else if (is_dmtc1) wd_o = gpr_in_hi;
    else if (is_ldc1) wd_o = mem_in[63:32];
    else if (is_mov) wd_o = (is_d || insn[11]) ? xo : xe;
    else wd_o = dst_l ? {32{res[31]}} : dst_d ? res_d[31:0] : res;
end

// the trace: registered with the write, so it lines up with the core's retire (W)
always_ff @(posedge clk) begin
    tr_we_even <= we_e; tr_we_odd <= we_o; tr_widx <= widx; tr_weven <= wd_e; tr_wodd <= wd_o;
    tr_fcr31 <= (valid && is_ctc1 && insn[15:11] == 5'd31) ? gpr_in & 32'h0183FFFF
              : (valid && is_arith && kind == K_CMP) ? {fcr31[31:24], rcond, fcr31[22:0]} : fcr31;
end

endmodule
`default_nettype wire
