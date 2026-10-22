// The game CPU on a clock of its own (MIRLO_CPU_CLK: docs/mips.md): its
// memory port -- what mips_sys asks of the SDRAM and of the devices -- taken
// across to the SoC's clock, in port_n64sys's place.
//
// The two clocks are outputs of one PLL, the CPU's a ratio of the SoC's
// (3/2: their rising edges meet every two cycles of the SoC's and are never
// closer than a sixth of that, 5.3 ns; 4/3: every three, 4.0 ns). Nothing
// here is asynchronous: every path from one clock to the other is from a
// register straight to a register, taken at every edge, and the timing
// analyser checks each at the closest pair of edges. What makes the crossing
// work is only that neither side expects the other to see a value for
// exactly one of its own cycles:
//
//   clk_c -> clk  values that stay until answered. A read or a device access
//                 is put in the rq_* registers and announced by turning rq_t
//                 over; the write pointer of the FIFO below only ever counts
//                 up.
//   clk -> clk_c  values that last one cycle of clk, which the faster clk_c
//                 samples once or twice. A word read from the SDRAM comes
//                 with its number's low bit (r_seq), so the second sample is
//                 known for what it is; a device's acknowledge is taken once,
//                 the request it answers being over with it.
//
// The stores are not in the CPU's write buffer here (mips_core's XWB): each
// leaves the CPU as it is done and goes through a FIFO written on clk_c and
// read on clk. The arbiter gets the SDRAM's one after the other, with no gap
// while there are any (a run: a write among another port's reads costs the
// bus its turn), begun with four waiting, when the oldest has waited, or when
// the CPU asks for anything else. A device's store goes to the device port
// from the FIFO's head, in its turn. The order the CPU asked in is kept: a
// read or a device's read is begun only when every store before it has gone
// to the arbiter -- what a device access sets off (a DMA, a task) may read
// that word.
`default_nettype none

module cpu_port #(
    parameter int PORT = 1                  // the CPU's port of the arbiter (the tag its read data comes with)
) (
    // ---- the CPU's side
    input  wire         clk_c, rst_c,
    // the stores (mips_core's st_*)
    input  wire         st_valid, st_dev,
    input  wire  [31:0] st_addr, st_data,
    input  wire  [3:0]  st_strb,
    output wire         st_full,
    output wire         st_done,            // a device's store is done (the CPU waits for no other then)
    // SDRAM reads (mips_sys's s_*): byte addresses; a word, or the 8 of a line, back as s_rvalid
    input  wire         s_req, s_line,
    input  wire  [31:0] s_addr,
    output wire         s_rvalid,
    output wire  [31:0] s_rdata,
    // a device's read (mips_sys's x_*): held until c_x_ack, the data with it
    input  wire         c_x_req,
    input  wire  [31:0] c_x_addr,
    output wire         c_x_ack,
    output wire  [31:0] c_x_rdata,
    output logic        c_irq, c_irq_mon,
    // ---- the SoC's side
    input  wire         clk, rst,
    output wire         p_valid, p_we,
    output wire  [23:0] p_addr,
    output wire  [31:0] p_wdata,
    output wire  [3:0]  p_be,
    input  wire         p_ready,
    input  wire  [31:0] p_rdata,
    input  wire         rd_next,            // sdr_ctrl's: a read word is on p_rdata in the next cycle,
    input  wire  [2:0]  rd_next_tag,        // for the port of this tag
    output wire         x_req, x_we,        // the device port, as mips_sys gave it: held until x_ack
    output wire  [31:0] x_addr, x_wdata,
    output wire  [3:0]  x_strb,
    input  wire         x_ack,
    input  wire  [31:0] x_rdata,
    input  wire         irq, irq_mon
);

// ---------------------------------------------------------------- the stores' FIFO
localparam int FA = 4;                      // 16 words
logic [66:0]  wf [1 << FA];                 // {a device's, strobes, data, address[31:2]}
logic [FA:0]  wp;                           // clk_c
logic [FA:0]  rp, wp_s;                     // clk
logic [FA:0]  rp_c;                         // clk_c
logic         dv_out;                       // clk_c: a device's store is in it (the CPU waits for that one)
logic         c_xack;
// (registered: full when the FIFO may be, at the next edge, with a push -- the CPU waits one entry early at worst)
logic         full_q;
wire  [FA:0]  occ1 = wp + 1'b1 - rp_c;
always_ff @(posedge clk_c) full_q <= rst_c ? 1'b0 : occ1[FA];
assign st_full = full_q;
always_ff @(posedge clk_c) begin
    if (st_valid) wf[wp[FA-1:0]] <= {st_dev, st_strb, st_data, st_addr[31:2]};
    if (rst_c) begin wp <= '0; rp_c <= '0; dv_out <= 1'b0; end
    else begin
        if (st_valid) wp <= wp + 1'b1;
        rp_c <= rp;
        if (st_valid && st_dev) dv_out <= 1'b1; else if (c_xack) dv_out <= 1'b0;
    end
end

// ---------------------------------------------------------------- a read, of the SDRAM or of a device: clk_c
logic        q_busy;                        // one is out
logic        rq_t, rq_dev, rq_line;
logic [31:0] rq_addr;
logic [3:0]  q_got;                         // a read's words so far
logic        r_rv, r_seq;                   // clk: a word for the CPU on p_rdata, its number's low bit
logic        c_rv, c_seq;                   // the SoC's side as clk_c's last edge found it (and c_xack)
logic [31:0] c_data, c_xdata;
assign s_rvalid  = q_busy && !rq_dev && c_rv && c_seq == q_got[0];
assign s_rdata   = c_data;
assign c_x_ack   = q_busy && rq_dev && c_xack;
assign c_x_rdata = c_xdata;
assign st_done   = c_xack;
always_ff @(posedge clk_c) begin
    c_rv <= r_rv; c_seq <= r_seq; c_data <= p_rdata; c_xack <= x_ack; c_xdata <= x_rdata; c_irq <= irq; c_irq_mon <= irq_mon;
    if (rst_c) begin q_busy <= 1'b0; rq_t <= 1'b0; end
    else if (!q_busy) begin
        if (s_req || c_x_req) begin
            q_busy <= 1'b1; rq_t <= !rq_t; q_got <= 4'd0;
            rq_dev <= c_x_req; rq_line <= s_line; rq_addr <= c_x_req ? c_x_addr : s_addr;
        end
    end else begin
        // (the answer is given in this cycle: the request it answers is gone in the next)
        if (c_x_ack) q_busy <= 1'b0;
        if (s_rvalid) begin q_got <= q_got + 4'd1; if (q_got == (rq_line ? 4'd7 : 4'd0)) q_busy <= 1'b0; end
    end
end

// ---------------------------------------------------------------- the SoC's side
logic        t_s, seen;                     // rq_t as the last edge found it; that of the last read done
logic        a_dev, a_line;                 // rq_*, likewise: they stay while a read is out
logic [31:0] a_addr;
logic [3:0]  issued, got;
logic        w_valid;                       // the FIFO's head is in w_head
logic [66:0] w_head;
logic        w_run;                         // the SDRAM's stores are going out
logic [4:0]  w_age;
logic        dv_s;                          // dv_out as the last edge found it
wire         w_dev = w_head[66];
wire         a_out = t_s != seen;           // a read is out
wire  [FA:0] w_n = wp_s - rp;
wire         w_pop = w_valid && (w_dev ? x_ack : w_run && p_ready);
wire         a_go = a_out && !w_valid && rp == wp_s;            // and every store before it has gone
wire         r_ask = a_go && !a_dev && issued != (a_line ? 4'd8 : 4'd1);
wire         r_mine = rd_next && rd_next_tag == 3'(PORT);
assign p_valid = (w_valid && !w_dev && w_run) || r_ask;
assign p_we    = w_valid;
// (a line's address is the line's first word -- mips_core asks for {pa[31:5], 5'd0} -- and a single word is asked for with issued
// 0: the read's word is the address's with issued in its low three bits, no adder)
assign p_addr  = w_valid ? w_head[23:0] : {a_addr[25:5], a_addr[4:2] | issued[2:0]};
assign p_wdata = w_head[61:30];
assign p_be    = w_head[65:62];
assign x_req   = (w_valid && w_dev) || (a_go && a_dev);
assign x_we    = w_valid;
assign x_addr  = w_valid ? {w_head[29:0], 2'b00} : a_addr;
assign x_wdata = w_head[61:30];
assign x_strb  = w_head[65:62];
always_ff @(posedge clk) begin
    r_rv <= r_mine;
    t_s <= rq_t; a_dev <= rq_dev; a_line <= rq_line; a_addr <= rq_addr;
    wp_s <= wp; dv_s <= dv_out;
    if (rst) begin
        rp <= '0; wp_s <= '0; w_valid <= 1'b0; t_s <= 1'b0; seen <= 1'b0; r_rv <= 1'b0; r_seq <= 1'b0;
        issued <= 4'd0; got <= 4'd0; w_run <= 1'b0; w_age <= '0;
    end else begin
        // the FIFO's head
        if ((!w_valid || w_pop) && rp != wp_s) begin w_head <= wf[rp[FA-1:0]]; rp <= rp + 1'b1; w_valid <= 1'b1; end
        else if (w_pop) w_valid <= 1'b0;
        // a run of the SDRAM's stores: to the last one waiting
        if (!w_valid && rp == wp_s) begin w_run <= 1'b0; w_age <= '0; end
        else begin
            if (!w_age[4]) w_age <= w_age + 5'd1;
            if (w_n >= (FA + 1)'(3) || w_age[4] || a_out || dv_s) w_run <= 1'b1;
        end
        // a read: its words asked for and counted, or a device's acknowledge awaited
        if (!a_out) begin issued <= 4'd0; got <= 4'd0; r_seq <= 1'b0; end
        else begin
            if (r_ask && p_ready) issued <= issued + 4'd1;
            if (r_rv) begin r_seq <= !r_seq; got <= got + 4'd1; if (got + 4'd1 == (a_line ? 4'd8 : 4'd1)) seen <= t_s; end
            if (a_go && a_dev && x_ack) seen <= t_s;
        end
    end
end

endmodule
`default_nettype wire
