// rsp_su: the RSP's scalar unit. The N64 RSP's integer instruction set (MIPS
// without multiply, divide, exceptions or 64-bit operations; add/addi/sub as
// their unsigned forms) with a 12-bit PC, on mips_lite's five-stage pipeline:
//   F  the PC goes to IMEM (a synchronous memory: the word comes back the
//      next cycle)
//   D  decode; the register file is read with the arriving instruction
//   E  forwarding, the ALU, branches and jumps (the delay slot is in D and
//      runs; what is being fetched is squashed if taken), the memory access
//      (rsp_top's DMEM: any byte address, 1 to 4 bytes, in the cycle), the
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

module rsp_su #(
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
    input  wire  [31:0] i_data,
    input  wire  [31:0] i_data2,        // the word after i_data's (IMEM's second port) ...
    input  wire         i2_ok,          // ... when that port read it (not the DMA's)

    // DMEM: bytes m_addr .. m_addr + m_n, first byte in [31:24]; the read
    // data the next cycle
    output logic        m_valid,
    output logic        m_we,
    output logic [11:0] m_addr,
    output logic [1:0]  m_n,            // bytes - 1 (0, 1 or 3)
    output logic [31:0] m_wdata,
    input  wire  [31:0] m_rdata,
    input  wire         m_stall,        // DMEM's port is the DMA's this cycle: a load or store waits (E holds)

    // the coprocessors
    output logic        c_valid,
    output logic [31:0] c_ins,
    output logic [31:0] c_a,            // rs
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
    6'h12: begin c.cop = 1'b1; c.we = !i[25] && (rs == 5'd0 || rs == 5'd2); c.wr = rt; c.cf = !i[25] && rs == 5'd2; end   // MFC2, CFC2 write rt
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
logic        x_we;     logic [4:0] x_wr;   logic [31:0] x_res;

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
// (not behind a branch or jump in E: D's instruction is its delay slot; a branch only second)
wire         pair_d = d_valid && i2_ok && run && !(e_valid && (e_c.br || e_c.j || e_c.jr))
                   && (v_first ? is_plain(i_data2) || is_br(i_data2) || is_c0(i_data2) || (x_ok(i_data2) && pair_cx(ins_d, i_data2))
                               : (is_plain(ins_d) || is_c0(ins_d) || (x_ok(ins_d) && pair_xc(ins_d, i_data2))) && is_vc(i_data2));
wire  [31:0] s_ins_d = (pair_d && v_first) ? i_data2 : ins_d;   // the scalar unit's instruction entering E
wire  [31:0] v_ins_d = v_first ? ins_d : i_data2;               // a pair's computation
wire  [31:0] c_ins_d = pair_d ? v_ins_d : ins_d;                // the coprocessors' instruction entering E
wire         pair_go = pair_d && !stall_e;

// ---- the register file: two copies, read with the instruction arriving in D
logic [31:0] rf_a [0:31];
logic [31:0] rf_b [0:31];
logic [31:0] rf_qa, rf_qb;
wire  [4:0]  ra_d = stall_e ? e_ins[25:21] : s_ins_d[25:21];
wire  [4:0]  rb_d = stall_e ? e_ins[20:16] : s_ins_d[20:16];
always_ff @(posedge clk) begin
    if (w_we) begin rf_a[w_wr] <= w_res; rf_b[w_wr] <= w_res; end
    rf_qa <= rf_a[ra_d]; rf_qb <= rf_b[rb_d];
end

// ---- E
wire  [4:0]  e_rs = e_ins[25:21], e_rt = e_ins[20:16];
logic [31:0] ea, eb;
always_comb begin
    ea = rf_qa;
    if (x_we && x_wr == e_rs) ea = x_res;
    if (w_we && w_wr == e_rs) ea = w_res;
    if (m_we_r && m_c.wr == e_rs) ea = m_res;
    eb = rf_qb;
    if (x_we && x_wr == e_rt) eb = x_res;
    if (w_we && w_wr == e_rt) eb = w_res;
    if (m_we_r && m_c.wr == e_rt) eb = m_res;
end
// ---- a load's value (and CFC2's) comes in M: the instruction behind it that reads it waits a cycle, then has it
// from W. Every other value is forwarded; what the vector unit's own instructions wait for it keeps itself
wire hazard /* verilator public_flat_rd */ = m_valid_r && m_we_r && (m_c.load || m_c.cf) && (m_c.wr == e_rs || m_c.wr == e_rt);

wire [31:0] imm_sx = {{16{e_ins[15]}}, e_ins[15:0]};
wire [31:0] op_b   = e_c.b_imm ? (e_c.imm_zx ? {16'h0, e_ins[15:0]} : imm_sx) : eb;
wire [4:0]  shamt  = e_c.sh_imm ? e_ins[10:6] : ea[4:0];
// one shifter: SRL/SRA directly, SLL as a right shift of the bit-reversed value
function automatic logic [31:0] rev32(input logic [31:0] v);
    for (int i = 0; i < 32; i++) rev32[i] = v[31 - i];
endfunction
wire        sh_left = e_c.alu == A_SLL;
wire [32:0] sh_in   = {e_c.alu == A_SRA && eb[31], sh_left ? rev32(eb) : eb};
wire [32:0] sh_out  = $signed(sh_in) >>> shamt;
wire [31:0] sh_r    = sh_out[31:0];
wire [31:0] sh_l    = rev32(sh_out[31:0]);
// one adder for add, subtract and both compares: ea + op_b, or ea - op_b = ea + ~op_b + 1,
// whose carry out is "no borrow" (ea >= op_b unsigned) and whose sign decides a signed
// compare when the signs agree
wire        sub_op = e_c.alu == A_SUB || e_c.alu == A_SLT || e_c.alu == A_SLTU;
wire [33:0] sum_c  = {1'b0, ea, 1'b1} + {1'b0, op_b ^ {32{sub_op}}, sub_op};      // (+ the carry in as the chain's own bit)
wire [31:0] sum    = sum_c[32:1];
wire        sum_co = sum_c[33];
wire        lt_u   = !sum_co;
wire        lt_s   = (ea[31] != op_b[31]) ? ea[31] : sum[31];
logic [31:0] alu;
always_comb begin
    unique case (e_c.alu)
    A_ADD, A_SUB: alu = sum;
    A_AND:   alu = ea & op_b;
    A_OR:    alu = ea | op_b;
    A_XOR:   alu = ea ^ op_b;
    A_NOR:   alu = ~(ea | op_b);
    A_SLT:   alu = {31'b0, lt_s};
    A_SLTU:  alu = {31'b0, lt_u};
    A_SLL:   alu = sh_l;
    A_SRL, A_SRA: alu = sh_r;
    A_LUI:   alu = {e_ins[15:0], 16'h0};
    default: alu = 'x;
    endcase
end

logic taken;
always_comb begin
    unique case (e_c.bcond)
    3'd0: taken = ea == eb;
    3'd1: taken = ea != eb;
    3'd2: taken = $signed(ea) <= 0;
    3'd3: taken = $signed(ea) > 0;
    3'd4: taken = ea[31];
    3'd5: taken = !ea[31];
    default: taken = 1'b0;
    endcase
end
wire [11:0] br_tgt = e_spc + 12'd4 + {imm_sx[9:0], 2'b00};
wire [11:0] j_tgt  = {e_ins[9:0], 2'b00};
wire        e_go   = e_valid && !stall_e;                 // E's instruction leaves it
wire        redirect = e_go && ((e_c.br && taken) || e_c.j || e_c.jr);
wire [11:0] redirect_pc = e_c.jr ? {ea[11:2], 2'b00} : e_c.j ? j_tgt : br_tgt;

// memory
wire [31:0] addr_v = ea + imm_sx;
assign m_valid = e_valid && (e_c.load || e_c.store) && !hazard;
assign m_we = e_c.store;
assign m_addr = addr_v[11:0];
assign m_n = e_c.size;
assign m_wdata = e_c.size == 2'd0 ? {eb[7:0], 24'h0} : e_c.size == 2'd1 ? {eb[15:0], 16'h0} : eb;

// the coprocessors (a pair's computation goes when its scalar instruction can: neither waits on a load nor on DMEM)
wire        s_mwait /* verilator public_flat_rd */ = m_stall && (e_c.load || e_c.store);
assign c_valid = e_valid && !hazard && (e_c.cop || (v_e && !s_mwait));
assign c_ins = v_e ? v_ins : e_ins;
assign c_pr = v_e;
assign c_lins = e_ins;
assign c_a = ea;
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

wire [31:0] e_res = e_c.link ? {20'h0, e_spc + 12'd8} : e_c.cop ? c_res : alu;
assign stall_e = e_valid && (hazard || ((e_c.cop || v_e) && !c_done) || s_mwait);
assign brk = e_go && e_c.brk;

// ---- M
logic [31:0] ld;
always_comb begin
    unique case (m_c.size)
    2'd0:    ld = m_c.lu ? {24'h0, m_rdata[31:24]} : {{24{m_rdata[31]}}, m_rdata[31:24]};
    2'd1:    ld = m_c.lu ? {16'h0, m_rdata[31:16]} : {{16{m_rdata[31]}}, m_rdata[31:16]};
    default: ld = m_rdata;
    endcase
end
wire [31:0] m_out = m_c.load ? ld : m_c.cf ? c_res_l : m_res;

// ---- sequencing
assign i_addr = stall_e ? d_pc[11:2] : pair_go ? pc_f[11:2] + 10'd1 : pc_f[11:2];
assign pc_next = d_valid ? d_pc : pc_f;
assign idle = !e_valid && !m_valid_r;

always_ff @(posedge clk) begin
    if (rst) begin
        pc_f <= 12'd0;
        d_valid <= 1'b0; e_valid <= 1'b0; m_valid_r <= 1'b0; v_e <= 1'b0;
        m_we_r <= 1'b0; w_we <= 1'b0; x_we <= 1'b0;
    end else begin
        x_we <= w_we; x_wr <= w_wr; x_res <= w_res;
        w_we <= m_valid_r && m_we_r; w_wr <= m_c.wr; w_res <= m_out;
        m_valid_r <= e_go;
        m_c <= e_c; m_res <= e_res;
        m_we_r <= e_go && e_c.we;
        if (!stall_e) begin
            e_valid <= d_valid && (run || redirect) && !brk;   // (a taken branch's delay slot always runs)
            e_pc <= d_pc; e_ins <= s_ins_d; e_c <= s2_d ? decode(i_data2) : decode(ins_d);
            e_spc <= (pair_d && v_first) ? d_pc + 12'd4 : d_pc;
            v_e <= pair_d && !brk; v_ins <= v_ins_d;
        end
        if (!stall_e) c_rd <= vrb_d;
        if (pc_load) begin
            pc_f <= {pc_in[11:2], 2'b00}; d_valid <= 1'b0;
        end else if (brk) begin
            // what follows the BREAK was not run: fetch it again when resumed
            pc_f <= redirect ? redirect_pc : d_valid ? d_pc : pc_f;
            d_valid <= 1'b0;
        end else if (!run && !redirect) begin
            // stopped: D's instruction (not run) is the next one
            if (!stall_e) begin
                if (d_valid) pc_f <= d_pc;
                d_valid <= 1'b0;
            end
        end else if (!stall_e) begin
            // (a pair: D's next instruction went with it, the fetch is a word on)
            d_valid <= !redirect;
            d_pc <= pair_go ? pc_f + 12'd4 : pc_f;
            pc_f <= redirect ? redirect_pc : pair_go ? pc_f + 12'd8 : pc_f + 12'd4;
        end
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
