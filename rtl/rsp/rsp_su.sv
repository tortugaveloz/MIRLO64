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
// A load followed by a user stalls one cycle. BREAK, as it leaves E, stops
// the unit (brk): what follows it is not executed and pc_next names it.
`default_nettype none

module rsp_su (
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
    input  wire         c_done,
    input  wire  [31:0] c_res,          // MFC0, MFC2, CFC2
    // the vector registers' read addresses (their memories register them): of the instruction E holds in the cycle
    // to come, its rd field (vs), and the register it reads otherwise -- rd for MFC2 / MTC2, rt for the rest (vt);
    // c_rb is that of the one in E now
    output logic [4:0]  c_ra_n,
    output logic [4:0]  c_rb_n,
    output logic [4:0]  c_rb,

    // (simulation) each instruction as it leaves E, and each register write
    output logic        tr_e,
    output logic [11:0] tr_e_pc,
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
    6'h12: begin c.cop = 1'b1; c.we = !i[25] && (rs == 5'd0 || rs == 5'd2); c.wr = rt; end   // MFC2, CFC2 write rt
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
logic        d_valid;  logic [11:0] d_pc;
logic        e_valid;  logic [11:0] e_pc;  logic [31:0] e_ins;  ctl_t e_c;
logic        m_valid_r; logic [31:0] m_res; ctl_t m_c;
logic        m_we_r;
logic        w_we;     logic [4:0] w_wr;   logic [31:0] w_res;
logic        x_we;     logic [4:0] x_wr;   logic [31:0] x_res;

// ---- the register file: two copies, read with the instruction arriving in D
logic [31:0] rf_a [0:31];
logic [31:0] rf_b [0:31];
logic [31:0] rf_qa, rf_qb;
wire  [31:0] ins_d = i_data;
logic        stall_e;
wire  [4:0]  ra_d = stall_e ? e_ins[25:21] : ins_d[25:21];
wire  [4:0]  rb_d = stall_e ? e_ins[20:16] : ins_d[20:16];
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
wire hazard = m_valid_r && m_we_r && m_c.load && ((m_c.wr == e_rs) || (m_c.wr == e_rt));

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
wire [11:0] br_tgt = e_pc + 12'd4 + {imm_sx[9:0], 2'b00};
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

// the coprocessors
assign c_valid = e_valid && e_c.cop && !hazard;
assign c_ins = e_ins;
assign c_a = ea;
assign c_b = eb;
function automatic logic [4:0] vrb(input logic [31:0] i);      // MFC2, MTC2 (COP2, rs 0 or 4): rd
    return (i[31:26] == 6'h12 && i[25:24] == 2'b00 && i[22:21] == 2'b00) ? i[15:11] : i[20:16];
endfunction
assign c_ra_n = stall_e ? e_ins[15:11] : ins_d[15:11];
assign c_rb_n = stall_e ? c_rb : vrb(ins_d);

wire [31:0] e_res = e_c.link ? {20'h0, e_pc + 12'd8} : e_c.cop ? c_res : alu;
assign stall_e = e_valid && (hazard || (e_c.cop && !c_done) || (m_stall && (e_c.load || e_c.store)));
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
wire [31:0] m_out = m_c.load ? ld : m_res;

// ---- sequencing
assign i_addr = stall_e ? d_pc[11:2] : pc_f[11:2];
assign pc_next = d_valid ? d_pc : pc_f;
assign idle = !e_valid && !m_valid_r;

always_ff @(posedge clk) begin
    if (rst) begin
        pc_f <= 12'd0;
        d_valid <= 1'b0; e_valid <= 1'b0; m_valid_r <= 1'b0;
        m_we_r <= 1'b0; w_we <= 1'b0; x_we <= 1'b0;
    end else begin
        x_we <= w_we; x_wr <= w_wr; x_res <= w_res;
        w_we <= m_valid_r && m_we_r; w_wr <= m_c.wr; w_res <= m_out;
        m_valid_r <= e_go;
        m_c <= e_c; m_res <= e_res;
        m_we_r <= e_go && e_c.we;
        if (!stall_e) begin
            e_valid <= d_valid && (run || redirect) && !brk;   // (a taken branch's delay slot always runs)
            e_pc <= d_pc; e_ins <= ins_d; e_c <= decode(ins_d);
        end
        if (!stall_e) c_rb <= vrb(ins_d);
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
            d_valid <= !redirect;
            d_pc <= pc_f;
            pc_f <= redirect ? redirect_pc : pc_f + 12'd4;
        end
    end
end

assign tr_e = e_go;
assign tr_e_pc = e_pc;
assign tr_e_we = e_c.we;
assign tr_e_wr = e_c.wr;
assign tr_we = w_we;
assign tr_wr = w_wr;
assign tr_val = w_res;

endmodule
`default_nettype wire
