// A Joybus master on one open-drain wire, a byte at a time: the N64 console's side of the controller port (docs/analogizer.md,
// "The SNAC port as the console's PIF").
//
// The wire idles high (an external 1 kOhm pull-up to 3.3 V). A bit is a cell of 4 us that starts with a falling edge: a 1 is
// 1 us low and 3 us high, a 0 is 3 us low and 1 us high, most significant bit first. A command is `txlen` bytes and the console's
// stop bit (a 1 us low pulse), after which the device answers with `rxlen` bytes in the same cells, followed by its own stop bit.
// This master never drives a 1: `low` says "pull the wire low", and it is the only output; high is the pull-up.
//
// A transaction (`start`, one cycle, not while `busy`):
//   tx   the bytes come from the buffer (`rd_addr` 0.. / `rd_data`, one clock behind) or, with `imm`, the single byte `imm_byte`
//   rx   a falling edge is waited for (`t_first` us for the first, 16 us between the bits of a reply), its bit is taken 2 us later
//        (a 1 is high by then, a 0 still low), a byte at a time goes to the buffer (`wr_*`, at rx_base + index). After the last
//        byte the master waits for the wire to be quiet (high for 12 us) so the device's stop bit and the next command do not meet.
//   end  `done` pulses and `busy` falls. Results, valid until the next start:
//          rx_cnt   whole bytes received         timeout  nothing answered (rx_cnt 0 with rxlen > 0): the device is absent
//          short_   fewer bytes than rxlen, or the wire did not go quiet (a device that talks on or holds the wire low)
//          t_us     the line time from `start` to `done`, in us (saturates at 4095)
//   `tk` is the 1 us tick (62.5 clocks of 16 ns: 62 and 63 alternately), free running, for the timers around the master.
//
// The wire is sampled through two flip-flops; the edge is found with the clock's resolution, so the bit is sampled 2.0 us after
// it (less the synchroniser's three clocks), the middle of the 1 us that tells a 1 (high from 1 us) from a 0 (low until 3 us).
`default_nettype none
module an_joybus_m #(
    parameter int CLK_HZ = 62_500_000
) (
    input  wire         clk, rst,
    input  wire         start,
    input  wire         imm,
    input  wire  [7:0]  imm_byte,
    input  wire  [5:0]  txlen,              // 1 .. 63
    input  wire  [5:0]  rxlen,              // 0 .. 63
    input  wire  [6:0]  rx_base,
    input  wire  [7:0]  t_first,            // us to wait for a reply's first edge
    output logic        busy,
    output logic        done,
    output logic [5:0]  rx_cnt,
    output logic        timeout, short_,
    output logic [11:0] t_us,
    output wire         tk,
    // the buffer: the tx bytes (read) and the rx bytes (written)
    output wire  [5:0]  rd_addr,
    input  wire  [7:0]  rd_data,
    output logic        wr_en,
    output logic [6:0]  wr_addr,
    output logic [7:0]  wr_data,
    // the wire
    output logic        low,                // pull the wire low (the only way it is driven)
    input  wire         dq                  // the wire (asynchronous)
);

localparam int P0     = CLK_HZ / 1_000_000;                     // clocks of a tick: P0, and P0 + 1 every other one when the clock has a half
localparam bit ALT    = (CLK_HZ % 1_000_000) >= 250_000;
localparam int SAMP   = 2 * CLK_HZ / 1_000_000 - 3;             // clocks from the edge (as the synchroniser shows it) to the sample
localparam int BIT_TO = 16;                                     // us between the bits of a reply
localparam int QUIET  = 12;                                     // us of a quiet wire after a reply
localparam int GUARD  = 255;                                    // us after which a wire that never goes quiet is given up

typedef enum logic [2:0] { S_IDLE, S_GO, S_TX, S_WH, S_F, S_S, S_H, S_G } st_t;
st_t         st;

// ---- the 1 us tick
logic [6:0]  ph = 7'd0;
logic        alt = 1'b0;
assign tk = ph == 7'(ALT && alt ? P0 : P0 - 1);
always_ff @(posedge clk) begin
    if (tk) begin ph <= 7'd0; alt <= ~alt; end
    else ph <= ph + 7'd1;
end

// ---- the wire in
logic        dq1, dq2;
always_ff @(posedge clk) begin dq1 <= dq; dq2 <= dq1; end

// ---- the transaction
logic        a_imm;
logic [7:0]  a_ib;
logic [5:0]  a_tx, a_rx, idx;
logic [6:0]  a_base;
logic [7:0]  a_tf;
logic [7:0]  cur, nxt;
logic [2:0]  bitn;
logic [2:0]  cp;                        // us since the cell's start
logic        stop;
logic [7:0]  tm, gc;
logic [6:0]  rc;
logic [7:0]  sh;
logic [2:0]  nb;                        // bits of the byte so far
logic        part;                      // a reply is under way (a bit was taken): a failure then leaves the wire to be waited for
assign rd_addr = (st == S_IDLE || st == S_GO) ? 6'd0 : idx + 6'd1;

logic        stuck;
task automatic finish(input logic stk);
    busy <= 1'b0; done <= 1'b1; st <= S_IDLE;
    timeout <= rx_cnt == 6'd0 && a_rx != 6'd0;
    short_ <= (rx_cnt != 6'd0 && rx_cnt != a_rx) || stk;
endtask

always_ff @(posedge clk) begin
    done <= 1'b0; wr_en <= 1'b0;
    if (rst) begin
        st <= S_IDLE; busy <= 1'b0; low <= 1'b0; rx_cnt <= '0; timeout <= 1'b0; short_ <= 1'b0; t_us <= '0; stuck <= 1'b0; idx <= '0;
    end else begin
        if (busy && tk && t_us != 12'hFFF) t_us <= t_us + 12'd1;
        case (st)
        S_IDLE: if (start) begin
            a_imm <= imm; a_ib <= imm_byte; a_tx <= txlen; a_rx <= rxlen; a_base <= rx_base; a_tf <= t_first;
            busy <= 1'b1; rx_cnt <= '0; timeout <= 1'b0; short_ <= 1'b0; stuck <= 1'b0; t_us <= '0; part <= 1'b0;
            st <= S_GO;
        end
        S_GO: if (tk) begin                         // the first cell starts on a tick
            cur <= a_imm ? a_ib : rd_data; idx <= '0; bitn <= 3'd7; cp <= 3'd0; stop <= 1'b0; low <= 1'b1; st <= S_TX;
        end
        S_TX: if (tk) begin
            cp <= cp + 3'd1;
            if (stop) begin
                if (cp == 3'd0) begin                    // the stop bit: 1 us low, then let go
                    low <= 1'b0; tm <= 8'd0; gc <= 8'd0;
                    st <= a_rx == 6'd0 ? S_G : S_WH;
                end
            end else begin
                if (cp == 3'd0 && bitn == 3'd7) nxt <= rd_data;       // (the next byte, whose address has been up for a byte's time)
                if (cp == 3'd0 && cur[7]) low <= 1'b0;                // a 1: 1 us low
                if (cp == 3'd2 && !cur[7]) low <= 1'b0;               // a 0: 3 us low
                if (cp == 3'd3) begin                                 // the cell ends: the next starts
                    cp <= 3'd0; low <= 1'b1;
                    if (bitn == 3'd0) begin
                        if (idx + 6'd1 == a_tx) stop <= 1'b1;
                        else begin idx <= idx + 6'd1; cur <= nxt; bitn <= 3'd7; end
                    end else begin bitn <= bitn - 3'd1; cur <= {cur[6:0], 1'b0}; end
                end
            end
        end
        // ---- the reply: the wire high again, a falling edge, the sample 2 us on, the wire high again ...
        S_WH: begin
            if (dq2) begin st <= S_F; tm <= 8'd0; nb <= 3'd0; end
            else if (tk) begin tm <= tm + 8'd1; if (tm == 8'd255) begin stuck <= 1'b1; finish(1'b1); end end
        end
        S_F: begin
            if (!dq2) begin st <= S_S; rc <= 7'd0; end
            else if (tk) begin
                tm <= tm + 8'd1;
                if (tm + 8'd1 >= (part || rx_cnt != 6'd0 ? 8'(BIT_TO) : a_tf)) begin
                    if (part || rx_cnt != 6'd0) begin st <= S_G; tm <= 8'd0; gc <= 8'd0; end    // (a reply that stopped short: wait for quiet)
                    else finish(stuck);                                                        // (nothing answered)
                end
            end
        end
        S_S: begin
            rc <= rc + 7'd1;
            if (rc == 7'(SAMP)) begin
                sh <= {sh[6:0], dq2}; nb <= nb + 3'd1; part <= 1'b1; tm <= 8'd0;
                if (nb == 3'd7) begin
                    wr_en <= 1'b1; wr_addr <= a_base + {1'b0, rx_cnt}; wr_data <= {sh[6:0], dq2}; rx_cnt <= rx_cnt + 6'd1; part <= 1'b0;
                    if (rx_cnt + 6'd1 == a_rx) begin st <= S_G; gc <= 8'd0; end else st <= S_H;
                end else st <= S_H;
            end
        end
        S_H: begin
            if (dq2) st <= S_F;
            else if (tk) begin
                tm <= tm + 8'd1;
                if (tm + 8'd1 >= 8'(BIT_TO)) begin st <= S_G; tm <= 8'd0; gc <= 8'd0; end
            end
        end
        S_G: if (tk) begin                                // the wire quiet for QUIET us
            gc <= gc + 8'd1;
            if (dq2) begin
                tm <= tm + 8'd1;
                if (tm + 8'd1 >= 8'(QUIET)) finish(stuck);
            end else tm <= 8'd0;
            if (gc == 8'(GUARD)) begin stuck <= 1'b1; finish(1'b1); end
        end
        default: st <= S_IDLE;
        endcase
    end
end
endmodule
`default_nettype wire
