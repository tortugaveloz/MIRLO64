// rsp_vu_oc: the RSP's vector unit (COP2) and its loads and stores (LWC2, SWC2).
//
// 32 registers of 8 lanes of 16 bits, a 48-bit accumulator per lane, the
// flags VCO, VCC and VCE, the reciprocal unit. The contract is the reference
// model, sim/rsp/rsp.c: the same state after each instruction.
//
// Eight lanes (LANES stays a parameter for the ports). An instruction arrives from the scalar unit's E stage (valid, held
// until done; the instructions in E are taken, with their decode, from ins_n / lins_n when E takes them: adv):
//   computational   SEL (the operands), then by kind -- a multiply: the products (MUL), the accumulator (EX1), the clamp and
//                   vd (EX2); one with no adder (VMRG, VMOV, the logic operations, VSAR, VRCPH): EX2; the others: the
//                   adder (EX1, two cycles for a clip and a reciprocal) and the result, the accumulator's low word, the
//                   flags and vd (EX2). All eight result lanes are written at the end, so vd may be vs or vt
//   VRCP.. VRSQH    with them, the reciprocal ROM
//   MFC2 MTC2 CFC2 CTC2   one cycle (MFC2's and CFC2's value comes the cycle after)
//   loads, stores   one cycle (DMEM is 16 bytes wide, as the N64's: what one
//                   moves lies in the 16 bytes from its address's word on);
//                   a load's data comes the cycle after its address
//   a pair          a computation on valid / ins and the
//                   scalar unit's load (LBV .. LUV), store, MFC2, MTC2, CFC2 or
//                   CTC2 on l_valid / l_ins (a load's or store's base on a), the
//                   scalar unit's pair: both start in the same cycle (done for
//                   the two); a load's (and MTC2's) write first and the
//                   computation's a cycle late (wp), as behind any load
// The registers are LUT RAM (three copies, read in the cycle, written a byte
// at a time); a load, and MTC2, writes its bytes over the register's the cycle
// after it starts.
//
//   LTV, STV        a byte a cycle (every graphics microcode has them, none
//                   runs them often)
//   SHV, SFV        one cycle (a JPEG microcode's YUV packing)
// Not implemented (no microcode of the ROMs scanned has them -- sim/rsp/
// rom_ucode_scan.py; they do nothing and set `unimpl`): LHV LFV LWV, SWV, SFV
// at an element not a multiple of 4, VRNDP VRNDN VMULQ VMACQ. The reserved
// functions give the console's result: vd 0, the accumulator's low word vs + vt.
`default_nettype none

module rsp_vu_oc #(
    parameter int LANES = 8,
    parameter bit MULW = 0                  // (not used: kept for the ports of the 4-lane unit that was)
) (
    input  wire         clk,
    input  wire         rst,

    input  wire         valid,
    // the instructions entering E (the unit keeps them, and what is decoded of them, from the cycle E takes them: adv):
    // the computation's and the scalar unit's own, a pair's load, store or move
    input  wire  [31:0] ins_n,
    input  wire  [31:0] lins_n,
    input  wire         adv,
    input  wire  [31:0] a,              // rs: the base of a load / store
    input  wire         l_valid,        // (LPAIR) a load, store or move paired with the computation on ins
    input  wire  [31:0] b,              // rt: MTC2, CTC2
    // the registers' read addresses, from the scalar unit: of the slot E holds next cycle -- the computation's vs and
    // vt (ra_n, rb_n), and the register the scalar unit's own reads or writes, a store's or a load's vt or MFC2 / MTC2's
    // rd (rd_n) -- and that last of the slot in E now (rd)
    input  wire  [4:0]  ra_n,
    input  wire  [4:0]  rb_n,
    input  wire  [4:0]  rd_n,
    input  wire  [4:0]  rd,
    output logic        done,
    output logic [31:0] res,            // MFC2
    output logic [31:0] res_l,          // CFC2's, the cycle after it leaves E (the flags of every computation before it)

    // DMEM (rsp_top_oc): 16 bytes a cycle, those from m_addr's word on. A store
    // writes the bytes of m_be with those of m_wdata, both by the byte's
    // address mod 16 (bit b, m_wdata[127 - 8b -: 8]); a load's data comes back
    // the next cycle rotated by m_rot (byte m of m_rdata is the one at address
    // mod 16 = m + m_rot), so byte m is the register's byte m
    output logic        m_valid,
    output logic        m_we,
    output logic [11:0] m_addr,
    output logic [11:0] m_addr16,       // ... and the address + 16 (the next line's: made in parallel)
    output logic [15:0] m_be,
    output logic [127:0] m_wpre,        // a store's bytes in the register's order (byte i in [127 - 8i -: 8]), and the rotation to DMEM's:
    output logic [3:0]  m_wsh,          // byte b of DMEM's line takes byte (b + m_wsh) mod 16 of m_wpre
    output logic [3:0]  m_rot,
    input  wire  [127:0] m_rdata,
    input  wire         m_stall_ld,     // DMEM's port is not for a load this cycle (the DMA's, or a store's write): a load (each beat of LTV) waits
    input  wire         m_stall_st,     // ... for a store (the DMA's): a store (each beat of STV) waits

    // the reciprocal ROM (rsp_top_oc): 512 reciprocals, then 512 inverse square roots
    output logic [9:0]  rom_addr,
    input  wire  [15:0] rom_q,

    output logic        unimpl,
    output logic [3:0]  commit,         // (simulation) instructions whose effects are all in with this cycle
    output logic        commit_part     // ... but the accumulator and flags already have a later one's first pass
);

// (eight lanes only: LANES stays a parameter for the ports)
localparam bit LPAIR = 1'b1;            // a computation and a load may come together (rsp_su_oc's pairs)

// ---------------------------------------------------------------- decode --
// The instructions in E are registers here, and what the unit asks of them -- which kind of instruction, which of the
// functions -- is registered with them, decoded the cycle before from the instructions arriving in E (the decode is not
// in front of the controls)
logic [31:0] ins, l_ins;
function automatic logic is_resv(input logic [5:0] f);          // the reserved functions: vd 0, the accumulator's low word vs + vt (the adder's, as VADDC's), the flags kept
    return f == 6'h12 || (f >= 6'h16 && f <= 6'h1C) || f == 6'h1E || f == 6'h1F || f == 6'h2E || f == 6'h2F || (f >= 6'h38 && f <= 6'h3E);
endfunction
// a computation's: vec -- COP2 with bit 25 -- and of its function
typedef struct packed {
    logic vec, mul, rcp, one, none, known, fast, cmp, cin, ch;
    logic [2:0] am;
} cdec_t;
function automatic cdec_t cdecode(input logic [31:0] i);
    cdec_t d;
    logic [5:0] f;
    logic mov;
    f = i[5:0];
    d.vec  = i[31:26] == 6'h12 && i[25];
    d.mul  = f[5:4] == 2'b00 && f[2:1] != 2'b01;                   // the multiplies (not VRNDP/VMULQ/VRNDN/VMACQ)
    d.rcp  = f[5:3] == 3'b110 && f[1:0] != 2'b11;                  // VRCP VRCPL VRCPH VRSQ VRSQL VRSQH
    mov    = f == 6'h33;
    d.one  = d.rcp || mov;                                         // writes one lane of vd
    d.none = f == 6'h37 || f == 6'h3F;                             // VNOP, VNULL
    d.known = d.mul || d.one || d.none || is_resv(f) || f == 6'h10 || f == 6'h11 || f == 6'h13 || f == 6'h14 || f == 6'h15
              || f == 6'h1D || (f >= 6'h20 && f <= 6'h2D);
    d.ch   = f == 6'h24 || f == 6'h25 || f == 6'h26;
    // the kinds: fast ones (no adder), compares and clips (s and t used in EX2), the rest (adds, reciprocals)
    d.fast = f == 6'h27 || mov || (f >= 6'h28 && f <= 6'h2D) || f == 6'h1D || (d.rcp && f[1:0] == 2'd2);
    d.cmp  = f == 6'h13 || (f >= 6'h20 && f <= 6'h23) || d.ch || d.rcp;     // (a reciprocal's accumulator word is vt, too)
    d.cin  = f == 6'h10 || f == 6'h11 || (f >= 6'h20 && f <= 6'h26 && f != 6'h21 && f != 6'h22);   // reads the flags in EX1: VADD and VSUB for their carry, the compares and clips for theirs
    unique case (f)
    6'h10:   d.am = 3'd1;
    6'h11:   d.am = 3'd2;
    6'h14:   d.am = 3'd3;
    6'h12, 6'h16, 6'h17, 6'h18, 6'h19, 6'h1A, 6'h1B, 6'h1C, 6'h1E, 6'h1F, 6'h2E, 6'h2F,
    6'h38, 6'h39, 6'h3A, 6'h3B, 6'h3C, 6'h3D, 6'h3E: d.am = 3'd3;
    6'h25:   d.am = 3'd4;
    6'h24:   d.am = 3'd5;
    6'h26:   d.am = 3'd6;
    6'h13:   d.am = 3'd7;
    default: d.am = 3'd0;
    endcase
    return d;
endfunction
// the scalar unit's own (a load, store or move): which, and of a load or store which one
typedef struct packed {
    logic mfc2, cfc2, mtc2, ctc2, lwc2, swc2, packed_, tv, hv, fv, known, go;
} ldec_t;
function automatic ldec_t ldecode(input logic [31:0] i);
    ldec_t d;
    logic cop2_l;
    logic [4:0] op5;
    op5 = i[15:11];
    cop2_l = i[31:26] == 6'h12 && !i[25];
    d.mfc2 = cop2_l && i[24:21] == 4'd0;
    d.cfc2 = cop2_l && i[24:21] == 4'd2;
    d.mtc2 = cop2_l && i[24:21] == 4'd4;
    d.ctc2 = cop2_l && i[24:21] == 4'd6;
    d.lwc2 = i[31:26] == 6'h32;
    d.swc2 = i[31:26] == 6'h3A;
    d.packed_ = op5 == 5'd6 || op5 == 5'd7;
    d.tv   = (d.lwc2 || d.swc2) && op5 == 5'd11;                  // LTV, STV
    d.hv   = d.swc2 && op5 == 5'd8;                               // SHV, SFV: a byte every second / fourth of the 16
    d.fv   = d.swc2 && op5 == 5'd9 && i[8:7] == 2'd0;
    d.known = op5 <= 5'd7 || op5 == 5'd11 || d.hv || d.fv;
    d.go   = (d.lwc2 || d.swc2) && d.known && !d.tv;
    return d;
endfunction
cdec_t cd;
ldec_t ld;
always_ff @(posedge clk)
    if (adv) begin ins <= ins_n; cd <= cdecode(ins_n); l_ins <= lins_n; ld <= ldecode(lins_n); end

wire [5:0] op   = ins[31:26];
wire       vec  = cd.vec;
// (the moves, loads and stores are never a pair's computation: from l_ins, the scalar unit's E instruction, which ins
// only adds a select to)
wire       mfc2 = ld.mfc2;
wire       cfc2 = ld.cfc2;
wire       mtc2 = ld.mtc2;
wire       ctc2 = ld.ctc2;
// (the load or store: l_ins, alone or a pair's load)
wire       lp   = LPAIR && l_valid;
wire [31:0] li  = l_ins;
wire [31:0] la  = a;
wire       lwc2 = ld.lwc2;
wire       swc2 = ld.swc2;
wire [4:0] lsop = li[15:11];
wire [3:0] el   = li[10:7];             // LWC2/SWC2/MFC2/MTC2: the element (a byte of the register)
wire [4:0] ltn  = li[20:16];            // LWC2/SWC2: the register
wire [3:0] ve   = ins[24:21];           // computational: vt's element
wire [4:0] vtn  = ins[20:16];
wire [4:0] vsn  = ins[15:11];
wire [4:0] vdn  = ins[10:6];
wire [5:0] fn   = ins[5:0];
wire [2:0] de   = ins[13:11];           // VRCP.., VMOV: the lane of vd written

wire fn_mul  /* verilator public_flat_rd */ = cd.mul;
wire fn_rcp  = cd.rcp;
wire fn_one  = cd.one;
wire fn_none = cd.none;
wire fn_known = cd.known;

// ------------------------------------------------------------- registers --
// three copies (LUT RAM, read in the cycle, each at an address its memory registers a cycle ahead): A at vs and C at
// vt, for the operands of a computation; D at the register a store or MFC2 reads (or the one STV has reached), for
// their bytes -- a pair's computation and store each have theirs. A write is of whole lanes or, a load's and MTC2's,
// of bytes: by an enable a byte, so nothing is read to keep the rest of a lane
logic [4:0]  wr_reg;
logic        wr_en;                   // a write this cycle (wr_be non-zero), made from the controls
logic [15:0] wr_be;                   // the register's bytes written (bit m: byte m -- lane m / 2, its upper half first)
logic [15:0] wr_d [8];
wire  [4:0]  rdd_n;                   // (below: D's address next cycle)
logic [15:0] qa [8], qc [8], qd [8];
logic [127:0] wr_w, qa_w, qc_w, qd_w;
logic [15:0]  wr_e;
always_comb
    for (int g = 0; g < 8; g++) begin
        wr_w[16 * g +: 16] = wr_d[g]; wr_e[2 * g + 1] = wr_be[2 * g]; wr_e[2 * g] = wr_be[2 * g + 1];
        qa[g] = qa_w[16 * g +: 16]; qc[g] = qc_w[16 * g +: 16]; qd[g] = qd_w[16 * g +: 16];
    end
rsp_lutram_oc ma (.clk, .en(wr_en), .we(wr_e), .wa(wr_reg), .wd(wr_w), .ra_n(ra_n), .q(qa_w));
rsp_lutram_oc mc (.clk, .en(wr_en), .we(wr_e), .wa(wr_reg), .wd(wr_w), .ra_n(rb_n), .q(qc_w));
rsp_lutram_oc md (.clk, .en(wr_en), .we(wr_e), .wa(wr_reg), .wd(wr_w), .ra_n(rdd_n), .q(qd_w));
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
function automatic logic [15:0] mul_res(input logic [2:0] k, input logic [47:0] a, input logic gt, input logic lt);
    logic klo, kc, c15, clo;
    klo = k == 3'd4 || k == 3'd6;
    if (k == 3'd1) begin kc = a[47] || gt; c15 = !a[47]; clo = !a[47]; end
    else if (klo)  begin kc = gt || lt;    c15 = gt;     clo = gt;     end
    else           begin kc = gt || lt;    c15 = !gt;    clo = gt;     end
    return kc ? {c15, {15{clo}}} : klo ? a[15:0] : a[31:16];
endfunction

// (what the lanes' adder does, by the instruction -- kept with it, x_am: 1 VADD, 2 VSUB, 3 VADDC, 4 VCH, 5 VCL, 6 VCR, 0 the rest (s - t),
// and 7, 0 - t (VABS's, and the clips' second cycle: their compare is over by then, the flags it made are in fl_r, and -t is
// what they and VABS pick) -- is in cdecode)

// EX1, any computation but a multiply: one adder for every add, subtract and compare, s + t or s - t, with the 17-bit
// signed result's top bit (b16) and the carry (unsigned: no borrow). Its sum and flags are registered (lo_r, fl_r);
// EX2 does the rest
typedef struct packed {
    logic [15:0] lo;
    logic [3:0]  fl;                     // {carry, b16, zero, ones}
} add_t;
function automatic add_t lane_x1(input logic [2:0] am, input logic [15:0] s, input logic [15:0] t, input logic co_lo);
    add_t o;
    logic sign, sub, cin, cout, b16, cin_x;
    logic [15:0] bb, lo, sa;
    sign = s[15] ^ t[15];
    unique case (am)
    3'd1:    begin sub = 1'b0;   cin = co_lo;  end          // VADD
    3'd2:    begin sub = 1'b1;   cin = !co_lo; end          // VSUB
    3'd3:    begin sub = 1'b0;   cin = 1'b0;   end          // VADDC
    3'd4:    begin sub = !sign;  cin = !sign;  end          // VCH: s + t if the signs differ, else s - t
    3'd5:    begin sub = !co_lo; cin = !co_lo; end          // VCL: the same, by VCO
    3'd6:    begin sub = !sign;  cin = 1'b1;   end          // VCR: s + t + 1, or s - t
    default: begin sub = 1'b1;   cin = 1'b1;   end          // VSUBC, the compares, -t (7)
    endcase
    bb = sub ? ~t : t;
    sa = am == 3'd7 ? 16'd0 : s;
    {cout, lo, cin_x} = {1'b0, sa, 1'b1} + {1'b0, bb, cin};              // (s + bb + cin: the carry in as the chain's own bit)
    b16 = s[15] ^ bb[15] ^ cout;
    o.lo = lo;
    o.fl = {cout, b16, lo == 16'd0, &lo};
    return o;
endfunction

// EX1, a compare or a clip: which of s, t and -t (~t) the result is -- the select of lane_x2, from the adder's flags of this
// cycle and the flag registers -- registered (pk_r), so that EX2 has no compare in front of its result
function automatic logic [2:0] lane_pk(input logic [5:0] f, input logic [15:0] s, input logic [15:0] t, input logic [3:0] fr,
                                       input logic co_lo, input logic co_hi, input logic cc_lo, input logic cc_hi, input logic ce);
    logic sign, cout, b16, zero, ones, eq, z17, cs, negside, cond, le_f, ge_f;
    sign = s[15] ^ t[15];
    {cout, b16, zero, ones} = fr;
    eq = zero; z17 = zero && !b16;
    cs = f == 6'h20 ? (b16 || (eq && co_hi && co_lo)) : (!b16 && !eq) || (eq && !(co_hi && co_lo));      // VLT, VGE
    if (f == 6'h24) begin                                    // VCL: by VCO, the unsigned compare (cout is s >= t)
        le_f = (co_lo && !co_hi) ? (ce ? (zero || !cout) : (zero && !cout)) : cc_lo;
        ge_f = (!co_lo && !co_hi) ? cout : cc_hi;
        negside = co_lo;
    end else begin                                           // VCH, VCR: by the signs
        le_f = sign ? (b16 || z17) : t[15];
        ge_f = sign ? t[15] : !b16;
        negside = sign;
    end
    cond = negside ? le_f : ge_f;
    if (f == 6'h13) return {s == 16'd0, s[15] ? 2'd2 : 2'd1};         // VABS: -t for a negative s, t for a positive; 0 for 0
    if (f == 6'h20 || f == 6'h23) return {1'b0, cs ? 2'd0 : 2'd1};
    return {1'b0, !cond ? 2'd0 : !negside ? 2'd1 : (f == 6'h26) ? 2'd3 : 2'd2};
endfunction

// EX1, VADD, VSUB and VABS: whether the result is a clamp's constant (the sum overflowed; -t of -8000), from the adder's flags and
// sum of this cycle, registered (kc_r)
function automatic logic lane_kc(input logic [5:0] f, input logic [15:0] s, input logic [15:0] t, input add_t a);
    return ((f == 6'h10 || f == 6'h11) && (a.fl[2] ^ a.lo[15])) || (f == 6'h13 && s[15] && t[15] && a.lo[15]);
endfunction

// EX1, a multiply: the product into the accumulator (all 48 bits)
function automatic logic [47:0] lane_mac(input logic [5:0] f, input logic signed [33:0] p, input logic [47:0] accm);
    logic signed [47:0] term;
    logic [47:0] pe;
    logic s1, s4, s5, s7;
    pe = 48'(p);
    s1 = f[2:0] == 3'd0 || f[2:0] == 3'd1;
    s4 = f[2:0] == 3'd4;
    s5 = f[2:0] == 3'd5 || f[2:0] == 3'd6;
    s7 = f[2:0] == 3'd7;
    term = ({48{s1}} & (pe << 1)) | ({48{s4}} & {32'd0, p[31:16]}) | ({48{s5}} & pe) | ({48{s7}} & (pe << 16));
    return term + (f[3] ? $signed(accm) : (f[2:1] == 2'b00) ? 48'sh8000 : 48'sd0);
endfunction

// What EX2 asks of the function, registered with the instruction (it is the start of the result's longest path)
typedef struct packed {
    logic       cl;                 // the result and the accumulator's low word are the adder's sum
    logic       cx;                 // a logic operation, VMOV or a reciprocal: the table is ttc
    logic [3:0] ttc;
    logic       usepk;              // the choice among s, t and -t is pk_r's (EX1 made it)
    logic       vmrg;               // ... or VMRG's, by VCC
    logic       vsar;
    logic       vas;                // VADD, VSUB: the result is clamped
    logic       abs_;               // VABS: -t of -8000 is 8000
    logic       resv;               // a reserved function: 0
} ydec_t;
function automatic ydec_t ydecode(input logic [5:0] f);
    ydec_t d;
    d.cl    = f == 6'h10 || f == 6'h11 || f == 6'h14 || f == 6'h15 || is_resv(f);
    d.cx    = (f >= 6'h28 && f <= 6'h2D) || f == 6'h33 || (f >= 6'h30 && f <= 6'h36) || f == 6'h22;
    unique case (f[2:1])
    2'd0:    d.ttc = 4'b1000;                                  // s & t
    2'd1:    d.ttc = 4'b1110;                                  // s | t
    default: d.ttc = 4'b0110;                                  // s ^ t
    endcase
    if (f >= 6'h28 && f <= 6'h2D) d.ttc = d.ttc ^ {4{f[0]}};
    else if (f == 6'h22) d.ttc = 4'b1100;                      // VNE: s
    else d.ttc = 4'b1010;                                      // VMOV, VRCP..: t
    d.usepk = f == 6'h13 || f == 6'h20 || f == 6'h23 || f == 6'h24 || f == 6'h25 || f == 6'h26;
    d.vmrg  = f == 6'h27;
    d.vsar  = f == 6'h1D;
    d.vas   = f == 6'h10 || f == 6'h11;
    d.abs_  = f == 6'h13;
    d.resv  = is_resv(f);
    return d;
endfunction

// EX2, any computation but a multiply: from the registered sum (lo) and flags (fr, the adder's {carry, b16, zero,
// ones} of its compare) the result, the accumulator's low word, the flags and the register write's data
function automatic lane_t lane_x2(input logic [5:0] f, input logic [3:0] e, input logic [15:0] s, input logic [15:0] t,
                                  input logic [15:0] lo, input logic [47:0] accm, input logic [2:0] pk_in, input logic kc_in, input ydec_t yd,
                                  input logic co_lo, input logic co_hi, input logic cc_lo, input logic cc_hi, input logic ce,
                                  input logic [3:0] fr);
    lane_t o = '0;
    logic eq, sign;
    logic cs, negside, cond, le_f, ge_f, abs_sat, ovf, a_nt;
    logic [1:0] pk;
    logic [3:0] tt;
    logic [15:0] accl;
    // the result, one of four by the lane's select: 0 the accumulator's new low word (the word picked or made), 1 VSAR's
    // middle word, 2 VSAR's high word, 3 a clamp's constant (its top bit, the rest)
    logic [1:0] rsel;
    logic       c15, clo;
    logic       cout, b16, zero, ones, z17_c;
    sign = s[15] ^ t[15];
    {cout, b16, zero, ones} = fr;
    eq = zero;                                               // (s == t: where it is asked the adder subtracts)
    z17_c = zero && !b16;                                    // the signed result is 0
    o.fl = fr;
    // The result is picked (s, t, -t or ~t), taken from the adder (lo), made by a
    // logic operation or, for VSAR, from the accumulator; VADD, VSUB and VABS
    // clamp it on its way to vd, and the accumulator's low word takes it unclamped.
    // What is picked or made of s and t alone -- s, t, ~t, 0, the logic operations --
    // is a function of the lane's two bits: a table of its four values (tt, the same
    // for the lane's sixteen bits), beside the adder's sum and -t.
    o.accl_w = f != 6'h1D;
    // the compares: b16 is s < t (signed)
    unique case (f)
    6'h20:   cs = b16 || (eq && co_hi && co_lo);
    6'h23:   cs = (!b16 && !eq) || (eq && !(co_hi && co_lo));
    default: cs = cc_lo;                                     // VMRG
    endcase
    // VCH, VCL, VCR: s or t (or -t, ~t) by what the compare found. A lane's
    // conditions: le (s <= -t) and ge (s >= t), and which of them decides
    if (f == 6'h24) begin                                    // VCL: by VCO, the unsigned compare (cout is s >= t)
        le_f = (co_lo && !co_hi) ? (ce ? (zero || !cout) : (zero && !cout)) : cc_lo;
        ge_f = (!co_lo && !co_hi) ? cout : cc_hi;
        negside = co_lo;
    end else begin                                           // VCH, VCR: by the signs
        le_f = sign ? (b16 || z17_c) : t[15];
        ge_f = sign ? t[15] : !b16;
        negside = sign;
    end
    cond = negside ? le_f : ge_f;
    // (what s and t give: the table of the function, or of the choice -- VMRG's is by VCC)
    pk = yd.usepk ? pk_in[1:0] : yd.vmrg ? (cc_lo ? 2'd0 : 2'd1) : 2'd1;
    unique case (pk)
    2'd0:    tt = 4'b1100;                                   // s
    2'd1:    tt = 4'b1010;                                   // t
    default: tt = 4'b0101;                                   // ~t (-t: a_nt)
    endcase
    if (yd.usepk && pk_in[2]) tt = 4'b0000;                  // VABS of 0
    if (yd.cx) tt = yd.ttc;
    a_nt = !yd.cx && pk == 2'd2;
    for (int i = 0; i < 16; i++) accl[i] = (yd.cl || a_nt) ? lo[i] : yd.vsar ? accm[i] : tt[{s[i], t[i]}];
    o.acc[15:0] = accl;
    // vd: the clamped result, or VSAR's word of the accumulator
    // (VABS of a negative s with t = 8000: -t is 8000 again: that is t and -t, from the adder, both negative)
    abs_sat = yd.abs_;
    rsel = (kc_in && (yd.vas || yd.abs_)) ? 2'd3 : 2'd0;     // (kc_in: made in EX1: VADD, VSUB overflow, VABS of -8000)
    c15 = !abs_sat && b16;                                   // (abs_sat: 7FFF; ovf: b16, then its inverse)
    clo = abs_sat || !b16;
    if (yd.resv) begin rsel = 2'd3; c15 = 1'b0; clo = 1'b0; end
    // (VSAR: the accumulator's word e names, the lane's, or 0)
    if (yd.vsar) begin
        rsel = e == 4'd8 ? 2'd2 : e == 4'd9 ? 2'd1 : e == 4'd10 ? 2'd0 : 2'd3;
        c15 = 1'b0; clo = 1'b0;
    end
    // the flags
    unique case (f)
    6'h10, 6'h11:  o.co_w = 1'b1;                                                                    // VADD, VSUB
    6'h14:         begin o.co_lo = cout; o.co_w = 1'b1; end                                          // VADDC
    6'h15:         begin o.co_lo = !cout; o.co_hi = !eq; o.co_w = 1'b1; end                          // VSUBC
    6'h20, 6'h23:  begin o.cc_lo = cs; o.cc_w = 1'b1; o.co_w = 1'b1; end
    6'h21:         begin o.cc_lo = eq && !co_hi; o.cc_w = 1'b1; o.co_w = 1'b1; end
    6'h22:         begin o.cc_lo = !eq || co_hi; o.cc_w = 1'b1; o.co_w = 1'b1; end
    6'h24, 6'h26:  begin o.cc_lo = le_f; o.cc_hi = ge_f; o.cc_w = 1'b1; o.co_w = 1'b1; o.ce_w = 1'b1; end
    6'h25: begin                                             // VCH
        o.ce = sign && ones && b16;
        o.co_hi = sign ? (!z17_c && !(ones && b16)) : !eq;
        o.co_lo = sign; o.co_w = 1'b1;
        o.cc_lo = le_f; o.cc_hi = ge_f; o.cc_w = 1'b1;
        o.ce_w = 1'b1;
    end
    6'h27:         o.co_w = 1'b1;                                                                    // VMRG
    default: ;
    endcase
    for (int i = 0; i < 16; i++)
        unique case (rsel)
        2'd0:    o.res[i] = accl[i];
        2'd1:    o.res[i] = accm[16 + i];
        2'd2:    o.res[i] = accm[32 + i];
        default: o.res[i] = (i == 15) ? c15 : clo;
        endcase
    return o;
endfunction

// ------------------------------------------------- the computational pipeline --
// SEL takes the operands from the registers (vt through its element's broadcast) into s_r and t_r. Three kinds of
// computation follow:
//   a multiply: MUL (the products, in the DSP blocks, into p_r), EX1 (the accumulator, 48 bits), EX2 (the clamp, and vd);
//   a "fast" one -- VMRG, VMOV, the logic operations, VSAR, VRCPH and VRSQH, which have no adder in them -- goes to EX2 at once;
//   any other: EX1 (the adder: its sum and flags into lo_r and fl_r; two cycles for a clip, whose second takes -t, and for
//     a reciprocal, whose second is the normalisation and the ROM's address) and EX2 (the result, the accumulator's low
//     word and the flags, and vd).
// EX2 takes one a cycle, in the order of SEL, so vd, the accumulator and the flags are written in order with no
// forwarding: the accumulator is read and written by a multiply in its EX1 and by the rest in EX2, and the flags in EX2
// (and read for their carry by VADD, VSUB and VCL in EX1, which therefore does not start while another computation is
// in EX1). A computation that uses s and t again in EX2 (a compare, a clip, VABS) keeps SEL from replacing them.
// Which computations can start, by what is in MUL and EX1: a fast one when both are empty; a multiply unless EX1 holds
// one that needs s and t; any other when MUL is empty, EX1 is not kept (a clip, a reciprocal), and the first is not
// busy with s and t.
// A computation that reads the register another is writing waits for its write.
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
wire  fn_ch = cd.ch;

logic [15:0]        s_n [LANES], t_n [LANES];
logic [15:0]        s_r [LANES], t_r [LANES];
logic [15:0]        s_y [LANES], t_y [LANES];     // EX2's copies: of s_r and t_r as EX1 had them, or SEL's (a computation with no EX1)
always_comb
    for (int l = 0; l < LANES; l++) begin s_n[l] = qa[l]; t_n[l] = tv[l]; end

// the MUL stage
logic               m_v /* verilator public_flat_rd */;
logic [5:0]         m_fn;
logic [4:0]         m_vdn;
wire  m_ssgn = m_fn[2:0] == 3'd0 || m_fn[2:0] == 3'd1 || m_fn[2:0] == 3'd5 || m_fn[2:0] == 3'd7;
wire  m_tsgn = m_fn[2:0] == 3'd0 || m_fn[2:0] == 3'd1 || m_fn[2:0] == 3'd6 || m_fn[2:0] == 3'd7;
logic signed [33:0] p_r [LANES];
// (the products' registers: with nothing but the multipliers before them, the fitter makes them the DSP blocks' output
// registers)
always_ff @(posedge clk)
    for (int l = 0; l < LANES; l++) p_r[l] <= $signed({m_ssgn & s_r[l][15], s_r[l]}) * $signed({m_tsgn & t_r[l][15], t_r[l]});

// EX1's registers
logic               x_v /* verilator public_flat_rd */, x_wait /* verilator public_flat_rd */, x_sec, x_mul /* verilator public_flat_rd */, x_one, x_rcp;
logic               x_st /* verilator public_flat_rd */;                     // EX1 holds a computation that uses s and t in EX2
logic [5:0]         x_fn;
logic [2:0]         x_am;                     // (amode of it)
logic [3:0]         x_ve;
logic [4:0]         x_vdn;
logic [2:0]         x_de;
logic [15:0]        lo_r [LANES];             // the adder's sum, and its flags (the first cycle's)
logic [3:0]         fl_r [LANES];
logic [LANES-1:0]   kc_r;                     // the clamp's constant is the result
logic [2:0]         pk_r [LANES];             // a compare's or a clip's choice among s, t and -t
add_t               ad [LANES];
logic [2:0]         pk1 [LANES];
logic [LANES-1:0]   kc1;
logic [47:0]        mac [LANES];
always_comb
    for (int l = 0; l < LANES; l++) begin
        ad[l]  = lane_x1(x_am, s_r[l], t_r[l], vco[l]);
        kc1[l] = lane_kc(x_fn, s_r[l], t_r[l], ad[l]);
        pk1[l] = lane_pk(x_fn, s_r[l], t_r[l], ad[l].fl, vco[l], vco[4'd8 + l], vcc[l], vcc[4'd8 + l], vce[l]);
        mac[l] = lane_mac(x_fn, p_r[l], {acc_h[l], acc_m[l], acc_l[l]});
    end
wire x_nm  = x_v && !x_mul;                   // a computation that is not a multiply is in EX1
wire x_ex2 = x_v && !x_wait;                  // its last cycle: EX2 is the next

// EX2's registers
logic               y_v /* verilator public_flat_rd */, y_mul, y_one, y_rcp;
logic [5:0]         y_fn;
ydec_t              y_yd;
logic [3:0]         y_ve;
logic [4:0]         y_vdn;
logic [2:0]         y_de;
lane_t ex2 [LANES];
logic [15:0] mres [LANES];
logic [LANES-1:0] mgt_r, mlt_r;               // a multiply's sum is above / below what a word holds (made as it was: EX1)
always_comb
    for (int l = 0; l < LANES; l++) begin
        ex2[l]  = lane_x2(y_fn, y_ve, s_y[l], t_y[l], lo_r[l], {acc_h[l], acc_m[l], acc_l[l]}, pk_r[l], kc_r[l], y_yd, vco[l], vco[4'd8 + l], vcc[l], vcc[4'd8 + l], vce[l], fl_r[l]);
        mres[l] = mul_res(y_fn[2:0], {acc_h[l], acc_m[l], acc_l[l]}, mgt_r[l], mlt_r[l]);
    end
logic [15:0] hold [8];                        // EX2's results, for a write put off
logic [15:0] hold_be_w;                       // ... and the bytes it writes
// a write put off behind a load's: the registers' one write port is the load's, and vd is written a cycle on, from `hold` (wp)
logic        wp /* verilator public_flat_rd */;
logic [4:0]  w_vdn;
logic [15:0] w_be;
wire         x_defer /* verilator public_flat_rd */, y_wr;         // (below, with the load's write) ... and EX2 writes vd

// ---- the reciprocal unit. SEL takes the input (rin); EX1's first cycle makes its absolute value (r_dat) and EX1's second
// normalises it to the ROM's address; the ROM's word is shifted back in EX2
logic [31:0] rin;
logic        r_sqrt, r_zero, r_min, r_mask;
logic [31:0] r_dat;
logic [4:0]  r_sh;
wire  [31:0] r_data0 = rin ^ {32{rin[31]}};
wire         r_ninc  = &rin[31:15];                                     // (rin >= -32768 and negative: -32768 is the one value of it that
                                                                        // is not negated, and its result is r_min's, whatever r_dat is)
wire  [31:0] r_data  = r_data0 + {31'd0, r_ninc};
logic [5:0]  r_clz;
always_comb begin
    r_clz = 6'd32;
    for (int k = 0; k < 32; k++) if (r_dat[k]) r_clz = 6'(31 - k);
end
wire  [31:0] r_norm = r_dat << r_clz[4:0];
wire  [8:0]  r_idx  = r_norm[30:22];
wire  [4:0]  r_dn   = 5'd31 - r_clz[4:0];
assign rom_addr = r_sqrt ? {1'b1, r_idx[8:1], r_clz[0]} : {1'b0, r_idx};
wire  [31:0] r_val  = {1'b0, 1'b1, rom_q, 14'd0};                          // (0x10000 | rom) << 14
wire  [4:0]  r_rsh  = r_sh;                                                 // (the shift back, made with the normalisation)
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
function automatic logic [11:0] offs(input logic [31:0] i);      // the offset scaled by the access's size
    logic [11:0] of;
    of = {{5{i[6]}}, i[6:0]};
    unique case (i[15:11])
    5'd0:             return of;
    5'd1:             return {of[10:0], 1'b0};
    5'd2:             return {of[9:0], 2'b0};
    5'd3, 5'd6, 5'd7: return {of[8:0], 3'b0};
    default:          return {of[7:0], 4'b0};
    endcase
endfunction
logic [11:0] off_sh, off_sh16;              // ... and 16 more: registered with the instruction, so that the address and the next line's are two adders after rs
always_ff @(posedge clk) if (adv) begin off_sh <= offs(lins_n); off_sh16 <= offs(lins_n) + 12'd16; end
// (the offset scaled first, so that the address is one adder after rs: the
// store's rotation hangs off its low bits)
wire  [11:0] ls_addr = la[11:0] + off_sh;
wire  [11:0] ls_addr16 = la[11:0] + off_sh16;
wire  ls_packed = ld.packed_;
wire  ls_tv     = ld.tv;                                           // LTV, STV
// SHV, SFV: a byte every second / fourth of the 16 from the 8-byte block
wire  st_hv     = ld.hv;
wire  st_fv     = ld.fv;
wire  ls_known  = ld.known;
wire  [3:0]  o    = ls_addr[3:0];
wire  [4:0]  size = 5'd1 << lsop[1:0];
// the access's first word: LRV / SRV the block's, a packed load (and SHV, SFV)
// the 8-byte block's
wire  [1:0]  win_w  = lsop == 5'd5 ? 2'b00 : ((lwc2 && ls_packed) || st_hv || st_fv) ? {ls_addr[3], 1'b0} : ls_addr[3:2];
wire  [11:0] ls_win = {ls_addr[11:4], win_w, 2'b00};
wire  [11:0] ls_win16 = {ls_addr16[11:4], win_w, 2'b00};
// a load's register bytes: a run of the register's sixteen from e on -- its size's; to the 16-byte block's end
// (LQV); or (LRV) from where the block's start comes, the bytes before the address, to the register's end
function automatic logic [15:0] ld_mask_f(input logic [3:0] o_, input logic [3:0] el_, input logic [4:0] op_);
    logic [4:0] n;
    logic [5:0] lo, hi;
    logic [15:0] mk;
    n = 5'd16 - {1'b0, o_};                                                       // the bytes from the address to its block's end
    lo = {2'b00, el_};
    unique case (op_)
    5'd4:    hi = {2'b00, el_} + {1'b0, n};                                       // LQV
    5'd5:    begin lo = {2'b00, el_} + {1'b0, n}; hi = 6'd16; end                 // LRV
    default: hi = {2'b00, el_} + {1'b0, 5'd1 << op_[1:0]};                        // LBV .. LDV
    endcase
    for (int m = 0; m < 16; m++) mk[m] = 6'(m) >= lo && 6'(m) < hi;
    return mk;
endfunction
// whether that mask leaves half of a lane written, from the parities of its ends (the end of a run cut at 16 is even): the
// address's low bit, no sum, and the compare el < o that says LQV's run ends, and LRV's starts, inside the 16
wire  ld_lt = el < o;
wire  ld_mg = lsop == 5'd0 ? 1'b1 : lsop <= 5'd3 ? el[0] : lsop == 5'd4 ? (el[0] | (ld_lt & (el[0] ^ o[0]))) : lsop == 5'd5 ? (ld_lt & (el[0] ^ o[0])) : 1'b0;
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
logic [3:0]  tv_n, tv_oa;               // the beat, and its place in the 16 bytes from the window's start (counted on, wrapping)
logic [11:0] tv_base, tv_base16;      // ... the window's start, and 16 more
wire  [3:0]  tv_idx  = tv_n - {el[3:1], 1'b0};                        // STV: the register's byte
wire  [4:0]  tv_reg  = {ltn[4:3], swc2 ? tv_n[3:1] : el[3:1] + tv_n[3:1]};
wire  [11:0] tv_addr = tv_base + {8'd0, tv_oa};
wire  [11:0] tv_addr16 = tv_base16 + {8'd0, tv_oa};
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
        wp = qd[i % 8];
        wn = qd[i / 2];
        wh = (i % 2 == 1) ? {wn[7:0], qd[((i + 1) / 2) % 8][15:8]} : wn;
        wf = qd[{el[3], 1'(i / 8) ^ el[3], 1'((i / 4) % 2)}];
        unique case (st_sel[i / 8])
        2'd0: sarr[i] = (i % 2 == 1) ? wn[7:0] : wn[15:8];
        2'd1: sarr[i] = wh[14:7];
        2'd2: sarr[i] = wp[15:8];
        2'd3: sarr[i] = wp[14:7];
        endcase
        if (i % 4 == 0 && st_fv) sarr[i] = wf[14:7];
    end
end

// MFC2's bytes: those of D's copy at e and e + 1 (mod 16), not through the store's rotation
logic [127:0] qd_flat;
always_comb for (int g = 0; g < 8; g++) qd_flat[127 - 16 * g -: 16] = qd[g];
wire  [3:0]  el1  = el + 4'd1;
wire  [7:0]  mf_b0 = qd_flat[127 - 8 * int'(el) -: 8];
wire  [7:0]  mf_b1 = qd_flat[127 - 8 * int'(el1) -: 8];

// the bytes a load (or MTC2) brings, and where they go. A load's data comes
// the cycle after its address -- by then the instruction may have left E, so
// what the write needs is kept (lw_*)
logic        lw_v /* verilator public_flat_rd */, lw_last, lw_packed, lw_p6;
// the write leaves half of a lane as it was (an odd byte at an end): a store
// or MFC2 does not start under it (l_free)
logic        lw_mgf;                  // the write leaves half a lane whatever its bytes: MTC2's, LTV's
wire         lw_merge = lw_mgf;
logic [4:0]  lw_reg;
logic [1:0]  lw_k;                    // what it is: 0 a load, 1 MTC2, 2 a beat of LTV
logic [3:0]  lw_o, lw_el;             // ... the load's address (low four bits) and element; MTC2's element; the beat's byte
logic [4:0]  lw_op;
logic [15:0] lw_be;                   // the register's bytes written: made from those, in the cycle of the write
logic        lw_mt;                   // ... MTC2's: rt's two bytes (lw_mb: the even byte's and the odd one's)
logic [15:0] lw_mb;
wire         idle    = st == V_IDLE && !m_v && !x_v && !y_v && !lw_v && !wp;  // nothing under way: the registers and flags are final
// MTC2: rt's two bytes at e and e + 1 (one at the register's end), written the cycle after it starts, as a load's
always_comb lw_be = lw_k == 2'd1 ? ((16'd1 << lw_el) | (lw_el == 4'd15 ? 16'd0 : 16'd2 << lw_el)) : lw_k == 2'd2 ? 16'd1 << lw_el : ld_mask_f(lw_o, lw_el, lw_op);
wire  [15:0] in_be   = lw_be;
wire         in_v    = lw_v;
wire         in_pk   = lw_v && lw_packed;
wire  [4:0]  rb_live = rd;            // the register the instruction in E reads (MFC2 / MTC2: rd; a store: vt)
logic [15:0] ld_d [8];
always_comb
    for (int g = 0; g < 8; g++) begin
        logic [7:0] dh, dl, dp;
        dh = lw_mt ? lw_mb[15:8] : m_rdata[127 - 16 * g -: 8];                    // register byte 2g
        dl = lw_mt ? lw_mb[7:0]  : m_rdata[119 - 16 * g -: 8];                    // ... 2g + 1
        dp = m_rdata[127 - 8 * g -: 8];                                           // a packed load's lane g
        if (in_pk) ld_d[g] = lw_p6 ? {dp, 8'd0} : {1'b0, dp, 7'd0};
        else       ld_d[g] = {dh, dl};
    end

// ----------------------------------------------------------- the controls --
// a computational instruction's SEL: not under a computation that writes what it reads (in MUL, EX1 or EX2), nor under a
// load's write of what it reads; not behind a computation in EX1 (above), nor, if it is not a multiply, behind one in MUL
wire cmp      /* verilator public_flat_rd */ = valid && vec && fn_known && !fn_none;
wire dep      /* verilator public_flat_rd */ = (m_v && (m_vdn == vsn || m_vdn == vtn)) || (x_v && (x_vdn == vsn || x_vdn == vtn))
             || (y_v && (y_vdn == vsn || y_vdn == vtn)) || (wp && (w_vdn == vsn || w_vdn == vtn));
// (a load's write of another register is no matter: the operands come from
// the copies the instruction addresses itself, and this one's own write is a
// cycle on. Microcode loads a register and computes with others at once.)
wire ldep     /* verilator public_flat_rd */ = lw_v && (lw_reg == vsn || lw_reg == vtn);
wire pipe_e   = !m_v && !x_v && !y_v && !wp;          // no computation is under way
// the kinds: fast ones (no adder), compares and clips (s and t used in EX2), the rest (adds, reciprocals)
wire fn_fast  /* verilator public_flat_rd */ = cd.fast;
wire fn_cmp   = cd.cmp;
wire fn_cin   /* verilator public_flat_rd */ = cd.cin;
wire x_kept   = x_v && x_wait;                                                // EX1 holds its computation another cycle
wire go_k     /* verilator public_flat_rd */ = fn_fast ? !x_kept && !m_v          // (a fast one goes to EX2 at once if EX1 is empty, else through EX1)
              : fn_mul  ? !x_kept
              :           !x_kept && !m_v && !(fn_cin && x_v && !x_mul);
`ifdef VERILATOR
logic dbg_drain /* verilator public_flat_rw */;                              // (simulation) wait for the pipeline to empty before each computation
`else
wire dbg_drain = 1'b0;
`endif
wire sel0r    /* verilator public_flat_rd */ = cmp && st == V_IDLE && !ldep && go_k && !dep && !(dbg_drain && !pipe_e);
wire none_ok  = valid && vec && (fn_none || !fn_known);                       // (one that does nothing: done at once)
wire sel_fire /* verilator public_flat_rd */ = sel0;
// the others: a load's first cycle reads no register, so it starts under anything that writes another register (its
// write, a cycle on, may meet a computation's, which is put off); a store or MFC2 under a computation that writes
// another register than the one it reads, and under a load's write of another that is of whole lanes; MTC2 as a load;
// CFC2 and CTC2 when no computation is under way (the flags they read and write are final then)
wire x_free   = !(m_v && m_vdn == rb_live) && !(x_v && x_vdn == rb_live) && !(y_v && y_vdn == rb_live) && !(wp && w_vdn == rb_live);
wire l_free   = !lw_v || (!lw_merge && lw_reg != rb_live);
// (a load into the register a computation is to write would be written over by it, put off or not)
logic [4:0] l_reg;                                            // (the register a load or MTC2 writes: registered with the instruction)
wire  mtc2_n = lins_n[31:26] == 6'h12 && !lins_n[25] && lins_n[24:21] == 4'd4;     // (MTC2, of the instruction entering E)
always_ff @(posedge clk) if (adv) l_reg <= mtc2_n ? lins_n[15:11] : lins_n[20:16];
wire ld_ok    = !(m_v && m_vdn == l_reg) && !(x_v && x_vdn == l_reg) && !x_defer;
wire ls_go    = ld.go;
wire m_stall  = swc2 ? m_stall_st : m_stall_ld;                              // (the port, for the load or store of E)
wire start_r  = (lp || (valid && !vec)) && st == V_IDLE
              && (ls_tv ? idle : (lwc2 || mtc2) ? ld_ok : (swc2 || mfc2) ? (l_free && x_free) : (cfc2 || ctc2) ? pipe_e : 1'b1)
              && !(m_stall && ls_go);                       // (a load or store waits for the port)
// a pair goes as one: its computation's SEL and the other's start in the same cycle, or neither
wire sel0     = sel0r && (!lp || start_r);
wire start_nc = start_r && (!lp || sel0r || none_ok);
// a load's read goes out this cycle, or MTC2 starts (its write is the next)
wire lw_nx    = (lwc2 && m_valid) || (mtc2 && start_nc);
// D's address next cycle: the register STV is at then (two beats each), else the one E's instruction reads
wire         stv_n   = swc2 && ((st == V_IDLE && start_nc && ls_tv) || (in_tv && !(tv_n == 4'd15 && !m_stall)));
wire  [4:0]  tv_nn   = {1'b0, tv_n} + 5'd1;
wire  [2:0]  tvr_n   = !in_tv ? 3'd0 : m_stall ? tv_n[3:1] : tv_nn[3:1];
assign rdd_n = stv_n ? {ltn[4:3], tvr_n} : rd_n;
// CFC2's flags, the cycle after it starts
logic        cf_v;                // CFC2 started last cycle
logic        ct_v;                // CTC2 started last cycle (its write is now)
logic [1:0]  ct_r;
logic [15:0] ct_d;
logic [1:0]  cf_r;
logic        mf_q;                // MFC2 started last cycle: its value (mf_val) is res_l
logic [31:0] mf_val;
always_comb
    res_l = mf_q ? mf_val : cf_r == 2'd0 ? {{16{vco[15]}}, vco} : cf_r == 2'd1 ? {{16{vcc[15]}}, vcc} : {24'd0, vce};

// EX2's write (a load's write of this cycle goes first: EX2's is put off, whole, a cycle on)
assign x_defer = y_v && (lw_v || wp || y_rcp);               // (a reciprocal always: its result comes from the ROM, late in the cycle)
assign y_wr    = y_v && !x_defer;

always_comb begin
    // DMEM
    m_valid = (start_nc && ls_go) || (in_tv && !m_stall);
    m_we    = swc2;
    m_addr  = in_tv ? tv_addr : ls_win;
    m_addr16 = in_tv ? tv_addr16 : ls_win16;
    m_be    = in_tv ? 16'd1 << tv_addr[3:0] : st_be;
    m_rot   = in_tv ? tv_addr[3:0] - tv_n : o - el;
    for (int i = 0; i < 16; i++) m_wpre[127 - 8 * i -: 8] = sarr[i];
    m_wsh = st_rot;
    // done, and the value MFC2 / CFC2 give
    done = 1'b0;
    if (sel0) done = 1'b1;                                      // the rest runs behind the next instruction
    if (none_ok && (!lp || start_r)) done = 1'b1;
    if (start_nc) begin
        if (mfc2 || cfc2 || mtc2 || ctc2) done = 1'b1;           // (MTC2's write comes behind)
        if ((lwc2 || swc2) && !ls_known) done = 1'b1;
        if (ls_go) done = 1'b1;                                 // (a load's write comes behind)
        if (lp && !(sel0r || none_ok)) done = 1'b0;             // (a pair: not without its computation)
    end
    if (in_tv && tv_n == 4'd15 && !m_stall) done = 1'b1;
    res = 32'd0;
    // the register write: EX2's, a load's bytes, MTC2
    // (a load's write, in_v, cannot meet EX2's -- that is put off, x_defer -- nor a put-off write's: it is the last choice)
    wr_reg = wp ? w_vdn : y_vdn;
    wr_be = wp ? w_be : y_wr ? (y_one ? 16'd3 << {y_de, 1'b0} : 16'hFFFF) : 16'd0;
    wr_en = wp || y_wr || in_v;
    for (int g = 0; g < 8; g++) wr_d[g] = hold[g];
    if (!wp) for (int l = 0; l < LANES; l++) wr_d[l] = y_mul ? mres[l] : ex2[l].res;
    if (in_v) begin
        wr_reg = lw_reg;
        wr_be = in_pk ? 16'hFFFF : in_be;
        for (int g = 0; g < 8; g++) wr_d[g] = ld_d[g];
    end
    // (simulation) quiescent: the registers, accumulator and flags are final
    commit = {3'd0, idle && !sel_fire && !start_nc};
    commit_part = 1'b0;
end

always_ff @(posedge clk) begin
    if (rst) begin
        st <= V_IDLE; m_v <= 1'b0; x_st <= 1'b0; x_v <= 1'b0; y_v <= 1'b0; lw_v <= 1'b0; unimpl <= 1'b0; x_wait <= 1'b0; x_sec <= 1'b0;
        wp <= 1'b0; mf_q <= 1'b0; lw_mgf <= 1'b0;
        vco <= 16'd0; vcc <= 16'd0; vce <= 8'd0; dph <= 1'b0; div_out <= 32'd0; div_in_h <= 16'd0;
        for (int g = 0; g < 8; g++) begin acc_h[g] <= 16'd0; acc_m[g] <= 16'd0; acc_l[g] <= 16'd0; end
    end else begin
        lw_v <= 1'b0;
        // the reciprocal: absolute value and normalisation, a cycle each, from the input taken at SEL
        r_dat <= r_data; r_sh <= r_sqrt ? {1'b0, r_dn[4:1]} : r_dn; r_mask <= rin[31];
        r_zero <= rin == 32'd0; r_min <= rin == 32'hFFFF_8000;
        if (valid && vec && !fn_known) unimpl <= 1'b1;
        if (start_nc && (lwc2 || swc2) && !ls_known) unimpl <= 1'b1;

        // ---- EX2
        // (EX2 takes EX1's computation, or SEL's -- a fast one, when EX1 is empty -- whatever SEL does: only y_v is its choice)
        y_v <= x_ex2 || (sel0 && fn_fast && !x_v);
        if (x_v) begin
            y_fn <= x_fn; y_yd <= ydecode(x_fn); y_ve <= x_ve; y_vdn <= x_vdn; y_de <= x_de; y_one <= x_one; y_rcp <= x_rcp; y_mul <= x_mul;
        end else begin
            y_fn <= fn; y_yd <= ydecode(fn); y_ve <= ve; y_vdn <= vdn; y_de <= de; y_one <= fn_one; y_rcp <= fn_rcp; y_mul <= 1'b0;
        end
        if (y_v) begin
            for (int l = 0; l < LANES; l++) begin
                if (!y_mul) begin
                    if (ex2[l].accl_w) acc_l[l] <= ex2[l].acc[15:0];
                    if (ex2[l].co_w) begin vco[l] <= ex2[l].co_lo; vco[8 + l] <= ex2[l].co_hi; end
                    if (ex2[l].cc_w) begin vcc[l] <= ex2[l].cc_lo; vcc[8 + l] <= ex2[l].cc_hi; end
                    if (ex2[l].ce_w) vce[l] <= ex2[l].ce;
                end
                hold[l] <= y_mul ? mres[l] : ex2[l].res;
            end
            if (y_rcp) begin
                if (y_fn[1:0] != 2'd2) div_out <= r_res;
                for (int g = 0; g < 8; g++) hold[g] <= y_fn[1:0] == 2'd2 ? div_out[31:16] : r_res[15:0];
            end
        end
        wp <= x_defer;
        if (x_defer) begin w_vdn <= y_vdn; w_be <= y_one ? 16'd3 << {y_de, 1'b0} : 16'hFFFF; end

        // ---- EX1
        if (x_v) begin
            if (x_mul) begin
                for (int l = 0; l < LANES; l++) begin
                    acc_h[l] <= mac[l][47:32]; acc_m[l] <= mac[l][31:16]; acc_l[l] <= mac[l][15:0];
                    mgt_r[l] <= !mac[l][47] && |mac[l][46:31]; mlt_r[l] <= mac[l][47] && !(&mac[l][46:31]);
                end
            end else begin
                for (int l = 0; l < LANES; l++) begin
                    lo_r[l] <= ad[l].lo;
                    if (!x_sec) begin fl_r[l] <= ad[l].fl; pk_r[l] <= pk1[l]; end
                    kc_r[l] <= kc1[l];    // (a clip's second cycle makes -t: its flags are the first's)
                end
            end
        end
        x_sec <= x_v && x_wait;

        // ---- SEL
        // (s and t are taken every cycle but while EX1 holds a computation for another cycle: no SEL is in front of their enable)
        for (int l = 0; l < LANES; l++) begin s_y[l] <= x_v ? s_r[l] : s_n[l]; t_y[l] <= x_v ? t_r[l] : t_n[l]; end
        if (!(x_v && x_wait)) begin
            for (int l = 0; l < LANES; l++) begin s_r[l] <= s_n[l]; t_r[l] <= t_n[l]; end
            // the reciprocal's input: 16 bits, or with the high half a VRCPH / VRSQH left
            rin <= (fn[1:0] == 2'd1 && dph) ? {div_in_h, qc[ve[2:0]]} : {{16{qc[ve[2:0]][15]}}, qc[ve[2:0]]};
            r_sqrt <= fn[2];
        end
        if (sel_fire) begin
            if (fn_rcp) begin
                // the divide registers' state after it (in program order, as the SEL's are)
                dph <= fn[1:0] == 2'd2;
                if (fn[1:0] == 2'd2) div_in_h <= qc[ve[2:0]];
            end
        end

        // ---- the MUL stage, and EX1's control: a computation in EX1 for a second cycle keeps it (a clip's second cycle makes
        // -t); otherwise it is taken from the MUL stage, or from SEL -- a multiply goes to the MUL stage and a fast
        // one to EX2, and what EX1 then holds is not valid (x_v): SEL's decision is only in x_v and m_v
        m_v <= sel0 && fn_mul;
        m_fn <= fn; m_vdn <= vdn;
        if (x_v && x_wait) begin
            x_wait <= 1'b0;
            if (x_am == 3'd4 || x_am == 3'd5 || x_am == 3'd6) x_am <= 3'd7;
        end else begin
            x_v <= m_v || (sel0 && !fn_mul && (!fn_fast || x_v));
            x_mul <= m_v;
            x_st <= !m_v && fn_cmp;
            x_wait <= !m_v && (fn_ch || (fn_rcp && !fn_fast));
            x_fn <= m_v ? m_fn : fn; x_vdn <= m_v ? m_vdn : vdn;
            x_am <= m_v ? 3'd0 : cd.am; x_ve <= m_v ? 4'd0 : ve; x_de <= m_v ? 3'd0 : de;
            x_one <= !m_v && fn_one; x_rcp <= !m_v && fn_rcp;
        end

        // ---- the others
        unique case (st)
        V_IDLE: if (start_nc) begin
            if (ls_tv && ls_known) begin
                tv_n <= 4'd0; tv_base <= {ls_addr[11:3], 3'd0}; tv_base16 <= {ls_addr[11:3], 3'd0} + 12'd16;
                tv_oa <= swc2 ? {1'b0, ls_addr[2:0]} - {el[3:1], 1'b0} : el + {ls_addr[3], 3'd0};
                st <= V_TV;
            end
        end
        V_TV: if (!m_stall) begin
            tv_n <= tv_n + 4'd1; tv_oa <= tv_oa + 4'd1;
            if (tv_n == 4'd15) st <= V_IDLE;
        end
        default: st <= V_IDLE;
        endcase
        // MFC2: the two bytes of the register from byte e on (D's copy), the cycle after
        mf_q <= start_nc && mfc2; mf_val <= {{16{mf_b0[7]}}, mf_b0, mf_b1};
        // CFC2: its flags the cycle after
        cf_v <= start_nc && cfc2; cf_r <= li[12:11];
        // CTC2: the flag register written the cycle after
        ct_v <= start_nc && ctc2; ct_r <= li[12:11]; ct_d <= b[15:0];
        if (ct_v) begin
            if (ct_r == 2'd0) vco <= ct_d;
            else if (ct_r == 2'd1) vcc <= ct_d;
            else vce <= ct_d[7:0];
        end
        // MTC2's bytes, written the cycle after
        if (mtc2 && start_nc) begin
            lw_v <= 1'b1; lw_reg <= li[15:11]; lw_k <= 2'd1; lw_el <= el; lw_packed <= 1'b0; lw_last <= 1'b1; lw_mt <= 1'b1;
            lw_mb <= el[0] ? {b[7:0], b[15:8]} : b[15:0];
            lw_mgf <= 1'b1;
        end
        // a load's bytes: on m_rdata the next cycle
        if (lwc2 && m_valid) begin
            lw_v <= 1'b1; lw_reg <= ltn; lw_k <= 2'd0; lw_o <= o; lw_el <= el; lw_op <= lsop; lw_mt <= 1'b0;
            lw_packed <= ls_packed; lw_p6 <= lsop == 5'd6; lw_last <= 1'b1;
            lw_mgf <= ld_mg;                                                  // (a packed load writes whole lanes)
            if (in_tv) begin
                lw_k <= 2'd2; lw_el <= tv_n; lw_reg <= tv_reg; lw_packed <= 1'b0; lw_last <= tv_n == 4'd15;
                lw_mgf <= 1'b1;
            end
        end
    end
end

endmodule
`default_nettype wire
