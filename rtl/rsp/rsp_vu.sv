// rsp_vu: the RSP's vector unit (COP2) and its loads and stores (LWC2, SWC2).
//
// 32 registers of 8 lanes of 16 bits, a 48-bit accumulator per lane, the
// flags VCO, VCC and VCE, the reciprocal unit. The contract is the reference
// model, sim/rsp/rsp.c: the same state after each instruction.
//
// LANES (8 or 4) is how many lanes the datapath holds: with 4, a computational
// instruction runs in two passes (lanes 0-3, then 4-7) on half the
// multipliers and adders. An instruction arrives from the scalar unit's E
// stage (valid, held until done):
//   computational   operands and products (a cycle), then the accumulator,
//                   the clamps and the flags (a cycle per pass); all eight
//                   result lanes are written at the end, so vd may be vs or vt
//   VRCP.. VRSQH    with them, the reciprocal ROM (three cycles in all)
//   MFC2 MTC2 CFC2 CTC2   one cycle
//   loads, stores   one cycle (DMEM is 16 bytes wide, as the N64's: what one
//                   moves lies in the 16 bytes from its address's word on);
//                   a load's data comes the cycle after its address
//   a pair          (LPAIR: eight lanes) a computation on valid / ins and a
//                   load (LBV .. LUV) of another register on l_valid / l_ins
//                   (its base on a), the scalar unit's pair: both start in the same cycle
//                   (done for the two), the load's write first and the
//                   computation's a cycle late (wp), as behind any load
// The registers are LUT RAM (two copies, read in the cycle, written a byte at
// a time); a load writes its bytes over the register's the cycle after its
// address.
//
//   LTV, STV        a byte a cycle (every graphics microcode has them, none
//                   runs them often)
//   SHV, SFV        one cycle (a JPEG microcode's YUV packing)
// Not implemented (no microcode of the ROMs scanned has them -- sim/rsp/
// rom_ucode_scan.py; they do nothing and set `unimpl`): LHV LFV LWV, SWV, SFV
// at an element not a multiple of 4, VRNDP VRNDN VMULQ VMACQ and the reserved
// functions.
`default_nettype none

module rsp_vu #(
    parameter int LANES = 4,
    // (with LANES 4) the multiplies on all eight lanes at once: four more
    // multipliers and accumulator adders, and only the other operations take
    // two passes
    parameter bit MULW = 0
) (
    input  wire         clk,
    input  wire         rst,

    input  wire         valid,
    input  wire  [31:0] ins,
    input  wire  [31:0] a,              // rs: the base of a load / store
    input  wire         l_valid,        // (LPAIR) a load paired with the computation on ins
    input  wire  [31:0] l_ins,          // the scalar unit's E instruction: a load, store or move (ins as well), or a pair's load
    input  wire  [31:0] b,              // rt: MTC2, CTC2
    // the registers' read addresses, from the scalar unit: of the instruction E holds next cycle -- its rd field (vs),
    // and rd for MFC2 / MTC2 or rt for the rest (rb_n) -- and that last of the one in E now (rb)
    input  wire  [4:0]  ra_n,
    input  wire  [4:0]  rb_n,
    input  wire  [4:0]  rb,
    output logic        done,
    output logic [31:0] res,            // MFC2, CFC2

    // DMEM (rsp_top): 16 bytes a cycle, those from m_addr's word on. A store
    // writes the bytes of m_be with those of m_wdata, both by the byte's
    // address mod 16 (bit b, m_wdata[127 - 8b -: 8]); a load's data comes back
    // the next cycle rotated by m_rot (byte m of m_rdata is the one at address
    // mod 16 = m + m_rot), so byte m is the register's byte m
    output logic        m_valid,
    output logic        m_we,
    output logic [11:0] m_addr,
    output logic [15:0] m_be,
    output logic [127:0] m_wdata,
    output logic [3:0]  m_rot,
    input  wire  [127:0] m_rdata,
    input  wire         m_stall,        // DMEM's port is the DMA's this cycle: a load or store (each beat of LTV / STV) waits

    // the reciprocal ROM (rsp_top): 512 reciprocals, then 512 inverse square roots
    output logic [9:0]  rom_addr,
    input  wire  [15:0] rom_q,

    output logic        unimpl,
    output logic [3:0]  commit,         // (simulation) instructions whose effects are all in with this cycle
    output logic        commit_part     // ... but the accumulator and flags already have a later one's first pass
);

localparam int NP = 8 / LANES;          // passes
localparam bit MW = NP == 2 && MULW;    // the multiplies' other four lanes are there
localparam bit LPAIR = NP == 1;         // a computation and a load may come together (rsp_su's pairs)

// ---------------------------------------------------------------- decode --
wire [5:0] op   = ins[31:26];
wire       cop2 = op == 6'h12;
wire       vec  = cop2 && ins[25];
// (the moves, loads and stores are never a pair's computation: from l_ins, the scalar unit's E instruction, which ins
// only adds a select to)
wire       cop2_l = l_ins[31:26] == 6'h12 && !l_ins[25];
wire       mfc2 = cop2_l && l_ins[24:21] == 4'd0;
wire       cfc2 = cop2_l && l_ins[24:21] == 4'd2;
wire       mtc2 = cop2_l && l_ins[24:21] == 4'd4;
wire       ctc2 = cop2_l && l_ins[24:21] == 4'd6;
// (the load or store: l_ins, alone or a pair's load)
wire       lp   = LPAIR && l_valid;
wire [31:0] li  = l_ins;
wire [31:0] la  = a;
wire       lwc2 = li[31:26] == 6'h32;
wire       swc2 = li[31:26] == 6'h3A;
wire [4:0] lsop = li[15:11];
wire [3:0] el   = li[10:7];             // LWC2/SWC2/MFC2/MTC2: the element (a byte of the register)
wire [4:0] ltn  = li[20:16];            // LWC2/SWC2: the register
wire [3:0] ve   = ins[24:21];           // computational: vt's element
wire [4:0] vtn  = ins[20:16];
wire [4:0] vsn  = ins[15:11];
wire [4:0] vdn  = ins[10:6];
wire [5:0] fn   = ins[5:0];
wire [2:0] de   = ins[13:11];           // VRCP.., VMOV: the lane of vd written

wire fn_mul  = fn[5:4] == 2'b00 && fn[2:1] != 2'b01;               // the multiplies (not VRNDP/VMULQ/VRNDN/VMACQ)
wire fn_rcp  = fn[5:3] == 3'b110 && fn[1:0] != 2'b11;              // VRCP VRCPL VRCPH VRSQ VRSQL VRSQH
wire fn_mov  = fn == 6'h33;
wire fn_one  = fn_rcp || fn_mov;                                   // writes one lane of vd
wire fn_none = fn == 6'h37 || fn == 6'h3F;                         // VNOP, VNULL
wire fn_known = fn_mul || fn_one || fn_none || fn == 6'h10 || fn == 6'h11 || fn == 6'h13 || fn == 6'h14 || fn == 6'h15
              || fn == 6'h1D || (fn >= 6'h20 && fn <= 6'h2D);

// ------------------------------------------------------------- registers --
// two copies (LUT RAM, read in the cycle, each at an address its memory registers a cycle ahead): A at vs, for the
// operands of a computational instruction; C at vt, for its other operands -- or at the register MFC2 / MTC2 name
// (rd), a store's (vt) or the one STV has reached, for their bytes. A write is of whole lanes or, a load's and
// MTC2's, of bytes: by an enable a byte, so nothing is read to keep the rest of a lane
logic [4:0]  wr_reg;
logic [15:0] wr_be;                   // the register's bytes written (bit m: byte m -- lane m / 2, its upper half first)
logic [15:0] wr_d [8];
wire  [4:0]  rc_n;                    // (below: C's address next cycle)
logic [15:0] qa [8], qc [8];
logic [127:0] wr_w, qa_w, qc_w;
logic [15:0]  wr_e;
always_comb
    for (int g = 0; g < 8; g++) begin
        wr_w[16 * g +: 16] = wr_d[g]; wr_e[2 * g + 1] = wr_be[2 * g]; wr_e[2 * g] = wr_be[2 * g + 1];
        qa[g] = qa_w[16 * g +: 16]; qc[g] = qc_w[16 * g +: 16];
    end
rsp_lutram ma (.clk, .we(wr_e), .wa(wr_reg), .wd(wr_w), .ra_n(ra_n), .q(qa_w));
rsp_lutram mc (.clk, .we(wr_e), .wa(wr_reg), .wd(wr_w), .ra_n(rc_n), .q(qc_w));
`ifdef VERILATOR
// (simulation) the registers where the testbench can read them
logic [15:0] shadow [0:31][0:7] /* verilator public_flat_rd */;
always_ff @(posedge clk)
    for (int g = 0; g < 8; g++) begin
        if (wr_be[2 * g])     shadow[wr_reg][g][15:8] <= wr_d[g][15:8];
        if (wr_be[2 * g + 1]) shadow[wr_reg][g][7:0]  <= wr_d[g][7:0];
    end
`endif

logic [15:0] acc_h [8] /* verilator public_flat_rd */;
logic [15:0] acc_m [8] /* verilator public_flat_rd */;
logic [15:0] acc_l [8] /* verilator public_flat_rd */;
logic [15:0] vco /* verilator public_flat_rd */;
logic [15:0] vcc /* verilator public_flat_rd */;
logic [7:0]  vce /* verilator public_flat_rd */;
logic [15:0] div_in_h;
logic [31:0] div_out;
logic        dph;

// --------------------------------------------------- the lanes' datapath --
typedef struct packed {
    logic [15:0] res;
    logic [47:0] acc;
    logic        acc_w, accl_w;          // write all 48 bits / the low 16
    logic        co_lo, co_hi, co_w;
    logic        cc_lo, cc_hi, cc_w;
    logic        ce, ce_w;
    logic [3:0]  fl;                     // the adder's {carry, b16, zero, ones}
} lane_t;

// a multiply's result: the accumulator's middle word, or its low word (VMUDL, VMUDN, VMADL, VMADN: k 4, 6), or a
// constant where the word does not hold the value -- 7FFF / 8000, VMULU's and VMACU's (k 1) FFFF / 0000, the low
// word's FFFF / 0000: its top bit and its other fifteen
function automatic logic [15:0] mul_res(input logic [2:0] k, input logic [47:0] a);
    logic gt, lt, klo, kc, c15, clo;
    gt = !a[47] && |a[46:31];
    lt = a[47] && !(&a[46:31]);
    klo = k == 3'd4 || k == 3'd6;
    if (k == 3'd1) begin kc = a[47] || gt; c15 = !a[47]; clo = !a[47]; end
    else if (klo)  begin kc = gt || lt;    c15 = gt;     clo = gt;     end
    else           begin kc = gt || lt;    c15 = !gt;    clo = gt;     end
    return kc ? {c15, {15{clo}}} : klo ? a[15:0] : a[31:16];
endfunction

// what the lanes' adder does, by the instruction (kept with it, x_am: the adder is the start of EX's longest path):
// 1 VADD, 2 VSUB, 3 VADDC, 4 VCH, 5 VCL, 6 VCR, 0 the rest (s - t)
function automatic logic [2:0] amode(input logic [5:0] f);
    unique case (f)
    6'h10:   return 3'd1;
    6'h11:   return 3'd2;
    6'h14:   return 3'd3;
    6'h25:   return 3'd4;
    6'h24:   return 3'd5;
    6'h26:   return 3'd6;
    default: return 3'd0;
    endcase
endfunction

function automatic lane_t lane_ex(input logic [5:0] f, input logic [2:0] am, input logic [3:0] e, input logic [15:0] s, input logic [15:0] t,
                                  input logic signed [33:0] p, input logic [15:0] vs, input logic [47:0] accm,
                                  input logic co_lo, input logic co_hi, input logic cc_lo, input logic cc_hi, input logic ce,
                                  input logic [3:0] fr);    // (the adder's flags of the cycle before: VCL, VCH and VCR's with one pass)
    lane_t o = '0;
    logic signed [47:0] term, accn;
    logic eq, sign;
    logic cs, negside, cond, le_f, ge_f, c_l, c_x, abs_sat, ovf, a_nt;
    logic [1:0] pk;
    logic [3:0] tt;
    logic [15:0] accl;
    // the result, one of four by the lane's select: 0 the accumulator's new low word (a multiply's sum's, or the word
    // picked or made), 1 the sum's middle word, 2 VSAR's word of the accumulator, 3 a clamp's constant (its top bit, the rest)
    logic [1:0] rsel;
    logic       c15, clo;
    // one adder for every add, subtract and compare: s + t or s - t, with the
    // 17-bit signed result's top bit (b16) and the carry (unsigned: no borrow)
    logic        sub, cin, cout, b16, zero, ones;
    logic        cout_c, b16_c, zero_c, ones_c, z17_c, eq_c;            // (VCL, VCH and VCR's: below)
    logic [15:0] bb, lo, nt;
    logic        cin_x;
    nt = -t;
    sign = s[15] ^ t[15];
    unique case (am)
    3'd1:    begin sub = 1'b0;   cin = co_lo;  end          // VADD
    3'd2:    begin sub = 1'b1;   cin = !co_lo; end          // VSUB
    3'd3:    begin sub = 1'b0;   cin = 1'b0;   end          // VADDC
    3'd4:    begin sub = !sign;  cin = !sign;  end          // VCH: s + t if the signs differ, else s - t
    3'd5:    begin sub = !co_lo; cin = !co_lo; end          // VCL: the same, by VCO
    3'd6:    begin sub = !sign;  cin = 1'b1;   end          // VCR: s + t + 1, or s - t
    default: begin sub = 1'b1;   cin = 1'b1;   end          // VSUBC, the compares
    endcase
    bb = sub ? ~t : t;
    {cout, lo, cin_x} = {1'b0, s, 1'b1} + {1'b0, bb, cin};               // (s + bb + cin: the carry in as the chain's own bit)
    b16 = s[15] ^ bb[15] ^ cout;
    zero = lo == 16'd0; ones = &lo;
    o.fl = {cout, b16, zero, ones};
    eq = zero;                                               // (s == t: where it is asked the adder subtracts)
    // VCL, VCH and VCR's flags: with one pass always the adder's of the cycle before (fr: their EX waits a cycle for
    // them, x_wait), so nothing of this cycle's adder is in front of the select they make -- with a flag that chose
    // between the two (as one did), the adder and its zero stood in front of every clip's select and were the longest
    // path into the registers' write; with two passes, this cycle's
    if (NP == 1) {cout_c, b16_c, zero_c, ones_c} = fr;
    else         {cout_c, b16_c, zero_c, ones_c} = {cout, b16, zero, ones};
    z17_c = zero_c && !b16_c;                                // the signed result is 0
    eq_c  = zero_c;
    // the multiplies: the product into the accumulator (the accumulator's high words
    // are taken from here when the instruction is one -- acc_w -- and are no matter
    // otherwise), the clamped result
    begin
        logic [47:0] pe;
        logic s1, s4, s5, s7;
        pe = 48'(p);
        s1 = f[2:0] == 3'd0 || f[2:0] == 3'd1;
        s4 = f[2:0] == 3'd4;
        s5 = f[2:0] == 3'd5 || f[2:0] == 3'd6;
        s7 = f[2:0] == 3'd7;
        term = ({48{s1}} & (pe << 1)) | ({48{s4}} & {32'd0, p[31:16]}) | ({48{s5}} & pe) | ({48{s7}} & (pe << 16));
    end
    accn = term + (f[3] ? $signed(accm) : (f[2:1] == 2'b00) ? 48'sh8000 : 48'sd0);
    o.acc = accn;
    if (f[5:4] == 2'b00) begin
        // (the multiplies' clamp, as mul_res)
        logic gt, lt, klo;
        o.acc_w = 1'b1;
        gt = !accn[47] && |accn[46:31];
        lt = accn[47] && !(&accn[46:31]);
        klo = f[2:0] == 3'd4 || f[2:0] == 3'd6;
        rsel = klo ? 2'd0 : 2'd1;
        if (f[2:0] == 3'd1) begin if (accn[47] || gt) rsel = 2'd3; c15 = !accn[47]; clo = !accn[47]; end
        else if (klo)       begin if (gt || lt) rsel = 2'd3;       c15 = gt;        clo = gt;        end
        else                begin if (gt || lt) rsel = 2'd3;       c15 = !gt;       clo = gt;        end
    end else begin
        // The result is picked (s, t, -t or ~t), taken from the adder (lo), made by a
        // logic operation or, for VSAR, from the accumulator; VADD, VSUB and VABS
        // clamp it on its way to vd, and the accumulator's low word takes it unclamped.
        // What is picked or made of s and t alone -- s, t, ~t, 0, the logic operations --
        // is a function of the lane's two bits: a table of its four values (tt, the same
        // for the lane's sixteen bits), beside the adder's sum and -t.
        o.accl_w = f != 6'h1D;
        c_l = f == 6'h10 || f == 6'h11 || f == 6'h14 || f == 6'h15;
        c_x = f >= 6'h28 && f <= 6'h2D;
        // the compares: b16 is s < t (signed)
        unique case (f)
        6'h20:   cs = b16 || (eq && co_hi && co_lo);
        6'h23:   cs = (!b16 && !eq) || (eq && !(co_hi && co_lo));
        default: cs = cc_lo;                                 // VMRG
        endcase
        // VCH, VCL, VCR: s or t (or -t, ~t) by what the compare found. A lane's
        // conditions: le (s <= -t) and ge (s >= t), and which of them decides
        if (f == 6'h24) begin                                // VCL: by VCO, the unsigned compare (cout is s >= t)
            le_f = (co_lo && !co_hi) ? (ce ? (zero_c || !cout_c) : (zero_c && !cout_c)) : cc_lo;
            ge_f = (!co_lo && !co_hi) ? cout_c : cc_hi;
            negside = co_lo;
        end else begin                                       // VCH, VCR: by the signs
            le_f = sign ? (b16_c || z17_c) : t[15];
            ge_f = sign ? t[15] : !b16_c;
            negside = sign;
        end
        cond = negside ? le_f : ge_f;
        unique case (f)
        6'h13:                    pk = s[15] ? 2'd2 : 2'd1;
        6'h20, 6'h23, 6'h27:      pk = cs ? 2'd0 : 2'd1;
        6'h22:                    pk = 2'd0;
        6'h24, 6'h25, 6'h26:      pk = !cond ? 2'd0 : !negside ? 2'd1 : (f == 6'h26) ? 2'd3 : 2'd2;
        default:                  pk = 2'd1;                 // VEQ, VRCP.., VMOV: vt
        endcase
        unique case (pk)
        2'd0:    tt = 4'b1100;                               // s
        2'd1:    tt = 4'b1010;                               // t
        default: tt = 4'b0101;                               // ~t (-t: a_nt)
        endcase
        if (f == 6'h13 && s == 16'd0) tt = 4'b0000;          // VABS of 0
        if (c_x) begin
            unique case (f[2:1])
            2'd0:    tt = 4'b1000;                           // s & t
            2'd1:    tt = 4'b1110;                           // s | t
            default: tt = 4'b0110;                           // s ^ t
            endcase
            tt = tt ^ {4{f[0]}};
        end
        a_nt = !c_x && pk == 2'd2;
        for (int i = 0; i < 16; i++) accl[i] = c_l ? lo[i] : a_nt ? nt[i] : tt[{s[i], t[i]}];
        o.acc[15:0] = accl;
        // vd: the clamped result, or VSAR's word of the accumulator
        abs_sat = f == 6'h13 && s[15] && t == 16'h8000;
        ovf = (f == 6'h10 || f == 6'h11) && (b16 ^ lo[15]);
        rsel = (abs_sat || ovf) ? 2'd3 : 2'd0;
        c15 = !abs_sat && b16;                               // (abs_sat: 7FFF; ovf: b16, then its inverse)
        clo = abs_sat || !b16;
        // (VSAR: the accumulator's word e names -- vs, the lane's -- or 0)
        if (f == 6'h1D) begin rsel = (e == 4'd8 || e == 4'd9 || e == 4'd10) ? 2'd2 : 2'd3; c15 = 1'b0; clo = 1'b0; end
        // the flags
        unique case (f)
        6'h10, 6'h11:  o.co_w = 1'b1;                                                                    // VADD, VSUB
        6'h14:         begin o.co_lo = cout; o.co_w = 1'b1; end                                          // VADDC
        6'h15:         begin o.co_lo = !cout; o.co_hi = !eq; o.co_w = 1'b1; end                          // VSUBC
        6'h20, 6'h23:  begin o.cc_lo = cs; o.cc_w = 1'b1; o.co_w = 1'b1; end
        6'h21:         begin o.cc_lo = eq && !co_hi; o.cc_w = 1'b1; o.co_w = 1'b1; end
        6'h22:         begin o.cc_lo = !eq || co_hi; o.cc_w = 1'b1; o.co_w = 1'b1; end
        6'h24, 6'h26:  begin o.cc_lo = le_f; o.cc_hi = ge_f; o.cc_w = 1'b1; o.co_w = 1'b1; o.ce_w = 1'b1; end
        6'h25: begin                                         // VCH
            o.ce = sign && ones_c && b16_c;
            o.co_hi = sign ? (!z17_c && !(ones_c && b16_c)) : !eq_c;
            o.co_lo = sign; o.co_w = 1'b1;
            o.cc_lo = le_f; o.cc_hi = ge_f; o.cc_w = 1'b1;
            o.ce_w = 1'b1;
        end
        6'h27:         o.co_w = 1'b1;                                                                    // VMRG
        default: ;
        endcase
    end
    for (int i = 0; i < 16; i++)
        unique case (rsel)
        2'd0:    o.res[i] = o.acc[i];
        2'd1:    o.res[i] = accn[16 + i];
        2'd2:    o.res[i] = vs[i];
        default: o.res[i] = (i == 15) ? c15 : clo;
        endcase
    return o;
endfunction

// a multiply's lane alone (lane_ex's first half): the lanes MULW adds
function automatic lane_t lane_mul(input logic [5:0] f, input logic signed [33:0] p, input logic [47:0] acc);
    lane_t o = '0;
    logic signed [47:0] term, accn;
    begin
        logic [47:0] pe;
        logic s1, s4, s5, s7;
        pe = 48'(p);
        s1 = f[2:0] == 3'd0 || f[2:0] == 3'd1;
        s4 = f[2:0] == 3'd4;
        s5 = f[2:0] == 3'd5 || f[2:0] == 3'd6;
        s7 = f[2:0] == 3'd7;
        term = ({48{s1}} & (pe << 1)) | ({48{s4}} & {32'd0, p[31:16]}) | ({48{s5}} & pe) | ({48{s7}} & (pe << 16));
    end
    accn = term + (f[3] ? $signed(acc) : (f[2:1] == 2'b00) ? 48'sh8000 : 48'sd0);
    o.acc = accn; o.acc_w = 1'b1;
    o.res = mul_res(f[2:0], accn);
    return o;
endfunction

// ------------------------------------------------- the computational pipeline --
// Two stages. SEL takes a pass's operands from the registers (vt through its
// element's broadcast) and multiplies them; EX, a cycle later, does the
// accumulator, the clamps and the flags of that pass, and with the last pass
// writes vd. The instruction leaves E as its last pass is taken (its EX runs
// behind the next instruction), and the next one's SEL may run under that EX
// -- unless it reads the register being written, or is not a computational
// instruction: those wait for the pipeline to empty. A reciprocal op waits
// for the EX of any instruction before it (they are serial through the
// divide registers), and with one pass its own EX takes a cycle more (the ROM).
// vt's element broadcast, for every lane at once: lane i takes the register's lane
// ve[2:0] (ve[3]), {i[2], ve[1:0]} (ve[2]), {i[2:1], ve[0]} (ve[1]) or i. The sources are shared between lanes: one eight-way choice for a
// single lane broadcast (ve[3]), one four-way for each half (ve[2]), one two-way
// for each pair (ve[1]); a lane then picks among those and its own.
logic [15:0] tv [8];
always_comb begin
    logic [15:0] t_b, t_h [2], t_p [4];
    t_b = qc[ve[2:0]];
    for (int h = 0; h < 2; h++) t_h[h] = qc[{1'(h), ve[1:0]}];
    for (int j = 0; j < 4; j++) t_p[j] = ve[0] ? qc[2 * j + 1] : qc[2 * j];
    for (int i = 0; i < 8; i++)
        tv[i] = ve[3] ? t_b : ve[2] ? t_h[i / 4] : ve[1] ? t_p[i / 2] : qc[i];
end
wire  fn_ch = fn == 6'h24 || fn == 6'h25 || fn == 6'h26;
// With two passes the instruction is in E for the first one only: the second
// pass's operands are taken from the registers with the first's and kept
// (s_hi, t_hi), and its SEL runs the cycle after (b1) with what is in E then
// -- the next instruction, which waits only if it needs the lanes' datapath
// (another computation) or the register being written.
logic               b1;                       // (NP = 2) the second pass's SEL, this cycle
logic [15:0]        s_hi [LANES], t_hi [LANES];
logic [5:0]         x_fn;
logic [2:0]         x_am;                     // (amode of it)
wire  [5:0]         fn_m  = (NP == 2 && b1) ? x_fn : fn;
wire  s_sgn = fn_m[2:0] == 3'd0 || fn_m[2:0] == 3'd1 || fn_m[2:0] == 3'd5 || fn_m[2:0] == 3'd7;
wire  t_sgn = fn_m[2:0] == 3'd0 || fn_m[2:0] == 3'd1 || fn_m[2:0] == 3'd6 || fn_m[2:0] == 3'd7;
logic [15:0]        s_n [LANES], t_n [LANES];
logic signed [33:0] p_n [LANES];
always_comb
    for (int l = 0; l < LANES; l++) begin
        s_n[l] = (NP == 2 && b1) ? s_hi[l] : qa[l];
        t_n[l] = (NP == 2 && b1) ? t_hi[l] : tv[l];
        p_n[l] = $signed({s_sgn & s_n[l][15], s_n[l]}) * $signed({t_sgn & t_n[l][15], t_n[l]});
    end
// (MULW) the upper lanes' products, for a multiply
logic signed [33:0] pm_n [LANES], pm_r [LANES];
always_comb
    for (int l = 0; l < LANES; l++) begin
        logic [15:0] sm, tm;
        sm = qa[((LANES + l) & 7)]; tm = tv[((LANES + l) & 7)];
        pm_n[l] = 34'sd0;
        if (MW) pm_n[l] = $signed({s_sgn & sm[15], sm}) * $signed({t_sgn & tm[15], tm});
    end

// EX's registers: the pass, its operands and products, the instruction's fields
logic               x_v, x_pass, x_last, x_wait, x_one, x_rcp;
logic               x_mw;                     // (MULW) a multiply: all eight lanes in this one pass
logic [3:0]         fl_r [LANES];             // the adder's flags of the cycle EX waited (VCL, VCH, VCR with one pass)
logic [3:0]         x_ve;
logic [4:0]         x_vdn;
logic [2:0]         x_de;
logic [15:0]        s_r [LANES], t_r [LANES];
logic signed [33:0] p_r [LANES];
logic [15:0]        hold [8];                 // the earlier pass's results
lane_t ex [LANES];
always_comb
    for (int l = 0; l < LANES; l++) begin
        logic [2:0] li, lm;
        logic [15:0] vs;
        li = 3'(int'(x_pass) * LANES + l);
        // (the accumulator a multiply adds to: with MULW it has one pass, x_pass 0 -- its lanes are these lanes' own, with no
        // choice between the passes' in front of the 48-bit adders; VSAR reads the pass's)
        lm = MW ? 3'(l) : li;
        // (VSAR's word: one of the two passes' lanes' three)
        unique case ({NP == 2 && x_pass, x_ve[1:0]})
        3'd0:    vs = acc_h[l];
        3'd1:    vs = acc_m[l];
        3'd2:    vs = acc_l[l];
        3'd4:    vs = acc_h[(LANES + l) & 7];
        3'd5:    vs = acc_m[(LANES + l) & 7];
        3'd6:    vs = acc_l[(LANES + l) & 7];
        default: vs = 'x;
        endcase
        ex[l] = lane_ex(x_fn, x_am, x_ve, s_r[l], t_r[l], p_r[l], vs, {acc_h[lm], acc_m[lm], acc_l[lm]},
                        vco[li], vco[4'd8 + li], vcc[li], vcc[4'd8 + li], vce[li], fl_r[l]);
    end
lane_t exm [LANES];
always_comb
    for (int l = 0; l < LANES; l++)
        exm[l] = MW ? lane_mul(x_fn, pm_r[l], {acc_h[((LANES + l) & 7)], acc_m[((LANES + l) & 7)], acc_l[((LANES + l) & 7)]}) : '0;
wire x_exec = x_v && !x_wait;                 // EX runs this cycle
wire x_fin  = x_exec && x_last;               // ... its last pass
// (two passes) the last pass under a load's write: the registers' one write
// port is the load's, and vd is written a cycle on, whole, from `hold` (wp)
logic        wp;
logic [4:0]  w_vdn;
wire         x_defer, x_wr;                   // (below, with the load's write) ... and EX writes vd

// ---- the reciprocal unit: the input (a cycle), normalised to the ROM's
// address (a cycle), the ROM's word shifted back (the EX of the last pass)
logic [31:0] rin;
logic        r_sqrt, r_zero, r_min, r_mask;
logic [4:0]  r_sh;
wire  [31:0] r_data0 = rin ^ {32{rin[31]}};
wire  [31:0] r_data  = ($signed(rin) > -32'sd32768) ? r_data0 + {31'd0, rin[31]} : r_data0;
logic [5:0]  r_clz;
always_comb begin
    r_clz = 6'd32;
    for (int k = 0; k < 32; k++) if (r_data[k]) r_clz = 6'(31 - k);
end
wire  [31:0] r_norm = r_data << r_clz[4:0];
wire  [8:0]  r_idx  = r_norm[30:22];
assign rom_addr = r_sqrt ? {1'b1, r_idx[8:1], r_clz[0]} : {1'b0, r_idx};
wire  [31:0] r_val  = {1'b0, 1'b1, rom_q, 14'd0};                          // (0x10000 | rom) << 14
wire  [4:0]  r_d    = 5'd31 - r_sh;
wire  [4:0]  r_rsh  = r_sqrt ? {1'b0, r_d[4:1]} : r_d;
wire  [31:0] r_res  = r_zero ? 32'h7FFF_FFFF : r_min ? 32'hFFFF_0000 : (r_val >> r_rsh) ^ {32{r_mask}};

// ------------------------------------------------------- loads and stores --
// Every load and store is one DMEM access, in the instruction's first cycle:
// what it moves lies in the 16 bytes from its address's word on (LQV, SQV
// up to the 16-byte block's end; LRV, SRV the block from its start; a packed
// load the 16 bytes from the 8-byte block, where its bytes wrap). DMEM's
// bytes and a register's are a rotation of each other: a load's register
// byte m is DMEM's byte at address mod 16 = m + (addr - e), a store's DMEM
// byte b (address mod 16) the register's byte b + (e - addr). The bytes are
// chosen by enables: the register's bytes a load writes, DMEM's a store does.
//   LTV, STV        a byte a cycle (every graphics microcode has them, none
//                   runs them often)
typedef enum logic { V_IDLE, V_TV } vst_t;
vst_t st;
wire  [11:0] off  = {{5{li[6]}}, li[6:0]};
// (the offset scaled first, so that the address is one adder after rs: the
// store's rotation hangs off its low bits)
logic [11:0] off_sh;
always_comb begin
    unique case (lsop)
    5'd0:             off_sh = off;
    5'd1:             off_sh = {off[10:0], 1'b0};
    5'd2:             off_sh = {off[9:0], 2'b0};
    5'd3, 5'd6, 5'd7: off_sh = {off[8:0], 3'b0};
    default:          off_sh = {off[7:0], 4'b0};
    endcase
end
wire  [11:0] ls_addr = la[11:0] + off_sh;
wire  ls_packed = lsop == 5'd6 || lsop == 5'd7;
wire  ls_tv     = (lwc2 || swc2) && lsop == 5'd11;                 // LTV, STV
// SHV, SFV: a byte every second / fourth of the 16 from the 8-byte block
wire  st_hv     = swc2 && lsop == 5'd8;
wire  st_fv     = swc2 && lsop == 5'd9 && el[1:0] == 2'd0;
wire  ls_known  = lsop <= 5'd7 || lsop == 5'd11 || st_hv || st_fv;
wire  [3:0]  o    = ls_addr[3:0];
wire  [4:0]  size = 5'd1 << lsop[1:0];
// the access's first word: LRV / SRV the block's, a packed load (and SHV, SFV)
// the 8-byte block's
wire  [1:0]  win_w  = lsop == 5'd5 ? 2'b00 : ((lwc2 && ls_packed) || st_hv || st_fv) ? {ls_addr[3], 1'b0} : ls_addr[3:2];
wire  [11:0] ls_win = {ls_addr[11:4], win_w, 2'b00};
// a load's register bytes: a run of the register's sixteen from e on -- its size's; to the 16-byte block's end
// (LQV); or (LRV) from where the block's start comes, the bytes before the address, to the register's end
logic [15:0] ld_mask;
always_comb begin
    logic [4:0] n;
    logic [5:0] lo, hi;
    n = 5'd16 - {1'b0, o};                                                        // the bytes from the address to its block's end
    lo = {2'b00, el};
    unique case (lsop)
    5'd4:    hi = {2'b00, el} + {1'b0, n};                                        // LQV
    5'd5:    begin lo = {2'b00, el} + {1'b0, n}; hi = 6'd16; end                  // LRV
    default: hi = {2'b00, el} + {1'b0, size};                                     // LBV .. LDV
    endcase
    for (int m = 0; m < 16; m++) ld_mask[m] = 6'(m) >= lo && 6'(m) < hi;
end
// a store's DMEM bytes (q: the byte's place from the address on; by lsop's low four bits -- the others are no store)
logic [15:0] st_be;
always_comb
    for (int b = 0; b < 16; b++) begin
        logic [3:0] q;
        q = 4'(b) - o;
        unique casez (lsop[3:0])
        4'b0100:       st_be[b] = 4'(b) >= o;                                     // SQV
        4'b0101:       st_be[b] = 4'(b) < o;                                      // SRV
        4'b011?:       st_be[b] = !q[3];                                          // SPV, SUV: 8 bytes
        4'b1???:       st_be[b] = !q[0] && !(lsop[0] && q[1]);                    // SHV: every second, SFV: every fourth
        default:       st_be[b] = {1'b0, q} < size;                               // SBV .. SDV
        endcase
    end

// LTV and STV, the transposed transfers: 16 bytes between a 16-byte window of
// DMEM (from the 8-byte block of the address, wrapping in it) and the eight
// registers of vt's group, a lane of each along a diagonal, a byte a cycle.
//   LTV  byte n of the window from (e + (addr & 8))  ->  byte n of register (e / 2 + n / 2) & 7
//   STV  byte (n - (e & ~1)) & 15 of register n / 2  ->  byte n of the window from (addr & 7) - (e & ~1)
logic [3:0]  tv_n, tv_off;
logic [11:0] tv_base;
wire  [3:0]  tv_idx  = tv_n - {el[3:1], 1'b0};                        // STV: the register's byte
wire  [4:0]  tv_reg  = {ltn[4:3], swc2 ? tv_n[3:1] : el[3:1] + tv_n[3:1]};
wire  [11:0] tv_addr = tv_base + {8'd0, tv_off + tv_n};
wire         in_tv   = st == V_TV;

// a store's (and MFC2's) bytes: the register's 16 (sarr: its bytes, or for
// SPV/SUV each lane's top 8 bits or bits 14..7 -- lane i & 7, the top 8 for
// the first eight and SPV, the other way round for SUV; for SHV bits 14..7 of
// the 16 from byte i, {byte i [6:0], byte i + 1 [7]}; for SFV a lane's bits
// 14..7, byte i (a multiple of 4) taking lane {e[3], i[3] ^ e[3], i[2]}:
// lanes 0-3 for e 0, 1,2,3,0 for 4, 4-7 for 8, 5,6,7,4 for 12) rotated to DMEM's:
// byte b takes sarr[(b + st_rot) & 15] -- by one to three bytes, then by
// words. A store's st_rot is e - addr = (e - the offset) - rs: the part known
// from the instruction first, then rs's low bits (the late ones: forwarded),
// the low two of them first
logic [7:0]  sarr [16];
logic [7:0]  sr1 [16];
logic [7:0]  sb [16];
wire         st_pk  = swc2 && ls_packed;
wire  [3:0]  st_eo  = el - off_sh[3:0];
wire  [3:0]  st_rot = in_tv ? tv_idx - tv_addr[3:0] : swc2 ? st_eo - a[3:0] : el;
// (byte i one of four, by a select the bytes share -- st_sel[0] for the first eight, [1] for the others: 0 its byte, 1 SHV's,
// 2 the packed top 8 bits, 3 the packed bits 14..7; SFV's bytes are only the four i a multiple of 4, the others no matter)
logic [1:0]  st_sel [2];
always_comb
    for (int h = 0; h < 2; h++) st_sel[h] = st_pk ? {1'b1, lsop[0] ^ 1'(h)} : {1'b0, st_hv};
always_comb begin
    for (int i = 0; i < 16; i++) begin
        logic [15:0] wp, wn, wh, wf;
        wp = qc[i % 8];
        wn = qc[i / 2];
        wh = (i % 2 == 1) ? {wn[7:0], qc[((i + 1) / 2) % 8][15:8]} : wn;
        wf = qc[{el[3], 1'(i / 8) ^ el[3], 1'((i / 4) % 2)}];
        unique case (st_sel[i / 8])
        2'd0: sarr[i] = (i % 2 == 1) ? wn[7:0] : wn[15:8];
        2'd1: sarr[i] = wh[14:7];
        2'd2: sarr[i] = wp[15:8];
        2'd3: sarr[i] = wp[14:7];
        endcase
        if (i % 4 == 0 && st_fv) sarr[i] = wf[14:7];
    end
    for (int b = 0; b < 16; b++) sr1[b] = sarr[(b + int'(st_rot[1:0])) & 15];
    for (int b = 0; b < 16; b++) sb[b]  = sr1[(b + 4 * int'(st_rot[3:2])) & 15];
end

// the bytes a load (or MTC2) brings, and where they go. A load's data comes
// the cycle after its address -- by then the instruction may have left E, so
// what the write needs is kept (lw_*)
logic        lw_v, lw_last, lw_packed, lw_p6;
// the write leaves half of a lane as it was (an odd byte at an end): a store
// or MFC2 does not start under it (l_free)
logic        lw_merge;
logic [4:0]  lw_reg;
logic [15:0] lw_be;                   // the register's bytes written
// (or under another's late write: a multiply of one pass, MULW, right behind it)
// (one pass: only a pair's computation, whose load writes in its EX's cycle)
assign x_defer = (NP == 2 || LPAIR) && x_fin && (lw_v || wp) && !x_one;
assign x_wr    = x_fin && !x_defer;
wire         idle    = st == V_IDLE && !x_v && !lw_v && !wp;  // nothing under way: the registers and flags are final
wire         mt_now  = valid && mtc2 && idle;
// MTC2: rt's two bytes at e and e + 1 (one at the register's end)
wire  [15:0] mt_be   = (16'd1 << el) | (el == 4'd15 ? 16'd0 : 16'd2 << el);
wire  [15:0] in_be   = lw_v ? lw_be : mt_now ? mt_be : 16'd0;
wire         in_v    = lw_v || mt_now;
wire         in_pk   = lw_v && lw_packed;
wire  [4:0]  rb_live = rb;            // the register the instruction in E reads (MFC2 / MTC2: rd; a store: vt)
logic [15:0] ld_d [8];
logic [1:0]  c_now;                   // (simulation: see commit)
logic [2:0]  c_def, c_defw;
always_comb
    for (int g = 0; g < 8; g++) begin
        logic [7:0] dh, dl, dp;
        dh = lw_v ? m_rdata[127 - 16 * g -: 8] : el[0] ? b[7:0] : b[15:8];       // register byte 2g
        dl = lw_v ? m_rdata[119 - 16 * g -: 8] : el[0] ? b[15:8] : b[7:0];       // ... 2g + 1
        dp = m_rdata[127 - 8 * g -: 8];                                           // a packed load's lane g
        if (in_pk) ld_d[g] = lw_p6 ? {dp, 8'd0} : {1'b0, dp, 7'd0};
        else       ld_d[g] = {dh, dl};
    end

// ----------------------------------------------------------- the controls --
// a computational instruction's SEL: not under an EX that writes what it reads,
// nor (a reciprocal) under any, nor under a load's write of what it reads
wire cmp      = valid && vec && fn_known && !fn_none;
wire dep      = (x_v && x_last && (x_vdn == vsn || x_vdn == vtn)) || (wp && (w_vdn == vsn || w_vdn == vtn));
// (a load's write of another register is no matter: the operands come from
// the copies the instruction addresses itself, and this one's own write is a
// cycle on. Microcode loads a register and computes with others at once.)
wire ldep     = lw_v && (lw_reg == vsn || lw_reg == vtn);
// (one pass: a one-lane write, which is not kept, does not reach EX under a computation's late write -- x_defer now is wp then)
wire sel0r    = cmp && st == V_IDLE && !ldep && !x_wait && !b1 && !(dep || (fn_rcp && x_v)) && !(NP == 1 && fn_one && x_defer);
wire none_ok  = valid && vec && (fn_none || !fn_known) && !x_v && !lw_v;      // (one that does nothing: done at once)
wire sel1     = NP == 2 && b1;                // (nothing holds it: the first pass's EX never waits)
wire sel_fire = sel0 || sel1;
// the others: a load's first cycle reads no register, so it starts under
// anything (but an EX that still waits: its write and the load's would meet);
// a store or MFC2 under an EX that writes another register than the one it
// reads (not one that waits: it would be done before an instruction ahead of
// it, and the lockstep bench takes them in order), and under a load's write
// of another that is of whole lanes; MTC2, CFC2 and CTC2 when nothing is
// under way
wire x_free   = (!x_v || (!x_wait && x_vdn != rb_live)) && !(wp && w_vdn == rb_live);
wire l_free   = !lw_v || (!lw_merge && lw_reg != rb_live);
// (a load under the second pass's SEL: its write would meet that pass's, a
// cycle on -- a load into another register goes ahead, and vd is written the
// cycle after it; LTV waits for the SEL. And none starts under that late
// write's EX: the next cycle's port is taken.)
wire ld_ok    = !x_wait && !x_defer && (!b1 || (ls_known && !ls_tv && ltn != x_vdn));
wire ls_go    = (lwc2 || swc2) && ls_known && !ls_tv;
wire start_r  = (lp || (valid && !vec)) && st == V_IDLE
              && (ls_tv ? idle : lwc2 ? ld_ok : (swc2 || mfc2) ? (l_free && x_free) : idle)
              && !(m_stall && ls_go);                       // (a load or store waits for the port)
// a pair goes as one: its computation's SEL and its load's start in the same cycle, or neither
wire sel0     = sel0r && (!lp || start_r);
wire start_nc = start_r && (!lp || sel0r || none_ok);
// a load's read goes out this cycle (its write is the next)
wire lw_nx    = lwc2 && m_valid;
// C's address next cycle: the register STV is at then (two beats each), else the one E's instruction reads
wire         stv_n   = swc2 && ((st == V_IDLE && start_nc && ls_tv) || (in_tv && !(tv_n == 4'd15 && !m_stall)));
wire  [4:0]  tv_nn   = {1'b0, tv_n} + 5'd1;
wire  [2:0]  tvr_n   = !in_tv ? 3'd0 : m_stall ? tv_n[3:1] : tv_nn[3:1];
assign rc_n = stv_n ? {ltn[4:3], tvr_n} : rb_n;

always_comb begin
    // DMEM
    m_valid = (start_nc && ls_go) || (in_tv && !m_stall);
    m_we    = swc2;
    m_addr  = in_tv ? tv_addr : ls_win;
    m_be    = in_tv ? 16'd1 << tv_addr[3:0] : st_be;
    m_rot   = in_tv ? tv_addr[3:0] - tv_n : o - el;
    for (int i = 0; i < 16; i++) m_wdata[127 - 8 * i -: 8] = sb[i];
    // done, and the value MFC2 / CFC2 give
    done = 1'b0;
    if (sel0) done = 1'b1;                                      // the rest runs behind the next instruction
    if (none_ok && (!lp || start_r)) done = 1'b1;
    if (start_nc) begin
        if (mfc2 || cfc2 || mtc2 || ctc2) done = 1'b1;
        if ((lwc2 || swc2) && !ls_known) done = 1'b1;
        if (ls_go) done = 1'b1;                                 // (a load's write comes behind)
        if (lp && !(sel0r || none_ok)) done = 1'b0;             // (a pair: not without its computation)
    end
    if (in_tv && tv_n == 4'd15 && !m_stall) done = 1'b1;
    res = {{16{sb[0][7]}}, sb[0], sb[1]};
    if (cfc2) res = li[12:11] == 2'd0 ? {{16{vco[15]}}, vco} : ins[12:11] == 2'd1 ? {{16{vcc[15]}}, vcc} : {24'd0, vce};
    // the register write: EX's last pass, a load's bytes, MTC2
    wr_reg = x_vdn; wr_be = 16'd0;
    for (int g = 0; g < 8; g++) wr_d[g] = hold[g];
    if (!wp) for (int l = 0; l < LANES; l++) wr_d[(NP - 1) * LANES + l] = ex[l].res;
    if (!wp && MW && x_mw) for (int l = 0; l < LANES; l++) begin wr_d[l] = ex[l].res; wr_d[LANES + l] = exm[l].res; end
    if (wp) begin wr_reg = w_vdn; wr_be = 16'hFFFF; end             // (the late write)
    if (x_wr) begin
        wr_be = 16'hFFFF;
        if (x_one) begin
            wr_be = 16'd3 << {x_de, 1'b0};
            if (x_rcp) for (int g = 0; g < 8; g++) wr_d[g] = x_fn[1:0] == 2'd2 ? div_out[31:16] : r_res[15:0];
        end
    end else if (in_v) begin
        wr_reg = lw_v ? lw_reg : vsn;
        wr_be = in_pk ? 16'hFFFF : in_be;
        for (int g = 0; g < 8; g++) wr_d[g] = ld_d[g];
    end
    // (simulation) instructions whose effects are complete with this cycle,
    // in order: what finishes under a computation not yet written (a load, a
    // store, MFC2 behind its second pass) is counted with it
    c_now  = 2'(lw_v && lw_last)
           + 2'((none_ok && (!lp || start_r))
                || (start_nc && !lp && (mfc2 || cfc2 || mtc2 || ctc2 || ((lwc2 || swc2) && !ls_known) || (swc2 && ls_go)))
                || (in_tv && swc2 && tv_n == 4'd15 && !m_stall));
    // (the computation whose write is late: it and what was kept behind it;
    // then the one in EX, if it is written now, with what is behind it)
    commit = (wp ? 4'd1 + 4'(c_defw) : 4'd0) + (x_wr ? 4'd1 + 4'(c_def) + 4'(c_now) : !x_v ? 4'(c_now) : 4'd0);
    commit_part = wp && x_exec;
end

// The products' registers in a block of their own, with nothing but an
// enable between them and the multipliers (taken at SEL, out of reset, as in
// the block below). In that block synthesis gave each bit a flip-flop behind a
// hold select, which the fitter cannot pack: here the fitter makes them the
// DSP blocks' output registers (272 flip-flops and their selects out of the
// logic with 8 lanes)
always_ff @(posedge clk)
    if (!rst && sel_fire)
        for (int l = 0; l < LANES; l++) begin p_r[l] <= p_n[l]; pm_r[l] <= pm_n[l]; end

always_ff @(posedge clk) begin
    if (rst) begin
        st <= V_IDLE; lw_v <= 1'b0; unimpl <= 1'b0; x_v <= 1'b0; x_wait <= 1'b0; b1 <= 1'b0; c_def <= 3'd0;
        wp <= 1'b0; c_defw <= 3'd0;
        vco <= 16'd0; vcc <= 16'd0; vce <= 8'd0; dph <= 1'b0; div_out <= 32'd0; div_in_h <= 16'd0;
        for (int g = 0; g < 8; g++) begin acc_h[g] <= 16'd0; acc_m[g] <= 16'd0; acc_l[g] <= 16'd0; end
    end else begin
        lw_v <= 1'b0;
        // the reciprocal: normalised the cycle after its input is taken
        r_sh <= r_clz[4:0]; r_mask <= rin[31];
        r_zero <= rin == 32'd0; r_min <= rin == 32'hFFFF_8000;
        if (valid && vec && !fn_known) unimpl <= 1'b1;
        if (start_nc && (lwc2 || swc2) && !ls_known) unimpl <= 1'b1;

        // ---- EX
        if (x_v && x_wait) x_wait <= 1'b0;
        for (int l = 0; l < LANES; l++) fl_r[l] <= ex[l].fl;
        if (x_exec) begin
            for (int l = 0; l < LANES; l++) begin
                automatic int li = int'(x_pass) * LANES + l;
                automatic int lm = MW ? l : li;
                // (a multiply's write: with MULW into these lanes' own, as the sum is of them -- lm)
                if (ex[l].acc_w) begin acc_h[lm] <= ex[l].acc[47:32]; acc_m[lm] <= ex[l].acc[31:16]; acc_l[lm] <= ex[l].acc[15:0]; end
                else if (ex[l].accl_w) acc_l[li] <= ex[l].acc[15:0];
                if (ex[l].co_w) begin vco[li] <= ex[l].co_lo; vco[8 + li] <= ex[l].co_hi; end
                if (ex[l].cc_w) begin vcc[li] <= ex[l].cc_lo; vcc[8 + li] <= ex[l].cc_hi; end
                if (ex[l].ce_w) vce[li] <= ex[l].ce;
                hold[li] <= ex[l].res;
            end
            if (MW && x_mw)
                for (int l = 0; l < LANES; l++) begin
                    automatic int li = LANES + l;
                    acc_h[li] <= exm[l].acc[47:32]; acc_m[li] <= exm[l].acc[31:16]; acc_l[li] <= exm[l].acc[15:0];
                    hold[li] <= exm[l].res;
                end
            if (x_last && x_rcp) begin
                if (x_fn[1:0] == 2'd2) begin div_in_h <= rin[15:0]; dph <= 1'b1; end
                else begin div_out <= r_res; dph <= 1'b0; end
            end
            x_v <= 1'b0;
        end

        // ---- SEL
        c_def <= (x_v && !x_fin) ? c_def + 3'(c_now) : 3'd0;
        if (x_defer) c_defw <= c_def + 3'(c_now);
        wp <= x_defer;
        if (x_defer) w_vdn <= x_vdn;
        b1 <= NP == 2 && sel0 && !(MW && fn_mul);
        if (sel_fire) begin
            for (int l = 0; l < LANES; l++) begin s_r[l] <= s_n[l]; t_r[l] <= t_n[l]; end
            x_v <= 1'b1; x_pass <= sel1; x_last <= NP == 1 || sel1 || (MW && fn_mul);
            x_mw <= MW && sel0 && fn_mul;
            // one pass: a reciprocal waits a cycle for the ROM; VCL, VCH and VCR
            // take their adder's flags in one cycle and select by them in the
            // next (eight lanes' select into the registers does not fit one).
            // And the pass that writes the register waits under a load's
            // write: that is the cycle its own would be.
            x_wait <= NP == 1 ? ((fn_rcp && fn[1:0] != 2'd2) || fn_ch || (lp && fn_one))   // (a pair's one-lane write: not kept, it waits)
                              : (sel1 && lw_nx && x_one);       // (a write of one lane is not kept: it waits)
            if (sel0) begin
                x_fn <= fn; x_am <= amode(fn); x_ve <= ve; x_vdn <= vdn; x_de <= de; x_one <= fn_one; x_rcp <= fn_rcp;
                for (int l = 0; l < LANES; l++) begin         // (two passes) the second's operands
                    s_hi[l] <= qa[((LANES + l) & 7)]; t_hi[l] <= tv[((LANES + l) & 7)];
                end
                // the reciprocal's input: 16 bits, or with the high half a VRCPH / VRSQH left
                rin <= (fn[1:0] == 2'd1 && dph) ? {div_in_h, qc[ve[2:0]]} : {{16{qc[ve[2:0]][15]}}, qc[ve[2:0]]};
                r_sqrt <= fn[2];
            end
        end

        // ---- the others
        unique case (st)
        V_IDLE: if (start_nc) begin
            if (ctc2) begin
                if (li[12:11] == 2'd0) vco <= b[15:0];
                else if (li[12:11] == 2'd1) vcc <= b[15:0];
                else vce <= b[7:0];
            end else if (ls_tv && ls_known) begin
                tv_n <= 4'd0; tv_base <= {ls_addr[11:3], 3'd0};
                tv_off <= swc2 ? {1'b0, ls_addr[2:0]} - {el[3:1], 1'b0} : el + {ls_addr[3], 3'd0};
                st <= V_TV;
            end
        end
        V_TV: if (!m_stall) begin
            tv_n <= tv_n + 4'd1;
            if (tv_n == 4'd15) st <= V_IDLE;
        end
        default: st <= V_IDLE;
        endcase
        // a load's bytes: on m_rdata the next cycle
        if (lwc2 && m_valid) begin
            lw_v <= 1'b1; lw_reg <= ltn; lw_be <= ld_mask;
            lw_packed <= ls_packed; lw_p6 <= lsop == 5'd6; lw_last <= 1'b1;
            lw_merge <= !ls_packed && |((ld_mask ^ (ld_mask >> 1)) & 16'h5555);   // (a packed load writes whole lanes)
            if (in_tv) begin
                lw_be <= 16'd1 << tv_n; lw_reg <= tv_reg; lw_packed <= 1'b0; lw_last <= tv_n == 4'd15;
                lw_merge <= 1'b1;
            end
        end
    end
end

endmodule
`default_nettype wire
