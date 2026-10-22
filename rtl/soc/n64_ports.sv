// The SDRAM ports of Mirlo-N64's masters (sdr_arb's request side: valid,
// ready, we, word address, data, byte enables; read data back in order).
`default_nettype none

// ---- n64_sys's system memory: RDRAM, the cartridge, the save. N64 physical -> SDRAM word:
//   RDRAM 0x0000_0000-0x007F_FFFF -> SDRAM 0
//   cart  0x1000_0000-0x1FBF_FFFF -> SDRAM CART_OFF (the Pocket loaded the ROM there)
//   save  SAVE_BASE..             -> SDRAM SAVE_OFF (the EEPROM: data slot 1)
// A read is taken at once (s_ack) and its words (8 for a line) come back as
// s_rvalid; a write is acknowledged once the arbiter has it.
module port_n64sys #(
    parameter bit          FLAT      = 0,               // native Mirlo: SDRAM's word = address[25:2]
    parameter logic [31:0] CART_OFF  = 32'h0100_0000,
    parameter logic [31:0] SAVE_OFF  = 32'h03FF_0000,
    parameter logic [31:0] SAVE_BASE = 32'h1FE0_0000
) (
    input  wire         clk, rst,
    input  wire         s_req, s_we, s_line, s_wrun,
    input  wire  [31:0] s_addr, s_wdata,
    input  wire  [3:0]  s_strb,
    output logic        s_ack, s_rvalid,
    output logic [31:0] s_rdata,
    output wire         busy,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
logic [31:0] a;
always_comb
    if (FLAT) a = s_addr;
    else if (s_addr < 32'h0080_0000) a = s_addr;
    else if (s_addr >= 32'h1000_0000 && s_addr < 32'h1FC0_0000) a = s_addr - 32'h1000_0000 + CART_OFF;
    else if (s_addr >= SAVE_BASE) a = s_addr - SAVE_BASE + SAVE_OFF;
    else a = s_addr;
// A write is acknowledged as it is taken here, so the next one of a run can
// follow it to the arbiter with no gap (a gap gives the grant away: the bus
// turns for every word). `busy`: one is still on its way to the SDRAM.
typedef enum logic [1:0] { P_IDLE, P_WRITE, P_READ } pst_t;
pst_t st;
logic [23:0] base;
logic [3:0]  n, issued, got;
assign p_we    = st == P_WRITE;
assign p_valid = st == P_WRITE || (st == P_READ && issued != n);
assign p_addr  = base + {20'd0, st == P_READ ? issued : 4'd0};
// (only a run's: the grant then reaches the write buffer's pointer and no
// further into the CPU -- any other access is acknowledged from P_IDLE)
wire   w_next  = st == P_WRITE && p_ready && s_req && s_we && s_wrun;
assign s_ack   = (st == P_IDLE && s_req) || w_next;
assign busy    = st == P_WRITE;
assign s_rvalid = st == P_READ && p_rvalid;
assign s_rdata  = p_rdata;
always_ff @(posedge clk) begin
    if (rst) st <= P_IDLE;
    else case (st)
    P_IDLE: if (s_req) begin
        base <= a[25:2]; p_wdata <= s_wdata; p_be <= s_strb;
        n <= s_line ? 4'd8 : 4'd1; issued <= 0; got <= 0;
        st <= s_we ? P_WRITE : P_READ;
    end
    P_WRITE: if (p_ready) begin
        if (w_next) begin base <= a[25:2]; p_wdata <= s_wdata; p_be <= s_strb; end
        else st <= P_IDLE;
    end
    P_READ: begin
        if (p_valid && p_ready) issued <= issued + 4'd1;
        if (p_rvalid) begin got <= got + 4'd1; if (got + 4'd1 == n) st <= P_IDLE; end
    end
    endcase
end
endmodule

// ---- RDP's memory port (LiteDRAM-native-like: a command, and a write's data
// on its own channel after it). Commands queue here (4) so RDP is not held
// to one in flight; a write goes to the arbiter with its data.
module port_rdp (
    input  wire         clk, rst,
    input  wire         m_cmd_valid, m_cmd_we,
    input  wire  [31:0] m_cmd_addr,         // byte address on RDP's bus (SDRAM at 0x4000_0000)
    output logic        m_cmd_ready,
    input  wire         m_wdata_valid,
    input  wire  [31:0] m_wdata,
    input  wire  [3:0]  m_wdata_we,
    output logic        m_wdata_ready,
    output logic        m_rdata_valid,
    output logic [31:0] m_rdata,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
logic [24:0] q [4];                         // {we, word address}
logic [2:0]  qw, qr;
wire  [2:0]  qn = qw - qr;
assign m_cmd_ready = qn != 3'd4;
wire  [24:0] head = q[qr[1:0]];
assign p_we    = head[24];
assign p_addr  = head[23:0];
assign p_wdata = m_wdata;
assign p_be    = m_wdata_we;
assign p_valid = qn != 0 && (!head[24] || m_wdata_valid);
assign m_wdata_ready = p_valid && p_we && p_ready;
assign m_rdata_valid = p_rvalid;
assign m_rdata = p_rdata;
always_ff @(posedge clk) begin
    if (rst) begin qw <= 0; qr <= 0; end
    else begin
        if (m_cmd_valid && m_cmd_ready) begin q[qw[1:0]] <= {m_cmd_we, m_cmd_addr[25:2]}; qw <= qw + 3'd1; end
        if (p_valid && p_ready) qr <= qr + 3'd1;
    end
end
endmodule

// ---- the Pocket's bridge (apf_wishbone_master: word addresses from 0x1000_0000;
// what of them is the SDRAM's: mirlo_mips.sv, the bridge's map). x_sel: the
// access is the PSRAM's instead (a ROM's first 32 MiB -- the bridge's writes
// there, the PI engine's reads) -- x_valid until x_ack, a read's data x_rdata
// in its cycle.
module port_apf (
    input  wire         clk, rst,
    input  wire         cyc, stb, we,
    input  wire  [29:0] adr,
    input  wire  [31:0] dat_w,
    input  wire  [3:0]  sel,
    output logic        ack,
    output logic [31:0] dat_r,
    input  wire         x_sel,
    output wire         x_valid,
    input  wire         x_ack,
    input  wire  [31:0] x_rdata,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
typedef enum logic [1:0] { A_IDLE, A_REQ, A_WAIT, A_ACK } ast_t;
ast_t st;
assign p_valid = st == A_REQ && !x_sel;
assign x_valid = st == A_REQ && x_sel;
assign p_we = we;
assign p_addr = adr[23:0];
assign p_wdata = dat_w;
assign p_be = sel;
always_ff @(posedge clk) begin
    ack <= 0;
    if (rst) st <= A_IDLE;
    else case (st)
    A_IDLE: if (cyc && stb && !ack) st <= A_REQ;
    A_REQ:  if (x_sel) begin if (x_ack) begin dat_r <= x_rdata; ack <= 1; st <= A_ACK; end end
            else if (p_ready) begin if (we) begin ack <= 1; st <= A_ACK; end else st <= A_WAIT; end
    A_WAIT: if (p_rvalid) begin dat_r <= p_rdata; ack <= 1; st <= A_ACK; end
    A_ACK:  st <= A_IDLE;                   // (the master drops its request after the ack)
    endcase
end
endmodule
`default_nettype wire
