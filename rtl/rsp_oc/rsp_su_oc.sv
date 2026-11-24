// rsp_su_oc: the RSP's scalar unit. The N64 RSP's integer instruction set (MIPS
// without multiply, divide, exceptions or 64-bit operations; add/addi/sub as
// their unsigned forms) with a 12-bit PC, on mips_lite's five-stage pipeline:
//   F  the PC goes to IMEM (a synchronous memory: the word comes back the
//      next cycle)
//   D  decode; the register file is read with the arriving instruction
//   E  forwarding, the ALU, branches and jumps (the delay slot is in D and
//      runs; what is being fetched is squashed if taken), the memory access
//      (rsp_top_oc's DMEM: any byte address, 1 to 4 bytes, in the cycle), the
//      coprocessor instructions -- COP0, COP2, LWC2, SWC2 -- which go out
//      on c_* with rs and rt and hold E until c_done
//   M  load data
//   W  register write
// BREAK, as it leaves E, stops the unit (brk): what follows it is not executed
// and pc_next names it.
// A load followed by an instruction that reads its value stalls a cycle (and
// one after CFC2, whose value comes in M too); every other value is
// forwarded. Pairs, as many as the console makes and with no rule of its
// timing beyond what the results need: a vector computation and the
// scalar-unit instruction next to it, either first -- one that is not a
// branch, jump or BREAK (those only second), with no coprocessor's register
// the first writes and the second uses (the console's rule, the lane field of
// a one-lane op or VNOP taken for a register as on the console), not in a
// delay slot -- enter E together (IMEM's second port gives the word after
// D's, at any word) and leave it in the same cycle, the scalar one through E
// as ever and the computation to the vector unit on c_*
// (c_pr). The fetch then goes on two words on. The scalar one may be an ALU
// op, a scalar load or store, a branch, MFC0 / MTC0, and with LPAIR (a vector
// unit of eight lanes) a vector load (LBV .. LUV) or store (SBV .. SUV, SHV,
// SFV), MFC2, MTC2, CFC2 or CTC2, which goes to the vector unit with it
// (c_lins, a load's or store's base on c_a). CFC2's value is taken in M (the
// flags of every computation before it are in by then).
`default_nettype none

// A 32-bit compare as three LUT levels: the difference of each three bits (dg, kept: the synthesis, in area mode, would fold them into a
// deeper tree), whether none of the first six differs (h1) and none of the other five (h2); a and b are equal when both are
module rsp_eq32h (
    input  wire [31:0] a,
    input  wire [31:0] b,
    output wire        h1,
    output wire        h2
);
(* keep *) wire [10:0] dg;
wire [32:0] d = {1'b0, a} ^ {1'b0, b};
genvar g;
generate for (g = 0; g < 11; g++) begin : grp
    assign dg[g] = |d[3 * g +: 3];
end endgenerate
assign h1 = ~|dg[5:0];
assign h2 = ~|dg[10:6];
endmodule

module rsp_su_oc #(
    parameter bit LPAIR = 0
) (
    input  wire         clk,
    input  wire         rst,
    // run: execute. While it is low nothing enters E; pc_load sets the PC.
    input  wire         run,
    input  wire         pc_load,
    input  wire  [11:0] pc_in,
    output logic [11:0] pc_next,        // the next instruction to run (valid once stopped)
    output logic        brk,            // BREAK leaves E this cycle
    output logic        idle,           // nothing in E, M (the unit has stopped)

    output logic [9:0]  i_addr,         // IMEM word address
    output logic [9:0]  i_addr1,        // ... and the next word's (IMEM's second port)
    input  wire  [31:0] i_data,
    input  wire  [31:0] i_data2,        // the word after i_data's (IMEM's second port) ...
    input  wire         i2_ok,          // ... when that port read it (not the DMA's)
    input  wire         p2_ok,          // ... and the two words can be a pair (by the rule alone: pw_ok of the two when IMEM was written)
    input  wire  [31:0] pw_a,           // two words, for pw_ok
    input  wire  [31:0] pw_b,
    output logic        pw_ok,          // pw_a, pw_b can be a pair (the rule, not the rest of what it takes)

    // DMEM: bytes m_addr .. m_addr + m_n, first byte in [31:24]; the read
    // data the next cycle
    output logic        m_valid,
    output logic        m_we,
    output logic [11:0] m_addr,
    output logic [11:0] m_addr16,       // ... and the address + 16 (the next line's: made in parallel)
    // ... and each of them in two parts, the sources that are not the register file's (m_addr_e, m_addr16_e: ORed, a zero where the file is the
    // source) and the file's (m_addr_r, m_addr16_r, when m_sel_r), for rsp_top_oc's choice among the units' addresses
    output logic [11:0] m_addr_e,
    output logic [11:0] m_addr16_e,
    output logic [11:0] m_addr_r,
    output logic [11:0] m_addr16_r,
    output logic        m_sel_r,
    output logic [1:0]  m_n,            // bytes - 1 (0, 1 or 3)
    output logic [31:0] m_wdata,
    input  wire  [31:0] m_rdata,
    input  wire         m_stall,        // DMEM's port is not for a load this cycle (the DMA's, or a store's write): a load waits (E holds)
    input  wire         m_stall_st,     // ... for a store (the DMA's): a store waits

    // the coprocessors
    output logic        c_valid,
    output logic [31:0] c_ins,
    output logic [31:0] c_a,            // rs
    // ... and its low 4 bits from each source -- the writer in M, the one in W, the register file's (c_bm, c_bw, c_br) -- with the choice (c_fm: M, c_fw: W,
    // else the file's), for a vector store's rotation; and the vector unit's load and store addresses, the scalar unit's own adders' sums with the
    // vector offset (c_om: the base from M, c_ow, c_or, and c_hm, c_hw, c_hr with 16 more: the next line's), and that offset's low 4 bits (c_o4)
    output logic [3:0]  c_bm,
    output logic [3:0]  c_bw,
    output logic [3:0]  c_br,
    output logic [11:0] c_om,
    output logic [11:0] c_ow,
    output logic [11:0] c_or,
    output logic [11:0] c_hm,
    output logic [11:0] c_hw,
    output logic [11:0] c_hr,
    output logic [3:0]  c_o4,
    output logic        c_fm,
    output logic        c_fw,
    output logic [31:0] c_b,            // rt
    output logic        c_pr,           // E holds a pair: the computation on c_ins, the scalar unit's own on c_lins
    output logic [31:0] c_lins,         // E's own instruction (c_ins but for a pair's computation)
    input  wire         c_done,
    input  wire  [31:0] c_res,          // MFC0, MFC2
    input  wire  [31:0] c_res_l,        // CFC2's, the cycle after it leaves E
    // the vector registers' read addresses (their memories register them): of the slot E holds in the cycle to come,
    // the computation's vs and vt (c_ra_n, c_rb_n), and the register the scalar unit's own instruction reads or writes
    // -- rd for MFC2 / MTC2, rt for a load or store (c_rd_n); c_rd is that of the one in E now
    output logic [4:0]  c_ra_n,
    output logic [4:0]  c_rb_n,
    output logic [4:0]  c_rd_n,
    output logic [4:0]  c_rd,
    // the instructions entering E, for the vector unit's decode: the coprocessors', the scalar unit's own, and that E takes them
    output logic [31:0] c_ins_n,
    output logic [31:0] c_lins_n,
    output logic        c_adv,
    output logic        c_vls,          // E's instruction is a vector load or store (LWC2, SWC2): registered

    // (simulation) each instruction as it leaves E, and each register write
    output logic        tr_e,
    output logic [11:0] tr_e_pc,        // (of a pair: the first)
    output logic        tr_e2,          // a pair: the next instruction (tr_e_pc + 4) leaves with it
    output logic        tr_e_mem,       // E holds a load or store (scalar or vector): DMEM's port held by the DMA keeps it there
    output logic        tr_e_we,        // ... and it writes register tr_e_wr
    output logic [4:0]  tr_e_wr,
    output logic        tr_we,
    output logic [4:0]  tr_wr,
    output logic [31:0] tr_val
);

typedef enum logic [3:0] {
    A_ADD, A_SUB, A_AND, A_OR, A_XOR, A_NOR, A_SLT, A_SLTU, A_SLL, A_SRL, A_SRA, A_LUI
} alu_t;

typedef struct packed {
    logic       we;
    logic [4:0] wr;
    logic       b_imm, imm_zx, sh_imm;
    alu_t       alu;
    logic       load, store;
    logic [1:0] size;        // bytes - 1
    logic       lu;
    logic       br;
    logic [2:0] bcond;       // 0 beq 1 bne 2 blez 3 bgtz 4 bltz 5 bgez
    logic       j, jr, link;
    logic       cop, brk;
    logic       cf;          // CFC2: its value in M
} ctl_t;

function automatic ctl_t decode(input logic [31:0] i);
    ctl_t c = '0;
    logic [5:0] op = i[31:26], fn = i[5:0];
    logic [4:0] rs = i[25:21], rt = i[20:16], rd = i[15:11];
    c.alu = A_ADD;
    c.bcond = 3'd7;           // (no branch: no condition holds)
    unique case (op)
    6'h00: begin
        c.wr = rd; c.we = 1'b1;
        unique case (fn)
        6'h00: begin c.alu = A_SLL; c.sh_imm = 1'b1; end
        6'h02: begin c.alu = A_SRL; c.sh_imm = 1'b1; end
        6'h03: begin c.alu = A_SRA; c.sh_imm = 1'b1; end
        6'h04: c.alu = A_SLL;
        6'h06: c.alu = A_SRL;
        6'h07: c.alu = A_SRA;
        6'h08: begin c.we = 1'b0; c.jr = 1'b1; end
        6'h09: begin c.jr = 1'b1; c.link = 1'b1; end
        6'h0D: begin c.we = 1'b0; c.brk = 1'b1; end
        6'h20, 6'h21: c.alu = A_ADD;
        6'h22, 6'h23: c.alu = A_SUB;
        6'h24: c.alu = A_AND;
        6'h25: c.alu = A_OR;
        6'h26: c.alu = A_XOR;
        6'h27: c.alu = A_NOR;
        6'h2A: c.alu = A_SLT;
        6'h2B: c.alu = A_SLTU;
        default: c.we = 1'b0;
        endcase
    end
    6'h01: begin
        c.br = rt[3:1] == 3'd0;
        c.bcond = rt[0] ? 3'd5 : 3'd4;
        if (rt[3:1] == 3'd0 && rt[4]) begin c.link = 1'b1; c.we = 1'b1; c.wr = 5'd31; end
    end
    6'h02: c.j = 1'b1;
    6'h03: begin c.j = 1'b1; c.link = 1'b1; c.we = 1'b1; c.wr = 5'd31; end
    6'h04: begin c.br = 1'b1; c.bcond = 3'd0; end
    6'h05: begin c.br = 1'b1; c.bcond = 3'd1; end
    6'h06: begin c.br = 1'b1; c.bcond = 3'd2; end
    6'h07: begin c.br = 1'b1; c.bcond = 3'd3; end
    6'h08, 6'h09: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_ADD;  end
    6'h0A:        begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_SLT;  end
    6'h0B:        begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_SLTU; end
    6'h0C: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_AND; end
    6'h0D: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_OR;  end
    6'h0E: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_XOR; end
    6'h0F: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_LUI; end
    6'h10: begin c.cop = rs == 5'd0 || rs == 5'd4; c.we = rs == 5'd0; c.wr = rt; end          // MFC0, MTC0
    6'h12: begin c.cop = 1'b1; c.we = !i[25] && (rs == 5'd0 || rs == 5'd2); c.wr = rt; c.cf = !i[25] && (rs == 5'd0 || rs == 5'd2); end   // MFC2, CFC2 write rt, their value in M
    6'h32, 6'h3A: c.cop = 1'b1;                                                            // LWC2, SWC2
    6'h20: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd0; end
    6'h21: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd1; end
    6'h23, 6'h27: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd3; end
    6'h24: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd0; c.lu = 1'b1; end
    6'h25: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd1; c.lu = 1'b1; end
    6'h28: begin c.store = 1'b1; c.size = 2'd0; end
    6'h29: begin c.store = 1'b1; c.size = 2'd1; end
    6'h2B: begin c.store = 1'b1; c.size = 2'd3; end
    default: ;
    endcase
    if (c.wr == 5'd0) c.we = 1'b0;
    return c;
endfunction

logic [11:0] pc_f;
logic        d_valid /* verilator public_flat_rd */;  logic [11:0] d_pc /* verilator public_flat_rd */;
logic        e_valid /* verilator public_flat_rd */;  logic [11:0] e_pc;  logic [31:0] e_ins;  ctl_t e_c;
logic [11:0] e_spc;                                         // (a pair: the scalar unit's instruction's own, for a branch's)
logic        v_e;      logic [31:0] v_ins;                  // E holds a pair: its vector computation
logic        m_valid_r; logic [31:0] m_res; ctl_t m_c;
logic        m_we_r;
logic        w_we;     logic [4:0] w_wr;   logic [31:0] w_res;

// ---- what an instruction reads and writes, for the console's issue rules (sim/rsp/rsp_time.c has them in C)
function automatic logic is_vc(input logic [31:0] i);
    return i[31:26] == 6'h12 && i[25];
endfunction
function automatic logic is_br(input logic [31:0] i);       // a branch, jump or BREAK
    logic [5:0] op; op = i[31:26];
    return (op[5:3] == 3'd0 && op[2:0] != 3'd0) || (op == 6'h00 && (i[5:0] == 6'h08 || i[5:0] == 6'h09 || i[5:0] == 6'h0D));
endfunction
function automatic logic is_plain(input logic [31:0] i);     // neither a coprocessor's nor a branch, jump or BREAK
    logic [5:0] op; op = i[31:26];
    return !is_br(i) && op != 6'h10 && op != 6'h12 && op != 6'h32 && op != 6'h3A;
endfunction
function automatic logic is_c0(input logic [31:0] i);       // MFC0, MTC0
    return i[31:26] == 6'h10 && (i[25:21] == 5'd0 || i[25:21] == 5'd4);
endfunction
// a computation's vector registers: {reads vs, reads vt, writes vd}
function automatic logic [2:0] c_use(input logic [31:0] i);
    logic [5:0] fn; fn = i[5:0];
    if (fn <= 6'h0F) return fn == 6'h02 || fn == 6'h0A ? 3'b011 : fn == 6'h0B ? 3'b001 : 3'b111;
    if (fn == 6'h10 || fn == 6'h11 || (fn >= 6'h13 && fn <= 6'h15) || (fn >= 6'h20 && fn <= 6'h2D)) return 3'b111;
    if (fn == 6'h1D) return 3'b001;
    if (fn >= 6'h30 && fn <= 6'h36) return 3'b011;
    return 3'b000;
endfunction
function automatic logic c_resv(input logic [31:0] i);      // a reserved function
    logic [5:0] f; f = i[5:0];
    return f == 6'h12 || (f >= 6'h16 && f <= 6'h1C) || f == 6'h1E || f == 6'h1F || f == 6'h2E || f == 6'h2F || (f >= 6'h38 && f <= 6'h3E);
endfunction
// the scalar unit's register of the vector unit's: {reads it, writes it, the eight of its group} and which -- a store's
// or MFC2's (read), a load's or MTC2's (written)
function automatic logic [7:0] x_vreg(input logic [31:0] i);
    logic [5:0] op; logic [4:0] rd; op = i[31:26]; rd = i[15:11];
    if (op == 6'h3A && rd <= 5'd11) return {1'b1, 1'b0, rd == 5'd11, i[20:16]};
    if (op == 6'h32 && (rd <= 5'd9 || rd == 5'd11)) return {1'b0, 1'b1, rd == 5'd11, i[20:16]};
    if (op == 6'h12 && !i[25] && i[25:21] == 5'd0) return {1'b1, 1'b0, 1'b0, rd};
    if (op == 6'h12 && !i[25] && i[25:21] == 5'd4) return {1'b0, 1'b1, 1'b0, rd};
    return 8'd0;
endfunction
// (LPAIR) the vector unit's instructions of the scalar unit that pair: LBV .. LUV, SBV .. SUV, SHV, SFV (an element
// a multiple of 4), MFC2, MTC2, CFC2, CTC2
function automatic logic x_ok(input logic [31:0] i);
    logic [4:0] rd; rd = i[15:11];
    return LPAIR && ((i[31:26] == 6'h32 && rd <= 5'd7) || (i[31:26] == 6'h3A && (rd <= 5'd8 || (rd == 5'd9 && i[8:7] == 2'd0)))
                     || (i[31:26] == 6'h12 && !i[25] && !i[24] && !i[21]));
endfunction
// the control registers (VCO, VCC, VCE) a computation uses -- reads and writes, for the pairing rule
function automatic logic [2:0] c_flags(input logic [31:0] i);
    logic [5:0] fn; fn = i[5:0];
    return (fn == 6'h10 || fn == 6'h11 || (fn >= 6'h13 && fn <= 6'h15)) ? 3'b001 : ((fn >= 6'h20 && fn <= 6'h23) || fn == 6'h27) ? 3'b011
         : (fn >= 6'h24 && fn <= 6'h26) ? 3'b111 : 3'b000;
endfunction
// ... CFC2's or CTC2's ({written, used}): rd's (rd & 3 of 3 is VCE, as 2)
function automatic logic [5:0] x_flags(input logic [31:0] i);
    logic [2:0] m; m = (i[12:11] == 2'd3) ? 3'b100 : 3'b001 << i[12:11];
    return (i[31:26] == 6'h12 && !i[25] && !i[24] && i[22:21] == 2'b10) ? {i[23] ? m : 3'b000, m} : 6'd0;
endfunction
// the pairing rule: the first writes no vector or control register the second reads or writes. A one-lane op's
// lane field (vs) or VNOP's vd is taken for a register, as on the console -- VNOP's only behind MTC2 or LTV
// (a reserved function reads vs and vt and writes vd -- 0 -- though the console's rule does not count it: no pair
// with what writes them or uses vd.) The computation c first, x second:
function automatic logic pair_cx(input logic [31:0] c, input logic [31:0] x);
    logic [2:0] cu_; logic [7:0] xr; logic [5:0] xf;
    cu_ = c_use(c) | {3{c_resv(c)}}; xr = x_vreg(x); xf = x_flags(x);
    return !(cu_[0] && (xr[7] || xr[6]) && xr[4:0] == c[10:6]) && !(c_flags(c) & xf[2:0]);
endfunction
// ... x first, the computation c second
function automatic logic pair_xc(input logic [31:0] x, input logic [31:0] c);
    logic [2:0] cu_; logic [7:0] xr; logic [5:0] xf; logic one, nop;
    cu_ = c_use(c) | {3{c_resv(c)}}; xr = x_vreg(x); xf = x_flags(x);
    one = c[5:0] >= 6'h30 && c[5:0] <= 6'h36; nop = c[5:0] == 6'h37;
    return !(xr[6] && ((cu_[2] && xr[4:0] == c[15:11]) || (cu_[1] && xr[4:0] == c[20:16]) || (cu_[0] && xr[4:0] == c[10:6])
                       || (one && xr[4:0] == c[15:11]) || (nop && x[31:26] == 6'h12 && xr[4:0] == c[10:6])))
        && !(xf[5:3] & c_flags(c));
endfunction
wire  [31:0] ins_d = i_data;
logic        stall_e /* verilator public_flat_rd */;
wire         v_first = is_vc(ins_d);
// The rule for two words to be a pair is a function of the two (pair_rule): the IMEM's second port brings its result with
// the second word (p2_ok, made when the word was written), so that it is not in front of the fetch's address. What is left is
// the unit's own state: D holds an instruction and IMEM's second port read the next, the unit runs, and D's instruction is not
// a branch's delay slot (a branch or jump in E).
function automatic logic pair_rule(input logic [31:0] w0, input logic [31:0] w1);
    return is_vc(w0) ? is_plain(w1) || is_br(w1) || (x_ok(w1) && pair_cx(w0, w1))
                     : (is_plain(w0) || (x_ok(w0) && pair_xc(w0, w1))) && is_vc(w1);
endfunction
assign pw_ok = pair_rule(pw_a, pw_b);
wire         pair_d = d_valid && i2_ok && run && !(e_valid && (e_c.br || e_c.j || e_c.jr)) && p2_ok;
wire  [31:0] s_ins_d = (pair_d && v_first) ? i_data2 : ins_d;   // the scalar unit's instruction entering E
wire  [31:0] v_ins_d = v_first ? ins_d : i_data2;               // a pair's computation
wire  [31:0] c_ins_d = pair_d ? v_ins_d : ins_d;                // the coprocessors' instruction entering E
wire         pair_go = pair_d && !stall_e;

// ---- the register file: two copies, read with the instruction arriving in D
logic [31:0] rf_qa, rf_qb;               // (the operands')
logic [31:0] rf_qac, rf_qbc;             // (the branch condition's: copies of the file, one for each use, so that a word has few loads)
logic [11:0] rf_qam;                     // (the address adders': the low 12 bits)
wire  [4:0]  e_rs = e_ins[25:21], e_rt = e_ins[20:16];
// (the read address is E's own while it waits: the file is read again, and has by then every result older than the one in W)
wire  [4:0]  ra_d = stall_e ? e_rs : s_ins_d[25:21];
wire  [4:0]  rb_d = stall_e ? e_rt : s_ins_d[20:16];
rsp_rfram_oc #(.W(76)) rfa3 (.clk, .we(w_we), .wa(w_wr), .wd({w_res[11:0], w_res, w_res}), .ra_n(ra_d), .q({rf_qam, rf_qac, rf_qa}));   // (the three copies read at rs: one memory, cut in slices of 16 bits)
rsp_rfram_oc #(.W(64)) rfb2 (.clk, .we(w_we), .wa(w_wr), .wd({w_res, w_res}), .ra_n(rb_d), .q({rf_qbc, rf_qb}));

// ---- E: the operands. Where each comes from is chosen by registers set the cycle before for the instruction that will be
// in E (D's, or E's own if it waits): the writer in M (its result is m_res), the one in W (w_res), or the register file, which
// has what is older than those (it is written at the end of W's cycle and read the cycle after, from an address registered a
// cycle ahead)
logic        fa_m, fa_w, fb_m, fb_w;
logic [31:0] ea, eb;
always_comb begin
    ea = ({32{fa_m}} & m_res) | ({32{fa_w}} & w_res) | ({32{!(fa_m | fa_w)}} & rf_qa);
    eb = ({32{fb_m}} & m_res) | ({32{fb_w}} & w_res) | ({32{!(fb_m | fb_w)}} & rf_qb);
end
// next cycle's choice, for a register number r read by D's instruction (the cycle's E goes: the writer is E's, now in M then)
// or by E's own (it waits: M's writer is in W then)
function automatic logic [1:0] fsel(input logic [4:0] r_d, input logic [4:0] r_e, input logic stall,
                                    input logic e_wr_v, input logic [4:0] e_wr, input logic m_wr_v, input logic [4:0] m_wr);
    logic nm;
    nm = e_wr_v && e_wr == r_d;
    if (!stall) return {nm, !nm && m_wr_v && m_wr == r_d};
    return {1'b0, m_wr_v && m_wr == r_e};
endfunction

// ---- a load's value (and CFC2's) comes in M: the instruction behind it that reads it waits a cycle, then has it
// from W. Every other value is forwarded; what the vector unit's own instructions wait for it keeps itself
logic hazard /* verilator public_flat_rd */;

wire [31:0] imm_sx = {{16{e_ins[15]}}, e_ins[15:0]};
wire [31:0] op_b   = e_c.b_imm ? (e_c.imm_zx ? {16'h0, e_ins[15:0]} : imm_sx) : eb;
// (the shift amount: what does not wait for the register file -- the immediate, the writer in M, the one in W -- is chosen first,
// and the file's word is the last term, not the third of a choice made after the operand's: one level after the file, not two)
wire        sa_r   = !e_c.sh_imm && !fa_m && !fa_w;
(* keep *) wire [4:0] sa_e = e_c.sh_imm ? e_ins[10:6] : (fa_m ? m_res[4:0] : 5'd0) | (fa_w ? w_res[4:0] : 5'd0);
wire [4:0]  shamt  = sa_e | ({5{sa_r}} & rf_qa[4:0]);
// one shifter, to the right: SLL's operand and result are reversed in bit order (a left shift is a right shift of the reversed word),
// the sign-extended value for SRA. The shifts by 16, 8, 4, 2 and 1 are stages in this order, the first with the operand's reversal in it (a stage
// of two words and the choice of reversing is six inputs), so that the word is three LUTs deep and not four
wire        sh_rev  = e_c.alu == A_SLL;
wire        sh_fill = e_c.alu == A_SRA && eb[31];
logic [31:0] eb_rv, sh_y, sh_yr, sh_a, sh_b, sh_c, sh_d;
always_comb for (int i = 0; i < 32; i++) eb_rv[i] = eb[31 - i];
always_comb for (int i = 0; i < 32; i++) sh_yr[i] = sh_y[31 - i];
wire [31:0] sh_x    = sh_rev ? eb_rv : eb;
always_comb for (int i = 0; i < 32; i++) sh_a[i] = shamt[4] ? (i + 16 < 32 ? sh_x[(i + 16) & 31] : sh_fill) : sh_x[i];
always_comb for (int i = 0; i < 32; i++) sh_b[i] = shamt[3] ? (i + 8  < 32 ? sh_a[(i + 8)  & 31] : sh_fill) : sh_a[i];
always_comb for (int i = 0; i < 32; i++) sh_c[i] = shamt[2] ? (i + 4  < 32 ? sh_b[(i + 4)  & 31] : sh_fill) : sh_b[i];
always_comb for (int i = 0; i < 32; i++) sh_d[i] = shamt[1] ? (i + 2  < 32 ? sh_c[(i + 2)  & 31] : sh_fill) : sh_c[i];
always_comb for (int i = 0; i < 32; i++) sh_y[i] = shamt[0] ? (i + 1  < 32 ? sh_d[(i + 1)  & 31] : sh_fill) : sh_d[i];
wire [31:0] sh_r    = sh_y;
wire [31:0] sh_l    = sh_yr;
// one adder for add, subtract and both compares: ea + op_b, or ea - op_b = ea + ~op_b + 1,
// whose carry out is "no borrow" (ea >= op_b unsigned) and whose sign decides a signed
// compare when the signs agree
wire        sub_op = e_c.alu == A_SUB || e_c.alu == A_SLT || e_c.alu == A_SLTU;
wire [33:0] sum_c  = {1'b0, ea, 1'b1} + {1'b0, op_b ^ {32{sub_op}}, sub_op};      // (+ the carry in as the chain's own bit)
wire [31:0] sum    = sum_c[32:1];
wire        sum_co = sum_c[33];
wire        lt_u   = !sum_co;
wire        lt_s   = (ea[31] != op_b[31]) ? ea[31] : sum[31];
// The result: an AND-OR of terms whose selects are decodes of E's registers (the instruction's function, its link and coprocessor
// flags -- early, beside the register file's read), not a case behind an operand's choice and then the link / coprocessor choice:
// the late words -- the shifters' -- are in the last LUT, with what is not late ahead of it
wire        r_lk  = e_c.link;
wire        r_cp  = !e_c.link && e_c.cop;
wire        r_al  = !e_c.link && !e_c.cop;                          // (the ALU's own)
wire        r_sum = r_al && (e_c.alu == A_ADD || e_c.alu == A_SUB);
wire        r_and = r_al && e_c.alu == A_AND, r_or = r_al && e_c.alu == A_OR, r_xor = r_al && e_c.alu == A_XOR, r_nor = r_al && e_c.alu == A_NOR;
wire        r_slt = r_al && e_c.alu == A_SLT, r_sltu = r_al && e_c.alu == A_SLTU;
wire        r_shl = r_al && e_c.alu == A_SLL;
wire        r_shr = r_al && (e_c.alu == A_SRL || e_c.alu == A_SRA);
wire        r_lui = r_al && e_c.alu == A_LUI;
(* keep *) wire [31:0] e_res_a = ({32{r_lk}} & {20'h0, e_spc + 12'd8}) | ({32{r_cp}} & c_res) | ({32{r_lui}} & {e_ins[15:0], 16'h0})
                  | ({32{r_sum}} & sum) | ({32{r_and}} & (ea & op_b)) | ({32{r_or}} & (ea | op_b)) | ({32{r_xor}} & (ea ^ op_b)) | ({32{r_nor}} & ~(ea | op_b))
                  | {31'b0, (r_slt & lt_s) | (r_sltu & lt_u)};
wire [31:0] e_res = e_res_a | ({32{r_shl}} & sh_l) | ({32{r_shr}} & sh_r);

// A branch's condition is made from the sources, not from the operands after the choice among them: the compares of the
// pairs of sources (the choice is then among their results) and the zero of each. The choices are AND-OR of terms whose selects
// are registers (the sources' one-hot: a_m, a_w, a_r for ea, b_* for eb; the condition's decode), not nested 2:1 choices, so that
// the compare's result is two levels behind the register file's words (a compare of 32 bits is three, with the choice)
wire a_m = fa_m, a_w = fa_w, a_r = !(fa_m | fa_w), b_m = fb_m, b_w = fb_w, b_r = !(fb_m | fb_w);
wire eq_mw = m_res == w_res;
wire h1_mr, h2_mr, h1_rm, h2_rm, h1_wr, h2_wr, h1_rw, h2_rw, h1_rr, h2_rr;
rsp_eq32h c_mr (.a(m_res),  .b(rf_qbc), .h1(h1_mr), .h2(h2_mr));
rsp_eq32h c_rm (.a(rf_qac), .b(m_res),  .h1(h1_rm), .h2(h2_rm));
rsp_eq32h c_wr (.a(w_res),  .b(rf_qbc), .h1(h1_wr), .h2(h2_wr));
rsp_eq32h c_rw (.a(rf_qac), .b(w_res),  .h1(h1_rw), .h2(h2_rw));
rsp_eq32h c_rr (.a(rf_qac), .b(rf_qbc), .h1(h1_rr), .h2(h2_rr));
(* keep *) wire ea_eq_eb = (a_m & b_m) | (a_w & b_w) | ((a_m & b_w | a_w & b_m) & eq_mw) | (a_m & b_r & h1_mr & h2_mr) | (a_r & b_m & h1_rm & h2_rm)
              | (a_w & b_r & h1_wr & h2_wr) | (a_r & b_w & h1_rw & h2_rw) | (a_r & b_r & h1_rr & h2_rr);
(* keep *) wire ea_zero  = (a_m & (m_res == 32'd0)) | (a_w & (w_res == 32'd0)) | (a_r & (rf_qac == 32'd0));
(* keep *) wire ea31     = (a_m & m_res[31]) | (a_w & w_res[31]) | (a_r & rf_qac[31]);
logic taken;                                                                                // (by the condition's code, a register, over the three: one LUT)
always_comb begin
    unique case (e_c.bcond)
    3'd0: taken = ea_eq_eb;
    3'd1: taken = !ea_eq_eb;
    3'd2: taken = ea31 || ea_zero;               // blez
    3'd3: taken = !ea31 && !ea_zero;             // bgtz
    3'd4: taken = ea31;
    3'd5: taken = !ea31;
    default: taken = 1'b0;
    endcase
end
wire bt    = taken;                                                                 // (a taken branch: the latest signal of the unit)
wire [11:0] br_tgt = e_spc + 12'd4 + {imm_sx[9:0], 2'b00};
wire [11:0] j_tgt  = {e_ins[9:0], 2'b00};
wire        e_go   = e_valid && !stall_e;                 // E's instruction leaves it
// (a register: made a cycle ahead, when the load leaves E, from the registers of the instruction that takes E's place -- D's; a
// load that leaves E is always followed by the cycle in which M has it, and E's stall does not repeat the check)
always_ff @(posedge clk) hazard <= !rst && e_go && e_c.we && (e_c.load || e_c.cf) && (e_c.wr == s_ins_d[25:21] || e_c.wr == s_ins_d[20:16]);
wire        redirect = e_go && (bt || e_c.j || e_c.jr);

// memory
// (the address is made from each source and chosen, as the branch's condition is: the operand's choice is not in front of the adder.
// The offset (e_ao, and with 16 more, e_ao16) is the scalar load's or store's immediate, or a vector load's or store's offset scaled by its
// size, both registered with the instruction: the same adders make the vector unit's addresses)
function automatic logic [11:0] offs(input logic [31:0] i);      // the vector offset scaled by the access's size
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
// (the immediate is E's instruction's own low bits; the vector offset is a register of its own, and E's registered choice, e_vls, is between them after
// the registers, so that no choice stands between the instruction word and a register, nor after the adder that makes the offset with 16 more: the next line's.
// The low four bits of that are the offset's, bits 11 .. 4 are two registers, the immediate's and the vector offset's)
logic        e_vls;                                         // (E's instruction is a vector load or store: registered below)
logic [11:0] e_off;
logic [11:4] e_imm16, e_off16;
wire  [11:0] im_16 = s_ins_d[11:0] + 12'd16, of_16 = offs(s_ins_d) + 12'd16;
always_ff @(posedge clk) if (!stall_e) begin e_off <= offs(s_ins_d); e_imm16 <= im_16[11:4]; e_off16 <= of_16[11:4]; end
wire  [11:0] e_ao   = e_vls ? e_off : e_ins[11:0];
wire  [11:0] e_ao16 = {e_vls ? e_off16 : e_imm16, e_ao[3:0]};
wire [11:0] ad_m = m_res[11:0] + e_ao, ad_w = w_res[11:0] + e_ao, ad_r = rf_qam[11:0] + e_ao;
wire [11:0] adh_m = m_res[11:0] + e_ao16, adh_w = w_res[11:0] + e_ao16, adh_r = rf_qam[11:0] + e_ao16;
wire [31:0] addr_v = {20'd0, fa_m ? ad_m : fa_w ? ad_w : ad_r};
assign m_valid = e_valid && (e_c.load || e_c.store) && !hazard;
assign m_we = e_c.store;
assign m_addr = addr_v[11:0];
assign m_addr16 = fa_m ? adh_m : fa_w ? adh_w : adh_r;
assign m_addr_e   = ({12{fa_m}} & ad_m)  | ({12{fa_w}} & ad_w);
assign m_addr16_e = ({12{fa_m}} & adh_m) | ({12{fa_w}} & adh_w);
assign m_addr_r   = ad_r;
assign m_addr16_r = adh_r;
assign m_sel_r    = !(fa_m | fa_w);
assign m_n = e_c.size;
// (the store's bytes, the first of them the word's first: a byte or a half-word is the register's low byte or bytes there; the bytes after the size's are not
// written -- no enable -- and are what the word's choice leaves)
assign m_wdata = {e_c.size == 2'd3 ? eb[31:24] : e_c.size == 2'd1 ? eb[15:8] : eb[7:0], e_c.size[1] ? eb[23:16] : eb[7:0], eb[15:0]};

// the coprocessors (a pair's computation goes when its scalar instruction can: neither waits on a load nor on DMEM)
wire        s_mwait /* verilator public_flat_rd */ = (m_stall && e_c.load) || (m_stall_st && e_c.store);
assign c_valid = e_valid && !hazard && (e_c.cop || (v_e && !s_mwait));
assign c_ins = v_e ? v_ins : e_ins;
assign c_pr = v_e;
assign c_lins = e_ins;
assign c_ins_n = c_ins_d;
assign c_lins_n = s_ins_d;
assign c_adv = !stall_e;
always_ff @(posedge clk) if (!stall_e) e_vls <= s_ins_d[31:26] == 6'h32 || s_ins_d[31:26] == 6'h3A;
assign c_vls = e_vls;
assign c_a = ea;
assign c_bm = m_res[3:0];
assign c_bw = w_res[3:0];
assign c_br = rf_qam[3:0];
assign c_om = ad_m;  assign c_ow = ad_w;  assign c_or = ad_r;
assign c_hm = adh_m; assign c_hw = adh_w; assign c_hr = adh_r;
assign c_o4 = e_ao[3:0];
assign c_fm = fa_m;
assign c_fw = fa_w;
assign c_b = eb;
function automatic logic [4:0] vrb(input logic [31:0] i);      // MFC2, MTC2 (COP2, rs 0 or 4): rd
    return (i[31:26] == 6'h12 && i[25:24] == 2'b00 && i[22:21] == 2'b00) ? i[15:11] : i[20:16];
endfunction
assign c_ra_n = stall_e ? c_ins[15:11] : c_ins_d[15:11];
assign c_rb_n = stall_e ? c_ins[20:16] : c_ins_d[20:16];
// (the two candidates' fields first, then the pair's choice: the pairing rule is not in front of them)
wire         s2_d   = pair_d && v_first;                        // E's scalar instruction is D's next word
wire  [4:0]  vrb_d  = s2_d ? vrb(i_data2) : vrb(ins_d);
assign c_rd_n = stall_e ? c_rd : vrb_d;

assign stall_e = e_valid && (hazard || ((e_c.cop || v_e) && !c_done) || s_mwait);
assign brk = e_go && e_c.brk;

// ---- M
// A load's value is picked out of the word DMEM's read brings (m_rdata: the bytes from the address on, the first in [31:24]) by its size
// -- a byte or a half-word is sign- or zero-extended, from the word's top bit -- and what M holds otherwise (CFC2's word, or the
// result) is the other term: an AND-OR of the word's bits with selects that are decodes of M's registers, not the size's case and
// the choice of the source behind it, so that the word goes through one LUT, not two
wire        l_w  = m_c.load && m_c.size[1];                           // a word
wire        l_h  = m_c.load && m_c.size == 2'd1;                      // a half-word
wire        l_b  = m_c.load && m_c.size == 2'd0;                      // a byte
wire        l_e  = (l_h || l_b) && !m_c.lu;                           // ... extended by its sign
wire [31:0] m_nl = m_c.cf ? c_res_l : m_res;
wire [31:0] m_out = {({16{l_w}} & m_rdata[31:16]) | {16{l_e & m_rdata[31]}} | ({16{!m_c.load}} & m_nl[31:16]),
                     ({8{l_w}} & m_rdata[15:8]) | ({8{l_h}} & m_rdata[31:24]) | {8{l_b & l_e & m_rdata[31]}} | ({8{!m_c.load}} & m_nl[15:8]),
                     ({8{l_w}} & m_rdata[7:0]) | ({8{l_h}} & m_rdata[23:16]) | ({8{l_b}} & m_rdata[31:24]) | ({8{!m_c.load}} & m_nl[7:0])};

// ---- sequencing
// (the fetch pointer's next words are kept: pf1 = pc_f's word + 1, pf2 = + 2, dp1 = D's word + 1, so that the address is a
// choice between registers by the stall and the pair)
logic [9:0]  pf1, dp1;
wire  [9:0]  pf2 = pc_f[11:2] + 10'd2;
assign i_addr  = stall_e ? d_pc[11:2] : pair_go ? pf1 : pc_f[11:2];
// (after a redirect pf1 is not made: it is the fetch pointer's word + 1, from an adder on the register, for the one cycle)
logic        r_fix;
wire  [9:0]  pf1c = pc_f[11:2] + 10'd1;
assign i_addr1 = stall_e ? dp1        : pair_go ? pf2 : r_fix ? pf1c : pf1;
assign pc_next = d_valid ? d_pc : pc_f;
assign idle = !e_valid && !m_valid_r;

// the fetch pointer and the word after it, in the cycle E's instruction leaves: a taken branch's target, or, if none is taken, where a
// BREAK stopped it, D's instruction when stopped, or the next word (two with a pair). Where E stalls, the pointer stays: that is the
// last choice, so the stall (the longest chain of the unit) is one 2:1 choice before the register.
wire        e_go0 = e_valid;                                                              // (E's instruction, if it is not stalled)
wire        brk0  = e_go0 && e_c.brk;
wire        jmp0  = e_go0 && (e_c.j || e_c.jr);                                           // (a jump, whose target does not wait for a condition)
wire [11:0] pc_nr0 = brk0     ? (d_valid ? d_pc : pc_f)
                   : !run     ? (d_valid ? d_pc : pc_f)
                   :            (pair_d ? pc_f + 12'd8 : pc_f + 12'd4);
wire [9:0]  pf1_nr0 = brk0    ? (d_valid ? dp1 : pf1)
                    : !run    ? (d_valid ? dp1 : pf1)
                    :           (pair_d ? pc_f[11:2] + 10'd3 : pf2);
always_ff @(posedge clk) begin
    if (rst) begin
        pc_f <= 12'd0; r_fix <= 1'b0;
        d_valid <= 1'b0; e_valid <= 1'b0; m_valid_r <= 1'b0; v_e <= 1'b0;
        m_we_r <= 1'b0; w_we <= 1'b0;
    end else begin
        {fa_m, fa_w} <= fsel(s_ins_d[25:21], e_rs, stall_e, e_valid && e_c.we, e_c.wr, m_we_r, m_c.wr);
        {fb_m, fb_w} <= fsel(s_ins_d[20:16], e_rt, stall_e, e_valid && e_c.we, e_c.wr, m_we_r, m_c.wr);
        w_we <= m_valid_r && m_we_r; w_wr <= m_c.wr; w_res <= m_out;
        m_valid_r <= e_go;
        m_c <= e_c; m_res <= e_res;
        m_we_r <= e_go && e_c.we;
        if (!stall_e) begin
            e_valid <= d_valid && (run || redirect) && !brk;   // (a taken branch's delay slot always runs)
            e_pc <= d_pc; e_ins <= s_ins_d; e_c <= decode(s_ins_d);
            e_spc <= (pair_d && v_first) ? d_pc + 12'd4 : d_pc;
            v_e <= pair_d && !brk; v_ins <= v_ins_d;
        end
        if (!stall_e) c_rd <= vrb_d;
        if (redirect) r_fix <= 1'b1; else if (!stall_e) r_fix <= 1'b0;      // (until the cycle that moves the pointer on)
        // (D's address follows the pointer whatever the rest does: where D is not valid it is not looked at -- a redirect, a
        // stop, a BREAK, a new PC all empty it -- so the redirect is not in front of it)
        if (!stall_e) begin
            d_pc <= pair_go ? pc_f + 12'd4 : pc_f;
            dp1  <= pair_go ? pf2 : r_fix ? pf1c : pf1;
        end
        if (pc_load) begin
            d_valid <= 1'b0;
        end else if (brk) begin
            // what follows the BREAK was not run: fetch it again when resumed
            d_valid <= 1'b0;
        end else if (!run && !redirect) begin
            // stopped: D's instruction (not run) is the next one
            if (!stall_e) d_valid <= 1'b0;
        end else if (!stall_e) begin
            d_valid <= !redirect;
        end
        // (the fetch pointer and the word after it: a taken branch's target over what the rest would make -- the branch's condition
        // is late, so it is the last choice. After a redirect pf1 is not made: r_fix has the next cycle's address from pc_f)
        pf1 <= pc_load ? pc_in[11:2] + 10'd1 : stall_e ? pf1 : pf1_nr0;
        pc_f <= bt ? (pc_load ? {pc_in[11:2], 2'b00} : stall_e ? pc_f : e_go0 ? br_tgt : pc_nr0)
                   : (pc_load ? {pc_in[11:2], 2'b00} : stall_e ? pc_f : jmp0 ? (e_c.jr ? {ea[11:2], 2'b00} : j_tgt) : pc_nr0);
    end
end

assign tr_e = e_go;
assign tr_e2 = e_go && v_e;
assign tr_e_mem = e_valid && (e_c.load || e_c.store || e_ins[31:26] == 6'h32 || e_ins[31:26] == 6'h3A);
assign tr_e_pc = e_pc;
assign tr_e_we = e_c.we;
assign tr_e_wr = e_c.wr;
assign tr_we = w_we;
assign tr_wr = w_wr;
assign tr_val = w_res;

endmodule
`default_nettype wire
