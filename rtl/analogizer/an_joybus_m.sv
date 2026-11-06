// A Joybus master on one open-drain wire, a byte at a time: the N64 console's side of the controller port (docs/analogizer.md,
// "The SNAC port as the console's PIF").
//
// The wire idles high (an external 1 kOhm pull-up to 3.3 V). A bit is a cell of 4 us that starts with a falling edge: a 1 is
// 1 us low and 3 us high, a 0 is 3 us low and 1 us high, most significant bit first. A command is `txlen` bytes and the console's
// stop bit (a 1 us low pulse), after which the device answers with `rxlen` bytes in the same cells, followed by its own stop bit.
// This master never drives a 1: `low` says "pull the wire low", and it is the only output; high is the pull-up.
//
// A transaction (`start`, one cycle, not while `busy`; txlen, rxlen, imm, rx_base and t_first stay as they are until `done`):
//   tx   the bytes come from the buffer (`rd_addr` 0.. / `rd_data`, one clock behind) or, with `imm`, the single byte 0x00
//   rx   a falling edge is waited for (`t_first` us for the first, 16 us between the bits of a reply), its bit is taken 2 us later
//        (a 1 is high by then, a 0 still low), a byte at a time goes to the buffer (`wr_*`, at rx_base + index). After the last
//        byte the master waits for the wire to be quiet (high for 12 us) so the device's stop bit and the next command do not meet.
//   end  `done` pulses and `busy` falls. Results, valid until the next start:
//          rx_cnt   whole bytes received         timeout  nothing answered (rx_cnt 0 with rxlen > 0): the device is absent
//          short_   fewer bytes than rxlen, or the wire did not go quiet (a device that talks on or holds the wire low)
//   `tk` is the 1 us tick (62.5 clocks of 16 ns: 62 and 63 alternately), restarted at each falling edge of a reply so that the
//   sample is 2 ticks after it.
//
// The wire is sampled through two flip-flops; the edge is found with the clock's resolution, so the bit is sampled 2.05 us after
// it, the middle of the 1 us that tells a 1 (high from 1 us) from a 0 (low until 3 us).
`default_nettype none
module an_joybus_m #(
    parameter int CLK_HZ = 62_500_000
) (
    input  wire         clk, rst,
    input  wire         start,
    input  wire         imm,
    input  wire  [5:0]  txlen,              // 1 .. 63
    input  wire  [5:0]  rxlen,              // 0 .. 63
    input  wire  [6:0]  rx_base,
    input  wire  [7:0]  t_first,            // us to wait for a reply's first edge
    output logic        busy,
    output logic        done,
    output wire  [5:0]  rx_cnt,
    output logic        timeout, short_,
    output wire         tk,
    // the buffer: the tx bytes (read) and the rx bytes (written)
    output wire  [5:0]  rd_addr,
    input  wire  [7:0]  rd_data,
    output logic        wr_en,
    output wire  [6:0]  wr_addr,
    output wire  [7:0]  wr_data,
    // the wire
    output logic        low,                // pull the wire low (the only way it is driven)
    input  wire         dq                  // the wire (asynchronous)
);

localparam int P0     = CLK_HZ / 1_000_000;                     // clocks of a tick: P0, and P0 + 1 every other one when the clock has a half
localparam bit ALT    = (CLK_HZ % 1_000_000) >= 250_000;
localparam int BIT_TO = 16;                                     // us between the bits of a reply
localparam int QUIET  = 12;                                     // us of a quiet wire after a reply

typedef enum logic [2:0] { S_IDLE, S_GO, S_TX, S_WH, S_F, S_S, S_H, S_G } st_t;
st_t         st = S_IDLE;

// ---- the wire in
logic        dq1, dq2;
always_ff @(posedge clk) begin dq1 <= dq; dq2 <= dq1; end

logic [6:0]  ph = 7'd0;
logic        alt = 1'b0;
assign tk = ph == 7'(ALT && alt ? P0 : P0 - 1);

logic [5:0]  n;                         // the tx byte being sent, then the rx bytes received
logic [7:0]  cur;
logic [2:0]  bitn;                      // the tx bit (7 down to 0)
logic [1:0]  cp;                        // us since the cell's start
logic        stop;
logic [7:0]  tm;                        // us waited
logic [3:0]  q;                         // us of a quiet wire so far
logic [7:0]  sh;
logic [2:0]  nb;                        // bits of the rx byte so far
logic        stuck;
assign rx_cnt = n;
assign rd_addr = (st == S_IDLE || st == S_GO) ? 6'd0 : n + 6'd1;
assign wr_addr = rx_base + {1'b0, n};
assign wr_data = sh;

task automatic finish(input logic stk);
    busy <= 1'b0; done <= 1'b1; st <= S_IDLE;
    timeout <= n == 6'd0 && rxlen != 6'd0;
    short_ <= (n != 6'd0 && n != rxlen) || stk;
endtask
// a reply that stopped short, or a bit that never came: if a reply was under way the wire is waited for, else it is the end
task automatic gave_up;
    if (nb != 3'd0 || n != 6'd0) begin st <= S_G; tm <= 8'd0; q <= 4'd0; end
    else finish(stuck);
endtask

always_ff @(posedge clk) begin
    done <= 1'b0; wr_en <= 1'b0;
    if (tk) begin ph <= 7'd0; alt <= ~alt; end else ph <= ph + 7'd1;
    if (rst) begin
        st <= S_IDLE; busy <= 1'b0; low <= 1'b0; timeout <= 1'b0; short_ <= 1'b0; stuck <= 1'b0; n <= '0; nb <= '0;
    end else begin
        if (wr_en) n <= n + 6'd1;
        case (st)
        S_IDLE: if (start) begin
            busy <= 1'b1; n <= '0; nb <= '0; timeout <= 1'b0; short_ <= 1'b0; stuck <= 1'b0;
            st <= S_GO;
        end
        S_GO: if (tk) begin                         // the first cell starts on a tick
            cur <= imm ? 8'h00 : rd_data; bitn <= 3'd7; cp <= 2'd0; stop <= 1'b0; low <= 1'b1; st <= S_TX;
        end
        S_TX: if (tk) begin
            cp <= cp + 2'd1;
            if (stop) begin
                if (cp == 2'd0) begin                    // the stop bit: 1 us low, then let go
                    low <= 1'b0; tm <= 8'd0; q <= 4'd0; n <= '0;
                    st <= rxlen == 6'd0 ? S_G : S_WH;
                end
            end else begin
                if (cp == 2'd0 && cur[7]) low <= 1'b0;                // a 1: 1 us low
                if (cp == 2'd2 && !cur[7]) low <= 1'b0;               // a 0: 3 us low
                if (cp == 2'd3) begin                                 // the cell ends: the next starts
                    low <= 1'b1;
                    if (bitn == 3'd0) begin
                        if (n + 6'd1 == txlen) stop <= 1'b1;
                        else begin n <= n + 6'd1; cur <= rd_data; bitn <= 3'd7; end    // (rd_data: the next byte, whose address has been up since the byte began)
                    end else begin bitn <= bitn - 3'd1; cur <= {cur[6:0], 1'b0}; end
                end
            end
        end
        // ---- the reply: the wire high again, a falling edge, the sample 2 us on, the wire high again ...
        S_WH: begin
            if (dq2) begin st <= S_F; tm <= 8'd0; end
            else if (tk) begin tm <= tm + 8'd1; if (tm == 8'd255) begin stuck <= 1'b1; finish(1'b1); end end
        end
        S_F: begin
            if (!dq2) begin st <= S_S; ph <= 7'd0; alt <= 1'b0; tm <= 8'd0; end       // (the ticks restart at the edge)
            else if (tk) begin
                tm <= tm + 8'd1;
                if (tm + 8'd1 >= (nb != 3'd0 || n != 6'd0 ? 8'(BIT_TO) : t_first)) gave_up();
            end
        end
        S_S: if (tk) begin
            tm <= tm + 8'd1;
            if (tm == 8'd1) begin                                                   // 2 ticks after the edge
                sh <= {sh[6:0], dq2}; nb <= nb + 3'd1; tm <= 8'd0;
                if (nb == 3'd7) begin
                    wr_en <= 1'b1;
                    if (n + 6'd1 == rxlen) begin st <= S_G; q <= 4'd0; end else st <= S_H;
                end else st <= S_H;
            end
        end
        S_H: begin
            if (dq2) st <= S_F;
            else if (tk) begin
                tm <= tm + 8'd1;
                if (tm + 8'd1 >= 8'(BIT_TO)) gave_up();
            end
        end
        S_G: if (tk) begin                                // the wire quiet for QUIET us (and given up after 255)
            tm <= tm + 8'd1;
            if (dq2) begin
                q <= q + 4'd1;
                if (q + 4'd1 >= 4'(QUIET)) finish(stuck);
            end else q <= 4'd0;
            if (tm == 8'd255) begin stuck <= 1'b1; finish(1'b1); end
        end
        default: st <= S_IDLE;
        endcase
    end
end
endmodule
`default_nettype wire
