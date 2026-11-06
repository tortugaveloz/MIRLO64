// The SNAC port as the console's PIF: four Joybus ports, one master (an_joybus_m.sv) shared by a mux, a 128-byte buffer and the
// registers the runtime drives them with (SNAC, 0x0E00: tools/mirlo_regs.py; docs/analogizer.md "The SNAC port as the console's PIF").
//
//   ID    00 R   0x534E0001
//   CTRL  04 RW  [0] ON the pins are the Joybus's (needs the adapter to own the port: `own`), [1] SCAN the background scan,
//                [15:8] TFIRST: us to wait for a reply's first edge (reset 64)
//   STAT  08 R   [3:0] PRESENT: the device on that port answered the last scan; [7:4] CHANGED: it came or went since (write 1s clear)
//   XFER  0C     write: start a transaction {[17:16] port, [13:8] rx bytes expected, [5:0] tx bytes (the buffer's first)};
//                read: [31] BUSY (from the write until the reply is in), [30] TIMEOUT (nothing answered), [29] SHORT (fewer bytes than
//                expected, or the wire never went quiet), [27:16] the line time in us, [5:0] the bytes received
//   PTR   10 RW  [6:0] the buffer's pointer
//   DATA  14 RW  the byte at PTR; each access moves PTR on by one
//
// The buffer: 0..62 the transaction (the tx bytes, which the reply's bytes then replace), 64 + 4 n the scan of port n:
// the number of bytes the status command (0x00) was answered with (3: a device is there), then those bytes.
//
// One transaction at a time. The scan (SCAN set) takes the wire between the runtime's transactions, a port every 2 ms in turn:
// the status command (0x00) to the port, 3 bytes expected, the answer into the scan table; a transaction asked for meanwhile
// waits (BUSY is up from its write) at most one scan's time, ~170 us.
//
// Pins. A port is driven only low: the pin's data is 0 and its translator's direction (and the FPGA's driver) comes on for the low
// part of a cell only. Ports 1 and 2 are the pins 30 and 31, each with its own direction. Ports 3 and 4 are bits 4 and 7 of bank 0,
// whose translator has ONE direction: while either is pulled low the bank is an output and the other bit, and the two the SNAC
// does not use (5, 6), are driven 1 -- the level the pull-ups give them, held for the low pulse (1 to 3 us) of a line whose device
// is not being spoken to; in the rest of the cell, and while a device answers, the bank is an input and everything is released.
// The translator's direction leads the FPGA's driver by a clock when they come on and trails it when they go off (gb_cart.sv's rule).
`default_nettype none
module an_snac_pif #(
    parameter int CLK_HZ = 62_500_000
) (
    input  wire         clk, rst,
    // the registers (mirlo_regs.sv): the strobes already decoded for this block
    input  wire         sel_wr, sel_rd,
    input  wire  [3:0]  a,                  // r_addr[5:2]
    input  wire  [31:0] wd,
    output logic [31:0] rd,
    input  wire         own,                // the adapter owns the cartridge port (INTERACT2 bit 31)
    // the pins
    output wire         en,                 // the Joybus has the pins (the reset clamp is released)
    output wire         p30_dir, p30_oe,    // port 1: pin 30, driven 0 when oe
    output wire         p31_dir, p31_oe,    // port 2: pin 31
    output wire         b0_dir, b0_oe,      // ports 3 and 4: bank 0
    output wire  [3:0]  b0_o,               // bank 0's bits [7:4] when oe
    input  wire  [3:0]  dq_in               // the four wires: pin 30, pin 31, bank 0 bit 4, bank 0 bit 7
);

// ---------------------------------------------------------------- registers
logic        on, scan_en;
logic [7:0]  t_first;
logic [3:0]  present, chg;
logic [6:0]  ptr;
logic        pend;                          // a transaction of the runtime's is asked for or under way
logic [1:0]  x_port;
logic [5:0]  x_tx, x_rx;
logic        x_to, x_sh;
logic [5:0]  x_cnt;
logic [11:0] x_us;

// ---------------------------------------------------------------- the buffer: port A the runtime's, port B the master's
(* ramstyle = "M10K, no_rw_check" *) reg [7:0] mem [0:127];
reg [7:0] qa, qb;
integer ii;
initial for (ii = 0; ii < 128; ii = ii + 1) mem[ii] = 8'd0;
wire        data_wr = sel_wr && a == 4'd5;
wire        m_wr_en;
wire [6:0]  m_wr_addr;
wire [7:0]  m_wr_data;
logic       s_wr;                           // the scan's count byte
logic [6:0] s_wr_addr;
logic [7:0] s_wr_data;
wire [5:0]  m_rd_addr;
always @(posedge clk) begin
    if (data_wr) mem[ptr] <= wd[7:0];
    qa <= mem[ptr];
end
wire        b_wr = m_wr_en || s_wr;
wire [6:0]  b_addr = m_wr_en ? m_wr_addr : s_wr ? s_wr_addr : {1'b0, m_rd_addr};
always @(posedge clk) begin
    if (b_wr) mem[b_addr] <= m_wr_en ? m_wr_data : s_wr_data;
    qb <= mem[b_addr];
end

// ---------------------------------------------------------------- the master and the sequencer
logic        m_start, m_imm;
logic [5:0]  m_txlen, m_rxlen;
logic [6:0]  m_base;
logic [1:0]  sel;                           // the port the wire is for
wire         m_busy, m_done, m_timeout, m_short, m_low, m_tk;
wire  [5:0]  m_cnt;
wire  [11:0] m_us;
logic        wire_in;
an_joybus_m #(.CLK_HZ(CLK_HZ)) jb (
    .clk, .rst, .start(m_start), .imm(m_imm), .imm_byte(8'h00), .txlen(m_txlen), .rxlen(m_rxlen), .rx_base(m_base), .t_first,
    .busy(m_busy), .done(m_done), .rx_cnt(m_cnt), .timeout(m_timeout), .short_(m_short), .t_us(m_us), .tk(m_tk),
    .rd_addr(m_rd_addr), .rd_data(qb), .wr_en(m_wr_en), .wr_addr(m_wr_addr), .wr_data(m_wr_data), .low(m_low), .dq(wire_in)
);
always_comb case (sel)
    2'd0: wire_in = dq_in[0];
    2'd1: wire_in = dq_in[1];
    2'd2: wire_in = dq_in[2];
    default: wire_in = dq_in[3];
endcase

logic        scan_run;                      // the master is doing the scan's transaction
logic [1:0]  scan_p;
logic [11:0] scan_t;                        // us since the last scan transaction
wire         x_start = sel_wr && a == 4'd3 && !pend;
always_ff @(posedge clk) begin
    m_start <= 1'b0; s_wr <= 1'b0;
    if (rst) begin
        on <= 1'b0; scan_en <= 1'b0; t_first <= 8'd64; present <= '0; chg <= '0; ptr <= '0; pend <= 1'b0; scan_run <= 1'b0; scan_p <= '0;
        scan_t <= '0; x_to <= 1'b0; x_sh <= 1'b0; x_cnt <= '0; x_us <= '0; sel <= '0; x_port <= '0; x_tx <= '0; x_rx <= '0;
        m_imm <= 1'b0; m_txlen <= '0; m_rxlen <= '0; m_base <= '0;
    end else begin
        // the registers
        if (sel_wr && a == 4'd1) begin on <= wd[0]; scan_en <= wd[1]; t_first <= wd[15:8]; end
        if (sel_wr && a == 4'd2) chg <= chg & ~wd[7:4];
        if (sel_wr && a == 4'd4) ptr <= wd[6:0];
        else if ((sel_wr || sel_rd) && a == 4'd5) ptr <= ptr + 7'd1;
        if (x_start) begin pend <= 1'b1; x_port <= wd[17:16]; x_tx <= wd[5:0]; x_rx <= wd[13:8]; end
        // the master: the runtime's transaction first, else the scan's
        if (!m_busy && !m_start && !m_done && !scan_run) begin
            if (pend && x_tx != 6'd0) begin
                m_start <= 1'b1; m_imm <= 1'b0; m_txlen <= x_tx; m_rxlen <= x_rx; m_base <= 7'd0; sel <= x_port;
            end else if (pend) begin                    // (a transaction with no bytes: nothing to do)
                pend <= 1'b0; x_to <= 1'b0; x_sh <= 1'b0; x_cnt <= '0; x_us <= '0;
            end else if (on && scan_en && scan_t[11]) begin
                m_start <= 1'b1; m_imm <= 1'b1; m_txlen <= 6'd1; m_rxlen <= 6'd3; m_base <= {1'b1, 2'b00, scan_p, 2'b01}; sel <= scan_p;
                scan_run <= 1'b1; scan_t <= '0;
            end
        end
        if (m_tk && !scan_t[11] && !(m_busy || scan_run)) scan_t <= scan_t + 12'd1;
        if (m_done) begin
            if (scan_run) begin
                scan_run <= 1'b0;
                s_wr <= 1'b1; s_wr_addr <= {1'b1, 2'b00, scan_p, 2'b00}; s_wr_data <= {2'b0, m_cnt};
                present[scan_p] <= m_cnt == 6'd3;
                if (present[scan_p] != (m_cnt == 6'd3)) chg[scan_p] <= 1'b1;
                scan_p <= scan_p + 2'd1;
            end else begin
                pend <= 1'b0; x_to <= m_timeout; x_sh <= m_short; x_cnt <= m_cnt; x_us <= m_us;
            end
        end
    end
end

always_comb begin
    rd = 32'd0;
    case (a)
    4'd0: rd = 32'h534E_0001;
    4'd1: rd = {16'd0, t_first, 6'd0, scan_en, on};
    4'd2: rd = {24'd0, chg, present};
    4'd3: rd = {pend, x_to, x_sh, 1'b0, x_us, 10'd0, x_cnt};
    4'd4: rd = {25'd0, ptr};
    4'd5: rd = {24'd0, qa};
    default: ;
    endcase
end

// ---------------------------------------------------------------- the pins
wire         e = on && own;
assign en = e;
wire         raw0 = e && m_low && sel == 2'd0;
wire         raw1 = e && m_low && sel == 2'd1;
wire         raw2 = e && m_low && sel[1];
logic        r0a, r0b, r1a, r1b, r2a, r2b;
always_ff @(posedge clk) begin
    r0a <= raw0; r0b <= r0a; r1a <= raw1; r1b <= r1a; r2a <= raw2; r2b <= r2a;
end
assign p30_dir = r0a | r0b;   assign p30_oe = r0a & r0b;
assign p31_dir = r1a | r1b;   assign p31_oe = r1a & r1b;
assign b0_dir  = r2a | r2b;   assign b0_oe  = r2a & r2b;
assign b0_o    = {sel != 2'd3, 2'b11, sel != 2'd2};
endmodule
`default_nettype wire
