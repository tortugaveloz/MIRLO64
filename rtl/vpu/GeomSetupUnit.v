// ============================================================================
// GeomSetupUnit -- scalar helpers for the geom core's CFU (instanced inside
// Vpu4DFixed, which owns the CFU handshake and forwards these ids).
//
// Why this and not a wider VPU: on a real game frame the 4-lane
// MATVEC ops account for 1.2 % of the geom core's cycles. What the core
// actually spent its time on was scalar work around them -- triangle setup's
// 64-bit blends and their float encoding, reciprocals, fx_mul(), float <->
// fixed conversions -- and every op here replaces one of those, BIT-EXACTLY:
// the host reference build keeps running the C (lang/c/geom/geom_triangle.c,
// geom_fixed.c, geom_pipeline.c), sim/vpu/tb_setupunit.cpp checks every op
// against a verbatim copy of it, and sim/geom_full/check_host_vs_rtl.sh
// checks whole frames.
//
// Area: every `*` in Verilog is its own multiplier to Quartus. The first
// version wrote one per operand pair and state (13 multipliers of 32x32) and
// did not fit: 2,617 ALMs and 31 DSP blocks, the device's last DSPs, with the
// overflow built in LUTs. Now there are exactly three, one per blend lane
// (p0..p2 <= mx*my every cycle), and the FSM steers their operands; fx_mul /
// fx_div_norm share lane 0.
//
// function id map (see lang/c/geom/geom_vpar.h):
//   0x04  W[rs2] = rs1 (0..5), saturated to 27 bits -- only with SU_WLOAD
//         (the unit test); the geom core's weights come from SETUP (0x67)
//           W0,W1 value (wN[1], wN[2])   W2,W3 d/dx   W4,W5 d/dy
//   0x40  latch a1 = rs1, a2 = rs2
//   0x41  rs1 = a0, rs2 = s: d1 = sat20(a1-a0), d2 = sat20(a2-a0), then
//           V = fxr((a0 << 16) + d1*W0 + d2*W1, s)
//           X = fxr(d1*W2 + d2*W3, s)      Y = fxr(d1*W4 + d2*W5, s)  (49-bit sums)
//         rd = V. fxr(p, s) = p / 2^s, halves away from zero, saturated to
//         int32: the sums are value * 2^32, so s = 8 gives an x2^24 colour
//         word, s = 2 depth (x2^30), s = 4 texture (x2^28).
//   (The 3-term form 0x44/0x47 for w is gone: fog is per vertex, in the
//    colour, as on the N64, so nothing interpolates w.)
//   (X and Y of a blend reach the firmware only through the output buffer:
//    0x46 deposits them, 0x5A reads them back)
//   0x50  rs1 = a (2^17 <= a <= 2^31): rd ~ 2^(32+s) / a,
//         s = max(msb(a) - 9, 16)                     (fx_recip_norm().r)
//         A 512-entry seed table (the N64 RSP's VRCP scheme) and one Newton
//         step on lane 0's multiplier, 7 cycles; relative error <= 2^-19.8.
//         Exactly the integer steps of fx_recip_norm() in geom_fixed.c.
//   0x51  rd = s - 16 of the last 0x50                (fx_recip_norm().e)
//   0x52  rd = fx_mul(rs1, rs2): S15.16 product, halves rounded away from
//         zero, saturated to int32
//   0x53  rd = fx_div_norm(rs1, k): k.r = rs2, k.e = the current e (set by
//         the last 0x50, or by 0x57 when the reciprocal came from software)
//   0x57  e = rs1 (fx_recip_norm_t.e for 0x53)
//   0x54  rd ~ 2^20 / sqrt(len2) (0 for 0): light_vertices()'s normal
//         renormalisation factor, len2 < 2^16 (an int8 normal's is < 49152).
//         A 1/sqrt seed table (the RSP's VRSQ) and a shift, 3 cycles; the
//         same steps as normal_f() in geom_pipeline.c.
//   0x58  projection constants: rs2 = 0 sx, 1 tx, 2 sy, 3 ty (S15.16
//         viewport scale/translate), 4 the MVP column shifts packed as
//         rs1 = sh0 | sh1 << 4 | sh2 << 8 | sh3 << 12
//   0x59  PROJECT the last MATVEC (vo0..vo3): c_j = col(vo_j, sh_j), then for
//         |w| >= 2^17: x = sat(fx_mul(c0/w, sx) + tx),
//         y = sat(ty - fx_mul(c1/w, sy)), z = fx_mul(fx_mul(c2/w, 1/2) + 1/2,
//         65534) with /w = the RECIPN + fx_div_norm pair above. rd = w;
//         |w| < 2^17 (w ~ 1: orthographic content) stops after the c_j and
//         leaves the rest to the C. Exactly load_vertices()'s steps.
//         Results go to the output buffer: x, y, z, c0, c1, c2 at out[56..61]
//   0x5A  rd = out[rs2[5:0]] (a cycle more than a register read)
//   0x46  0x41 that also writes V, X, Y to the output buffer at
//         rs2[13:8], rs2[21:16], rs2[29:24] (rs2[3:0] = s as before)
//   0x60  out[rs2[5:0]] = rs1
//   0x61  PUSH: out[0 .. rs1-1] to out_valid/out_data, a word per cycle
//         while out_ready; answers once the last word is taken, so the
//         core's later CSR writes to RDP stay behind it.
//   0x62  edge-setup config: rs2 = 0..3 the scissor start x, start y, end x,
//         end y in edge units (x << 5, 0 <= v < 2^16), 4 the cull flag
//         (rs1[0]: 1 = drop back faces, g_geom_cull), 5 the crack grow
//         (rs1[0]: 1 = wInit += (|wXInc| + |wYInc|) / 2, GEOM_CRACK_FIX)
//   0x64, 0x65, 0x66  vertex 0, 1, 2 of a triangle: rs1 = screen x, rs2 =
//         screen y (S15.16) -> to_edge_fixed(), the overflow guard, the
//         bounding box's running min/max. 0x66 then takes the signed area and
//         the cull sign: rd = 1 if the triangle is rejected (edges_of()).
//   0x67  SETUP the triangle of the last 0x64..0x66 (geom_triangle_setup_e()'s
//         integer part): bounding box clamped to the scissor, 1/area, and per
//         edge wInit/wXInc/wYInc -- descriptor words 1..11 go to out[4..14]
//         with the top-left bias applied; the normalised weights wN/wXN/wYN
//         (from the unbiased values) go to W0..W5 for the blends that follow.
//         rd = 0 if the clamped box is empty (not drawn), else 1.
//         Edge products run on their own two 18x18 multipliers (one DSP).
//   The output buffer (64 x 32, one RAM block) holds a whole triangle packet:
//   FTE header, OP_TRIANGLE_STREAM word, descriptor -- the geom core used to
//   read every result back and store each word to the command CSR itself.
//   (0x55/0x56, float<->fixed conversions, were removed on 2026-09-23: the
//    GDL and the triangle stream no longer carry floats at all.)
// ============================================================================

`default_nettype none

module GeomSetupUnit (
    input  wire        clk,
    input  wire        resetn,
    input  wire        start,          // one-cycle pulse: a command for us
    input  wire [9:0]  fid,
    input  wire [31:0] in0,
    input  wire [31:0] in1,
    output reg         done,           // one-cycle pulse with `result`
    output reg  [31:0] result,
    // PROJECT (0x59) answers w as soon as it has it and finishes the divides
    // in the background: no command may start while this is set
    output reg         bg,
    // the MATVEC4 outputs of the enclosing Vpu4DFixed, sign-extended (0x59)
    input  wire signed [31:0] vo0, vo1, vo2, vo3,
    // PUSH (0x61): the output buffer, word by word, to RDP's command FIFO
    output reg         out_valid,
    input  wire        out_ready,
    output wire [31:0] out_data
);
    // W0..W5 are written only by SETUP (0x67), straight from lane 0's
    // rounding. SU_WLOAD adds 0x04 (W[rs2] = rs1) for the unit test's
    // arbitrary weights, through one registered write port.
    // N64-class precision (the RSP's multiply-accumulate is 16-bit lanes into
    // 48-bit accumulators): weights saturate to 27 bits (+-1024.0 in S15.16;
    // only extreme slivers get near), attribute differences to 20 bits
    // (+-8.0: colour, z and texture all fit), so the blend lanes are 20x27
    // -- one DSP block each, no partial products summed in logic -- and the
    // sums are exact in 49 bits. geom_triangle.c's C does the same.
`ifndef SU_NO_BLEND
    reg signed [26:0] w [0:5];
`endif
    function signed [26:0] sat27;
        input [31:0] v;
        begin
            if (!v[31] && v[30:26] != 5'd0)          sat27 = 27'sh3FFFFFF;
            else if (v[31] && v[30:26] != 5'h1F)     sat27 = -27'sh4000000;
            else                                     sat27 = v[26:0];
        end
    endfunction
    function signed [19:0] sat20;
        input signed [32:0] v;
        begin
            if (v > 33'sh7FFFF)          sat20 = 20'sh7FFFF;
            else if (v < -33'sh80000)    sat20 = -20'sh80000;
            else                         sat20 = v[19:0];
        end
    endfunction
`ifdef SU_WLOAD
    reg        w_we;
    reg [3:0]  w_wa;
    reg [31:0] w_wd;
    always @(posedge clk)
        if (w_we)
            case (w_wa)
                4'd0: w[0] <= sat27(w_wd); 4'd1: w[1] <= sat27(w_wd); 4'd2: w[2] <= sat27(w_wd);
                4'd3: w[3] <= sat27(w_wd); 4'd4: w[4] <= sat27(w_wd); 4'd5: w[5] <= sat27(w_wd);
                default: ;
            endcase
`endif
`ifndef SU_NO_BLEND
    reg signed [31:0] a0r, a1r, a2r;
    reg signed [19:0] d2r;
    // the blend's differences, exact (33 bits) then saturated
    wire signed [19:0] bd1 = sat20($signed({a1r[31], a1r}) - $signed({in0[31], in0}));
    wire signed [19:0] bd2 = sat20($signed({a2r[31], a2r}) - $signed({in0[31], in0}));
`endif

    // ---- the three multipliers: p = mx * my, every cycle ------------------
    // Every blend step multiplies ONE difference (d1, then d2) by three
    // weights, one per output -- so mx is shared, and the weights are picked
    // straight out of w[] by `wsel` rather than copied into operand
    // registers. Lane 0 alone also serves fx_mul/fx_div_norm/RECIPN/PROJECT
    // (my0 = mm_b) and stays 33x33; lanes 1 and 2 only ever blend: 20x27.
    reg signed [32:0] mx;
    reg        [1:0]  wsel;             // 0: W0/W2/W4  1: W1/W3/W5  3: lane 0 = mm_b
    reg signed [32:0] mm_b;
    reg signed [65:0] p0;
`ifndef SU_NO_BLEND
    wire signed [32:0] my0 = (wsel == 2'd0) ? {{6{w[0][26]}}, w[0]} : (wsel == 2'd1) ? {{6{w[1][26]}}, w[1]} : mm_b;
    wire signed [26:0] my1 = (wsel == 2'd0) ? w[2] : w[3];
    wire signed [26:0] my2 = (wsel == 2'd0) ? w[4] : w[5];
    wire signed [19:0] mxb = mx[19:0];  // a blend's mx is a 20-bit difference
    reg signed [46:0] p1, p2;
    always @(posedge clk) begin
        p0 <= mx * my0;
        p1 <= mxb * my1;
        p2 <= mxb * my2;
    end
`else
    // SU_NO_BLEND (RDP firmware: RDP does the attribute setup): lane 0 only
    always @(posedge clk) p0 <= mx * mm_b;
`endif
    function signed [32:0] sx33;
        input signed [31:0] v;
        begin sx33 = {v[31], v}; end
    endfunction

`ifndef SU_NO_BLEND
    reg signed [48:0] sv, sx, sy;       // exact: |a0 << 16| < 2^47, |d*W| < 2^45

    // ---- fxr(): p / 2^s, halves away from zero, saturated to int32 --------
    // One rounder, used for V, X and Y in turn (S_C0..S_C2).
    reg  [3:0]         rsh;              // s of the op in flight: 2, 4 or 8
    reg  signed [48:0] rq_in;
    reg  signed [48:0] rq;
    reg         [31:0] rq_out;
    always @(*) begin
        case (rsh)
            4'd2:    rq = (rq_in + (rq_in[48] ? 49'sd1   : 49'sd2  )) >>> 2;
            4'd4:    rq = (rq_in + (rq_in[48] ? 49'sd7   : 49'sd8  )) >>> 4;
            default: rq = (rq_in + (rq_in[48] ? 49'sd127 : 49'sd128)) >>> 8;
        endcase
        if (!rq[48] && rq[47:31] != 17'd0)                rq_out = 32'h7FFFFFFF;
        else if (rq[48] && rq[47:31] != 17'h1FFFF)        rq_out = 32'h80000000;
        else                                              rq_out = rq[31:0];
    end
`endif

    // ---- fx_mul / fx_div_norm rounding, from lane 0's product --------------
    reg        [4:0]  rc_e;
    reg        [31:0] rc_res;           // the last RECIPN result (k.r)
    reg signed [63:0] fm_q;
    reg        [63:0] fm_u;
    reg        [31:0] fm_res;
    reg signed [31:0] dn_hi;
    reg        [31:0] dn_res;
    always @(*) begin
        // fx_mul() rounds halves away from zero: -(((-p) + 2^15) >> 16) for
        // p < 0, which equals floor((p + 2^15 - 1) / 2^16) -- one add with a
        // sign-dependent constant instead of a 64-bit negate on each side.
        fm_u = 64'd0;
        fm_q = ($signed(p0[63:0]) + (p0[65] ? 64'sd32767 : 64'sd32768)) >>> 16;
        // |p| < 2^62 (|a|,|b| <= 2^31), so fm_q fits 47 bits: saturate unless
        // bits 46..31 all equal the sign
        if (!fm_q[63] && fm_q[62:31] != 32'd0)       fm_res = 32'h7FFFFFFF;
        else if (fm_q[63] && fm_q[62:31] != 32'hFFFF_FFFF) fm_res = 32'h80000000;
        else                                          fm_res = fm_q[31:0];
        dn_hi = p0[63:32];
        // $signed() on EVERY operand: an unsigned {..} concatenation in the
        // sum would make the whole expression unsigned and turn >>> into a
        // logical shift (it did -- every negative x with e > 0 came back
        // near 2^31 in tb_setupunit.cpp).
        if (rc_e == 5'd0) dn_res = $signed(dn_hi) + $signed({31'd0, p0[31]});
        else              dn_res = $signed(dn_hi >>> rc_e) + $signed({31'd0, dn_hi[rc_e - 5'd1]});
    end

    // fm_res registered for PROJECT's viewport/depth steps: lane 0's product
    // -> 64-bit rounding -> saturation -> sat_add -> mx/px/py in one cycle
    // was the sys domain's slowest path (60.9 MHz); 62.83 MHz needs it cut.
    reg        [31:0] fm_r;
    always @(posedge clk) fm_r <= fm_res;

    // ---- normalised reciprocal: seed table + one Newton step --------------
    // The table is tools/gen_rcp_tab.py's (the C includes the same values):
    // entry i = round(2^17 / (1 + (i + 0.5)/512)) - 2^16. Registered read,
    // so it infers a ROM in one RAM block.
    reg [15:0] rcp_tab [0:511];
    reg [15:0] rsq_tab [0:511];         // NORMF (0x54), same generator
`include "GeomRcpTab.vh"
    reg [8:0]  rc_idx;
    reg [15:0] rc_seed;
    always @(posedge clk) rc_seed <= rcp_tab[rc_idx];
    reg [8:0]  nf_idx;
    reg [15:0] nf_seed;
    reg signed [3:0] nf_sh;             // 4 - e/2: -3..4
    always @(posedge clk) nf_seed <= rsq_tab[nf_idx];
    reg [31:0] rc_m;                    // a << (31 - msb): [2^31, 2^32)
    reg [3:0]  rc_sh;                   // 9 + s - msb: final left shift, 0..8
    reg signed [31:0] rc_r0;            // the seed, ~2^23 / m'

    reg        proj;                    // RECIPN runs for PROJECT
    // ---- PROJECT (0x58..0x5A) ---------------------------------------------
    reg signed [31:0] pv_sx, pv_tx, pv_sy, pv_ty;
    reg [2:0]  pv_sh0, pv_sh1, pv_sh2, pv_sh3;
    // No result registers: c0..c2, x, y, z go to the output buffer
    // (out[56..61], read back with 0x5A), and the divides re-derive c_j from
    // the MATVEC outputs, which hold still until the next MATVEC.
    reg signed [31:0] pw, pq;           // w; the quotient in flight (P3..P6)
    reg        p_neg;
    reg [1:0]  pj;                      // column being shifted (one shifter, 4 cycles)
    wire signed [31:0] pcol = colfx(pj == 2'd0 ? vo0 : pj == 2'd1 ? vo1 : pj == 2'd2 ? vo2 : vo3,
                                    pj == 2'd0 ? pv_sh0 : pj == 2'd1 ? pv_sh1 : pj == 2'd2 ? pv_sh2 : pv_sh3);
    // vpar_col_to_fx(): << sh, saturating
    function signed [31:0] colfx;
        input signed [31:0] raw;
        input [2:0] sh;
        begin
            if (raw > (32'sh7FFFFFFF >>> sh))                colfx = 32'sh7FFFFFFF;
            else if (raw < ($signed(32'h80000000) >>> sh))   colfx = $signed(32'h80000000);
            else                                             colfx = raw <<< sh;
        end
    endfunction
    // fx_add_i() / fx_sub_i(): on overflow, the bound b's sign points at
    function signed [31:0] sat_add;
        input signed [31:0] a, b;
        reg signed [32:0] t;
        begin
            t = {a[31], a} + {b[31], b};
            sat_add = (t[32] != t[31]) ? (b[31] ? $signed(32'h80000000) : 32'sh7FFFFFFF) : t[31:0];
        end
    endfunction
    function signed [31:0] sat_sub;
        input signed [31:0] a, b;
        reg signed [32:0] t;
        begin
            t = {a[31], a} - {b[31], b};
            sat_sub = (t[32] != t[31]) ? (b[31] ? 32'sh7FFFFFFF : $signed(32'h80000000)) : t[31:0];
        end
    endfunction
    wire signed [31:0] dn_signed = p_neg ? -$signed(dn_res) : $signed(dn_res);

    function [5:0] msb32;
        input [31:0] x;
        integer k;
        begin
            msb32 = 6'd0;
            for (k = 0; k < 32; k = k + 1)
                if (x[k]) msb32 = k[5:0];
        end
    endfunction




    // ---- control ----------------------------------------------------------
    localparam S_IDLE = 5'd0,
               S_B1 = 5'd1, S_B2 = 5'd2, S_B3 = 5'd3,           // 2-term blend
               S_C0 = 5'd7, S_C1 = 5'd8, S_C2 = 5'd9,
               S_R1 = 5'd11, S_R2 = 5'd12, S_R3 = 5'd10,         // RECIPN
               S_R4 = 5'd17, S_R5 = 5'd18, S_R6 = 5'd19,
               S_P1 = 5'd20, S_P2 = 5'd21, S_P3 = 5'd22, S_P4 = 5'd23,     // PROJECT
               S_P5 = 5'd24, S_P6 = 5'd25, S_P7 = 5'd26, S_P8 = 5'd27,
               S_P9 = 5'd28, S_P10 = 5'd29, S_PC = 5'd30, S_PN = 5'd31,
               S_P11 = 6'd32, S_P12 = 6'd33,                      // PROJECT, fm_r's extra cycle
               S_M1 = 5'd13, S_M2 = 5'd14,                        // fx_mul / div_norm
               S_SQ = 5'd15, S_ND = 5'd16,
               S_PU_W = 6'd34, S_PU_S = 6'd35,                    // PUSH
               S_AP2 = 6'd51,                                     // APPEND2's second word
               S_V1 = 6'd36,                                      // vertex load: y
               S_A1 = 6'd37, S_A2 = 6'd38, S_A3 = 6'd39,          // area + verdict
               S_S0 = 6'd40, S_S1 = 6'd41, S_S2 = 6'd42,          // SETUP: box, 1/area
               S_W0 = 6'd43, S_W1 = 6'd44,
               S_E0 = 6'd45, S_E1 = 6'd46, S_E2 = 6'd47,          // SETUP: per edge
               S_D0 = 6'd48, S_D1 = 6'd49,
               S_RD = 6'd50;                                      // 0x5A
    reg [5:0] st;

    // ---- output buffer (0x46/0x5A/0x60/0x61) -------------------------------
    // One write port (ob_we, registered) and one registered read port, so it
    // is a RAM block, not 2048 flip-flops.
    reg [31:0] ob [0:63];
    reg        ob_we;
    reg [5:0]  ob_wa, ob_ra;
    reg [31:0] ob_wd, ob_q;
    // read address: the op's rs2 in the cycle a command arrives (0x5A reads
    // out[rs2] one cycle later), PUSH's word pointer otherwise
    reg [6:0]  pu_n, pu_i;               // PUSH: words to send, words sent
    reg [5:0]  ap_ptr;                   // APPEND2 (0x63): next out[] to fill; PUSH clears it
    reg [31:0] ap_w2;
    // ... and while PUSH streams, the word after the one on out_data if that
    // one is taken this cycle, else the same again: a word per cycle
    wire [5:0] ob_raddr = (st == S_IDLE) ? in1[5:0]
                        : (st == S_PU_S) ? pu_i[5:0] + {5'd0, out_valid & out_ready} : ob_ra;
    always @(posedge clk) begin
        if (ob_we) ob[ob_wa] <= ob_wd;
        ob_q <= ob[ob_raddr];
    end
    assign out_data = ob_q;
`ifndef SU_NO_BLEND
    reg        dep;                      // the blend in flight deposits its results
    reg [5:0]  dep_v, dep_x, dep_y;
    always @(*) rq_in = (st == S_C0) ? sx : (st == S_C1) ? sy : sv;
`endif

    reg       mm_div;

    // ---- RECIPN's normalisation, one copy for its three users -------------
    // 0x50 (rs1), PROJECT (|w|, S_PN) and SETUP (the area, S_S0). An input
    // below 2^17 (only SETUP's small areas: fx_recip() shifts them up to
    // [2^17, 2^18) first) normalises to the same mantissa; msb is taken as 17.
    reg  [31:0] e_area;                  // SETUP: the culled area (> 0)
    wire [31:0] pn_ra = pw[31] ? 32'd0 - pw : pw;
    wire [31:0] rn_a  = (st == S_PN) ? pn_ra : (st == S_S0) ? e_area : in0;
    wire [5:0]  rn_m  = msb32(rn_a);
    wire [5:0]  rn_mc = (rn_m < 6'd17) ? 6'd17 : rn_m;
    wire [5:0]  rn_s  = (rn_mc > 6'd25) ? (rn_mc - 6'd9) : 6'd16;
    wire [31:0] rn_nm = rn_a << (6'd31 - rn_m);
    wire [3:0]  rn_sh = 4'd9 + rn_s[3:0] - rn_mc[3:0];   // 9+s-m is 0..8: low bits suffice
    wire [4:0]  rn_e  = rn_s[4:0] - 5'd16;

    // ---- edge setup (0x62, 0x64..0x67): geom_triangle.c's integer half ----
    // Edge units are screen pixels * 32 (EDGE_FUNC_SHIFT). to_edge_fixed(),
    // v / 2048 rounded half away from zero, is floor((v + 1023 + (v >= 0)) /
    // 2048): one adder with a carry-in and a shift. One converter: x in the
    // vertex op's own cycle, y in S_V1. |result| > 23170 is offscreen_guard()'s
    // reject, and every v whose exact conversion would not fit lands there.
    reg  [31:0] e_ry;
    wire [31:0] te_in = (st == S_V1) ? e_ry : in0;
    wire signed [32:0] te_s = $signed({te_in[31], te_in}) + 33'sd1023 + {32'd0, ~te_in[31]};
    wire signed [21:0] te_r = te_s[32:11];
    wire        te_g  = (te_r > 22'sd23170) || (te_r < -22'sd23170);
    wire signed [15:0] te_v = te_r[15:0];

    reg  signed [15:0] evx0, evy0, evx1, evy1, evx2, evy2;
    reg  signed [15:0] e_mnx, e_mxx, e_mny, e_mxy;      // running bounding box
    reg  [1:0]  e_k, e_j;                // vertex being loaded; edge being set up
    reg         e_g, e_cull, e_neg, e_small, eset, e_sw;
    reg  [1:0]  e_grow;                  // 0x62 rs2 = 5: crack grow level (below)
    reg  [4:0]  e_m;                     // msb(area)
    reg  [15:0] sc_ex, sc_ey;            // scissor end (0x62); its start is 0

    // The box, clamped to the scissor: combinational from the running
    // min/max, which hold still from the last vertex op to the next.
    wire signed [17:0] bsx = e_mnx + 18'sd16, bsy = e_mny + 18'sd16;
    wire signed [17:0] bex = e_mxx + 18'sd48, bey = e_mxy + 18'sd48;
    wire [15:0] bb_sx = bsx[17] ? 16'd0 : bsx[15:0];    // > 0 here: |v| <= 23170
    wire [15:0] bb_sy = bsy[17] ? 16'd0 : bsy[15:0];
    wire signed [17:0] bb_ex = (bex < $signed({2'b00, sc_ex})) ? bex : $signed({2'b00, sc_ex});
    wire signed [17:0] bb_ey = (bey < $signed({2'b00, sc_ey})) ? bey : $signed({2'b00, sc_ey});
    wire        bb_empty = ($signed({2'b00, bb_sx}) >= bb_ex) || ($signed({2'b00, bb_sy}) >= bb_ey);
    wire [15:0] anx = {bb_sx[15:5], 5'd0}, any = {bb_sy[15:5], 5'd0};   // anchor

    // The two edge multipliers, a fixed pipeline: a = v[ai], b = v[bi],
    // c = ecv ? v2 : anchor are chosen in cycle t; (by-ay), (ax-bx) and the
    // differences register at the end of t+1, the products
    // P = (cx-ax)*(by-ay), Q = (cy-ay)*(ax-bx) at the end of t+2, so
    // edge_fn() = P + Q in t+3. (A DSP pre-adder for cx-ax would need 19
    // bits and a 27x27 block per product: 3 more DSPs than this.) A negative sign
    // (no cull, clockwise) swaps a and b instead of negating anything:
    // edge_fn(b, a, c) == -edge_fn(a, b, c) in int32 as well.
    // Operands fit 18 bits: |v| <= 23170 after the guard, the anchor < 2^16.
    reg  [1:0]  ai, bi;
    reg         ecv;
    wire signed [15:0] ax = (ai == 2'd0) ? evx0 : (ai == 2'd1) ? evx1 : evx2;
    wire signed [15:0] ay = (ai == 2'd0) ? evy0 : (ai == 2'd1) ? evy1 : evy2;
    wire signed [15:0] bx = (bi == 2'd0) ? evx0 : (bi == 2'd1) ? evx1 : evx2;
    wire signed [15:0] by = (bi == 2'd0) ? evy0 : (bi == 2'd1) ? evy1 : evy2;
    wire signed [17:0] cx = ecv ? {{2{evx2[15]}}, evx2} : {2'b00, anx};
    wire signed [17:0] cy = ecv ? {{2{evy2[15]}}, evy2} : {2'b00, any};
    reg  signed [17:0] d_cxax, d_cyay, d_byay, d_axbx;
    reg  signed [35:0] ep, eq;
    always @(posedge clk) begin
        d_cxax <= cx - ax; d_cyay <= cy - ay; d_byay <= by - ay; d_axbx <= ax - bx;
        ep <= d_cxax * d_byay;
        eq <= d_cyay * d_axbx;
    end
    wire [31:0] ev_wi = ep[31:0] + eq[31:0];            // edge_fn(), int32 wrap
    wire [31:0] ev_wx = {{9{d_byay[17]}}, d_byay, 5'd0}; // (by - ay) * 32
    wire [31:0] ev_wy = {{9{d_axbx[17]}}, d_axbx, 5'd0}; // (ax - bx) * 32
    wire        ev_tl = d_byay[17] || (d_byay == 18'sd0 && d_axbx[17]);   // top-left rule
    // wInit's adjustment, registered in S_E1 (the differences hold the edge
    // through S_E2): minus the top-left bias and, with e_grow = L (1..3), plus
    // (|wXInc| + |wYInc|) >> (L + 1) = (16 >> L) (|by - ay| + |ax - bx|) --
    // the coverage grows up to 1/4, 1/8 or 1/16 px past each edge, so
    // triangles meeting at a T-junction overlap instead of leaving pixel
    // centres to neither (geom_pipeline.c, GEOM_CRACK_FIX). The weights still
    // come from the exact wInit, so pixels in the grown band extrapolate
    // colour and texture: half a pixel drew seams all over Mario; 1/16 closes
    // every WF crack on the host and leaves him clean.
    wire [17:0] ev_aby = d_byay[17] ? 18'd0 - d_byay : d_byay;
    wire [17:0] ev_abx = d_axbx[17] ? 18'd0 - d_axbx : d_axbx;
    wire [18:0] ev_abs = {1'b0, ev_aby} + {1'b0, ev_abx};
    // Only an edge the triangle is at least ~1 px thick across: 2 * area >=
    // 32 * (|dx| + |dy|) (edge units, 1/32 px; the L1 length makes it
    // conservative). A sliver's grown band extrapolates colour and depth far
    // outside it -- a grass/sand seam on the castle grounds drew a green line
    // across the screen, over Mario. Not grown, it covers only the pixel
    // centres it really contains, as on the N64.
    wire        ev_thick = e_area >= {8'd0, ev_abs, 5'd0};
    reg  [31:0] e_adj;
    always @(posedge clk)
        e_adj <= (!ev_thick ? 32'd0 :
                  e_grow == 2'd1 ? {10'd0, ev_abs, 3'd0} :
                  e_grow == 2'd2 ? {11'd0, ev_abs, 2'd0} :
                  e_grow == 2'd3 ? {12'd0, ev_abs, 1'd0} : 32'd0) - {31'd0, ev_tl};
    // per edge, lane 0 takes wXInc, wYInc, then wInit (S_E0..S_E2)
    wire [31:0] ev_iss = (st == S_E0) ? ev_wx : (st == S_E1) ? ev_wy : ev_wi;
    // lane 0's result for SETUP's weights: fx_div_norm or, small areas, fx_mul
    wire [31:0] e_res = e_small ? fm_res : dn_res;
    reg         dq1_v, dq2_v, dq3_v;     // a weight issued 1 / 2 / 3 cycles ago ...
    reg  [3:0]  dq1_a, dq2_a, dq3_a;     // ... and its W index
    reg  [31:0] wres_r;                  // lane 0's rounded result, registered for W
    reg         e_empty;                 // bb_empty, registered in S_S0

    always @(posedge clk) begin : ctl
        reg [31:0] nm;
        reg [31:0] ra;
        reg [22:0] r0;
        reg signed [63:0] eps;
        reg signed [31:0] r1;
        reg [5:0]  m;
        reg [5:0]  s;
        reg [25:0] r;
        reg [24:0] q;
        done <= 1'b0;
        ob_we <= 1'b0;
        // lane 0's product -> rounding -> W was too long for one cycle:
        // the result registers first (wres_r), W is written the cycle after
        dq1_v <= 1'b0; dq2_v <= dq1_v; dq2_a <= dq1_a; dq3_v <= dq2_v; dq3_a <= dq2_a;
        wres_r <= e_res;
`ifdef SU_WLOAD
        w_we <= 1'b0;
        if (dq3_v) begin w_we <= 1'b1; w_wa <= dq3_a; w_wd <= wres_r; end
`elsif SU_NO_BLEND
`else
        if (dq3_v)
            case (dq3_a)
                4'd0: w[0] <= sat27(wres_r); 4'd1: w[1] <= sat27(wres_r); 4'd2: w[2] <= sat27(wres_r);
                4'd3: w[3] <= sat27(wres_r); 4'd4: w[4] <= sat27(wres_r); default: w[5] <= sat27(wres_r);
            endcase
`endif
        if (!resetn) begin
            st <= S_IDLE;
            out_valid <= 1'b0;
            ap_ptr <= 6'd0;
`ifndef SU_NO_BLEND
            dep <= 1'b0;
`endif
            eset <= 1'b0;
            dq1_v <= 1'b0; dq2_v <= 1'b0; dq3_v <= 1'b0;
            bg <= 1'b0;
        end else begin
            case (st)
                S_IDLE: if (start) begin
                    case (fid)
`ifdef SU_WLOAD
                        10'h04: begin
                            // written next cycle: the earliest reader (a
                            // blend's S_B1) is two ops' handshakes away
                            w_we <= 1'b1; w_wa <= in1[3:0]; w_wd <= in0;
                            result <= 32'd0; done <= 1'b1;
                        end
`endif
`ifndef SU_NO_BLEND
                        10'h40: begin a1r <= in0; a2r <= in1; result <= 32'd0; done <= 1'b1; end
                        10'h41, 10'h46: begin
                            // d1 terms go into the multipliers now, d2 next cycle
                            dep <= fid[2]; dep_v <= in1[13:8]; dep_x <= in1[21:16]; dep_y <= in1[29:24];
                            a0r <= in0;
                            d2r <= bd2;
                            mx <= {{13{bd1[19]}}, bd1}; wsel <= 2'd0; rsh <= in1[3:0];
                            st  <= S_B1;
                        end
`endif

                        10'h60: begin
                            ob_we <= 1'b1; ob_wa <= in1[5:0]; ob_wd <= in0;
                            result <= 32'd0; done <= 1'b1;
                        end
                        10'h63: begin             // APPEND2: out[ap_ptr], out[ap_ptr+1] = rs1, rs2
                            ob_we <= 1'b1; ob_wa <= ap_ptr; ob_wd <= in0;
                            ap_w2 <= in1;
                            st <= S_AP2;
                        end
                        10'h61: begin
                            if (in0[6:0] == 7'd0) begin result <= 32'd0; done <= 1'b1; end
                            else begin
                                pu_n <= in0[6:0]; pu_i <= 7'd0; ob_ra <= 6'd0;
                                st <= S_PU_W;
                            end
                        end
`ifndef SU_NO_EDGE
                        10'h62: begin
                            case (in1[2:0])
                                3'd2: sc_ex <= in0[15:0];
                                3'd3: sc_ey <= in0[15:0];
                                3'd4: e_cull <= in0[0];
                                3'd5: e_grow <= in0[1:0];
                                default: ;                      // start x, y: always 0
                            endcase
                            result <= 32'd0; done <= 1'b1;
                        end
                        10'h64, 10'h65, 10'h66: begin
                            e_k <= fid[1:0];
                            case (fid[1:0])
                                2'd0: evx0 <= te_v; 2'd1: evx1 <= te_v; default: evx2 <= te_v;
                            endcase
                            if (fid[1:0] == 2'd0) begin
                                e_mnx <= te_v; e_mxx <= te_v; e_g <= te_g;
                            end else begin
                                if (te_v < e_mnx) e_mnx <= te_v;
                                if (te_v > e_mxx) e_mxx <= te_v;
                                e_g <= e_g | te_g;
                            end
                            e_ry <= in1;
                            st <= S_V1;
                        end
                        10'h67: st <= S_S0;
`endif
                        10'h5A: st <= S_RD;             // out[rs2] (ob_raddr)
                        10'h58: begin
                            case (in1[2:0])
                                3'd0: pv_sx <= in0; 3'd1: pv_tx <= in0;
                                3'd2: pv_sy <= in0; 3'd3: pv_ty <= in0;
                                default: begin
                                    pv_sh0 <= in0[2:0];  pv_sh1 <= in0[6:4];
                                    pv_sh2 <= in0[10:8]; pv_sh3 <= in0[14:12];
                                end
                            endcase
                            result <= 32'd0; done <= 1'b1;
                        end
                        10'h59: begin pj <= 2'd0; proj <= 1'b1; st <= S_PC; end
                        10'h50: begin
                            proj <= 1'b0;
                            rc_m <= rn_nm; rc_idx <= rn_nm[30:22]; rc_sh <= rn_sh; rc_e <= rn_e;
                            st     <= S_R1;
                        end
                        10'h51: begin result <= {27'd0, rc_e}; done <= 1'b1; end

                        10'h52: begin
                            mm_div <= 1'b0;
                            mx <= sx33(in0); mm_b <= sx33(in1); wsel <= 2'd3;
                            st  <= S_M1;
                        end
                        10'h53: begin
                            mm_div <= 1'b1;
                            mx <= sx33(in0); mm_b <= {1'b0, in1}; wsel <= 2'd3;   // k.r explicit
                            st  <= S_M1;
                        end
                        10'h57: begin rc_e <= in0[4:0]; result <= 32'd0; done <= 1'b1; end

                        10'h54: begin
                            if (in0[15:0] == 16'd0) begin result <= 32'd0; done <= 1'b1; end
                            else begin
                                m = msb32({16'd0, in0[15:0]});
                                nm = {16'd0, in0[15:0]} << (6'd15 - m);
                                nf_idx <= {m[0], nm[14:7]};
                                nf_sh  <= 4'sd4 - $signed({1'b0, m[3:1]});
                                st     <= S_SQ;
                            end
                        end



                        default: begin result <= 32'd0; done <= 1'b1; end
                    endcase
                end

`ifndef SU_NO_BLEND
                // ---- 2-term blend: d1 products land in S_B2, d2 in S_B3 ----
                S_B1: begin
                    mx <= {{13{d2r[19]}}, d2r}; wsel <= 2'd1;
                    st  <= S_B2;
                end
                S_B2: begin
                    sv <= ($signed({{17{a0r[31]}}, a0r}) <<< 16) + p0[48:0];
                    sx <= p1;
                    sy <= p2;
                    st <= S_B3;
                end
                S_B3: begin
                    sv <= sv + p0[48:0]; sx <= sx + p1; sy <= sy + p2;
                    st <= S_C0;
                end

                // the rounder sees rq_in = sv, then sx, then sy
                // X, Y, then V, which is also the result: no holding registers
                S_C0: begin ob_we <= dep; ob_wa <= dep_x; ob_wd <= rq_out; st <= S_C1; end
                S_C1: begin ob_we <= dep; ob_wa <= dep_y; ob_wd <= rq_out; st <= S_C2; end
                S_C2: begin
                    ob_we <= dep; ob_wa <= dep_v; ob_wd <= rq_out;
                    result <= rq_out; done <= 1'b1; st <= S_IDLE;
                end
`endif

                // ---- PUSH: ob_ra was set a cycle ago, ob_q lands this edge ----
                S_AP2: begin
                    ob_we <= 1'b1; ob_wa <= ap_ptr + 6'd1; ob_wd <= ap_w2;
                    ap_ptr <= ap_ptr + 6'd2;
                    result <= 32'd0; done <= 1'b1; st <= S_IDLE;
                end
                S_PU_W: begin out_valid <= 1'b1; st <= S_PU_S; end
                S_PU_S: if (out_ready) begin
                    if (pu_i + 7'd1 == pu_n) begin
                        out_valid <= 1'b0;
                        ap_ptr <= 6'd0;
                        result <= 32'd0; done <= 1'b1; st <= S_IDLE;
                    end else begin
                        pu_i <= pu_i + 7'd1;           // its word lands in ob_q this edge
                    end
                end

                // ---- fx_mul / fx_div_norm on lane 0 ----
                S_RD: begin result <= ob_q; done <= 1'b1; st <= S_IDLE; end
                S_M1: st <= S_M2;                   // product registers this edge
                S_M2: begin
                    result <= mm_div ? dn_res : fm_res;
                    done <= 1'b1; st <= S_IDLE;
                end

                // ---- PROJECT: the four column shifts, then 1/|w| as 0x50 does
                // -- only where the C takes fx_recip_norm() (|w| >= 2^17) ----
                S_PC: begin
                    if (pj == 2'd3) pw <= pcol;
                    else begin ob_we <= 1'b1; ob_wa <= 6'd59 + {4'd0, pj}; ob_wd <= pcol; end
                    pj <= pj + 2'd1;                    // back to 0 for S_P1
                    if (pj == 2'd3) st <= S_PN;
                end
                // w's normalisation from the registered pw: in S_PC the column
                // mux + saturation + abs + msb + shift did not close timing
                S_PN: begin
                    p_neg <= pw[31];
                    if (pn_ra < 32'h0002_0000) begin
                        result <= pw; done <= 1'b1; proj <= 1'b0;   // |w| < 2^17: the C's case
                        st <= S_IDLE;
                    end else begin
                        rc_m <= rn_nm; rc_idx <= rn_nm[30:22]; rc_sh <= rn_sh; rc_e <= rn_e;
                        // w is the answer; out[56..58] follow in the background
                        result <= pw; done <= 1'b1; bg <= 1'b1;
                        st     <= S_R1;
                    end
                end

                // ---- RECIPN: seed, then r1 = r0 + r0*(2^46 - m24*r0) / 2^46 ----
                S_R1: st <= S_R2;                   // rc_seed registers this edge
                S_R2: begin
                    r0 = {1'b1, rc_seed, 6'd0};
                    rc_r0 <= {9'd0, r0};
                    mx <= {9'd0, rc_m[31:8]}; mm_b <= {10'd0, r0}; wsel <= 2'd3;
                    st <= S_R3;
                end
                S_R3: st <= S_R4;                   // m24 * r0 registers this edge
                S_R4: begin
                    eps = $signed(64'h0000_4000_0000_0000) - $signed(p0[63:0]);
                    mx <= {rc_r0[31], rc_r0}; mm_b <= eps[42:10]; wsel <= 2'd3;   // |eps >> 10| < 2^27
                    st <= S_R5;
                end
                S_R5: st <= S_R6;                   // r0 * (eps >> 10) registers this edge
                S_R6: begin
                    r1 = rc_r0 + $signed(p0[63:36]);            // (r0 * eps10) >> 36, |.| < 2^14
                    rc_res <= $unsigned(r1) << rc_sh;
                    if (proj) st <= S_P1;
                    else
                    if (eset) begin eset <= 1'b0; st <= S_S2; end
                    else begin
                        result <= $unsigned(r1) << rc_sh;
                        done <= 1'b1;
                        st <= S_IDLE;
                    end
                end

                // ---- PROJECT: three divides, then viewport and depth, one
                // lane-0 product issued per cycle (each lands two states on)
                S_P1: begin mx <= sx33(pcol); pj <= 2'd1; mm_b <= {1'b0, rc_res}; wsel <= 2'd3; st <= S_P2; end
                S_P2: begin mx <= sx33(pcol); pj <= 2'd2; st <= S_P3; end
                // each quotient waits one cycle in pq for its viewport product
                S_P3: begin mx <= sx33(pcol); pq <= dn_signed; st <= S_P4; end
                S_P4: begin mx <= sx33(pq); pq <= dn_signed; mm_b <= sx33(pv_sx); st <= S_P5; end
                S_P5: begin mx <= sx33(pq); pq <= dn_signed; mm_b <= sx33(pv_sy); st <= S_P6; end
                // products issued in P4, P5, P6 are read from fm_r three
                // states later (P7, P8, P9); P9's issue lands in P12
                S_P6: begin mx <= sx33(pq); mm_b <= sx33(32'sd32768); st <= S_P7; end
                S_P7: begin ob_we <= 1'b1; ob_wa <= 6'd56; ob_wd <= sat_add($signed(fm_r), pv_tx); st <= S_P8; end
                S_P8: begin ob_we <= 1'b1; ob_wa <= 6'd57; ob_wd <= sat_sub(pv_ty, $signed(fm_r)); st <= S_P9; end
                S_P9: begin
                    mx <= sx33(sat_add($signed(fm_r), 32'sd32768)); mm_b <= sx33(32'sd65534);
                    st <= S_P10;
                end
                S_P10: st <= S_P11;
                S_P11: st <= S_P12;
                S_P12: begin
                    ob_we <= 1'b1; ob_wa <= 6'd58; ob_wd <= fm_r;
                    bg <= 1'b0; proj <= 1'b0;
                    st <= S_IDLE;
                end

                // ---- NORMF: the seed registers this edge, then the shift ----
                S_SQ: st <= S_ND;
                S_ND: begin
                    result <= nf_sh[3] ? ({16'd0, nf_seed} >> (-nf_sh)) : ({16'd0, nf_seed} << nf_sh);
                    done <= 1'b1; st <= S_IDLE;
                end

`ifndef SU_NO_EDGE
                // ---- vertex load, y; after vertex 2 the area ----
                S_V1: begin
                    case (e_k)
                        2'd0: evy0 <= te_v; 2'd1: evy1 <= te_v; default: evy2 <= te_v;
                    endcase
                    if (e_k == 2'd0) begin e_mny <= te_v; e_mxy <= te_v; end
                    else begin
                        if (te_v < e_mny) e_mny <= te_v;
                        if (te_v > e_mxy) e_mxy <= te_v;
                    end
                    e_g <= e_g | te_g;
                    if (e_k == 2'd2) begin
                        ai <= 2'd0; bi <= 2'd1; ecv <= 1'b1; e_sw <= 1'b0; st <= S_A1;
                    end else begin result <= 32'd0; done <= 1'b1; st <= S_IDLE; end
                end
                S_A1: st <= S_A2;                   // operands register
                S_A2: st <= S_A3;                   // products register
                // edges_of(): sign = cull ? +1 : (area <= 0 ? -1 : 1), and
                // area * sign -- for -1, edge_fn(v1, v0, v2) (a second pass)
                S_A3: begin
                    if (!e_sw && !e_cull && $signed(ev_wi) <= 0) begin
                        e_sw <= 1'b1; ai <= 2'd1; bi <= 2'd0; st <= S_A1;
                    end else begin
                        e_neg <= e_sw; e_area <= ev_wi;
                        result <= {31'd0, e_g | ($signed(ev_wi) <= 0)};
                        done <= 1'b1; st <= S_IDLE;
                    end
                end

                // ---- SETUP: box, then 1/area on RECIPN's states ----
                S_S0: begin
                    rc_m <= rn_nm; rc_idx <= rn_nm[30:22]; rc_sh <= rn_sh; rc_e <= rn_e;
                    e_small <= e_area < 32'h0002_0000;
                    e_m <= rn_m[4:0];
                    e_empty <= bb_empty;
                    ob_we <= 1'b1; ob_wa <= 6'd4;   // written even if empty: harmless
                    ob_wd <= {5'd0, bb_sy[15:5], 5'd0, bb_sx[15:5]};
                    st <= S_S1;
                end
                S_S1: begin
                    if (e_empty) begin
                        result <= 32'd0; done <= 1'b1; st <= S_IDLE;   // fully off-screen
                    end else begin
                        ob_we <= 1'b1; ob_wa <= 6'd5;
                        ob_wd <= {5'd0, bb_ey[15:5], 5'd0, bb_ex[15:5]};
                        eset <= 1'b1; proj <= 1'b0;
                        st <= S_R1;
                    end
                end
                // lane 0's second operand for the whole edge loop: k.r, or
                // for area < 2^17 fx_recip(area) = RECIPN of area << k (e = 0),
                // rounded r >> (msb - 1). That rounding is fx_div_norm(2^31, r)
                // with e = msb - 2 (msb 2 on); msb 1 has no shift, area 1
                // saturates.
                S_S2: begin
                    wsel <= 2'd3;
                    e_j <= 2'd0;
                    if (e_neg) begin ai <= 2'd2; bi <= 2'd1; end    // edge 0: v1 -> v2, swapped
                    else       begin ai <= 2'd1; bi <= 2'd2; end
                    ecv <= 1'b0;
                    if (!e_small)          mm_b <= {1'b0, rc_res};
                    else if (e_m == 5'd0)  mm_b <= {1'b0, 32'h7FFFFFFF};
                    else if (e_m == 5'd1)  mm_b <= {1'b0, rc_res[31] ? 32'h7FFFFFFF : rc_res};
                    else begin
                        mx <= {2'b01, 31'd0}; mm_b <= {1'b0, rc_res}; rc_e <= e_m - 5'd2;
                    end
                    st <= S_W0;
                end
                S_W0: st <= S_W1;
                S_W1: begin
                    if (e_small && e_m > 5'd1) mm_b <= {1'b0, dn_res};
                    st <= S_E0;
                end
                // ---- per edge j: wXInc, wYInc, wInit issued to lane 0 in
                // turn (their weights land in W two cycles later, dq1/dq2) and
                // written to the output buffer, wInit with the top-left bias.
                // The next edge's operands are chosen in S_E1: the differences
                // still hold this edge's through S_E2, the products too.
                S_E0: begin
                    mx <= sx33(ev_iss);
                    ob_we <= 1'b1; ob_wa <= 6'd9 + {4'd0, e_j}; ob_wd <= ev_iss;
                    dq1_v <= e_j != 2'd0; dq1_a <= (e_j == 2'd1) ? 4'd2 : 4'd3;
                    st <= S_E1;
                end
                S_E1: begin
                    mx <= sx33(ev_iss);
                    ob_we <= 1'b1; ob_wa <= 6'd12 + {4'd0, e_j}; ob_wd <= ev_iss;
                    dq1_v <= e_j != 2'd0; dq1_a <= (e_j == 2'd1) ? 4'd4 : 4'd5;
                    // next: edge 1 = v2 -> v0, edge 2 = v0 -> v1 (swapped if e_neg)
                    if (e_j == 2'd0) begin ai <= e_neg ? 2'd0 : 2'd2; bi <= e_neg ? 2'd2 : 2'd0; end
                    else             begin ai <= e_neg ? 2'd1 : 2'd0; bi <= e_neg ? 2'd0 : 2'd1; end
                    st <= S_E2;
                end
                S_E2: begin
                    mx <= sx33(ev_iss);
                    ob_we <= 1'b1; ob_wa <= 6'd6 + {4'd0, e_j}; ob_wd <= ev_wi + e_adj;
                    dq1_v <= e_j != 2'd0; dq1_a <= (e_j == 2'd1) ? 4'd0 : 4'd1;
                    e_j <= e_j + 2'd1;
                    st <= (e_j == 2'd2) ? S_D0 : S_E0;
                end
                S_D0: st <= S_D1;
                // the last W lands two cycles after this (wres_r, then W); the
                // earliest reader, a blend's S_B1, is further than that away
                S_D1: begin result <= 32'd1; done <= 1'b1; st <= S_IDLE; end
`endif

                default: st <= S_IDLE;
            endcase
        end
    end
endmodule
