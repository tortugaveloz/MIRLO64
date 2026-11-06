// A Joybus device for the benches (sim/analogizer/snac, sim/mips_soc): a controller, with or without a pak, and the ways a real
// one misbehaves. It sees the wire and pulls it low (open drain: `low`); it decodes the console's bits by the width of their low
// pulse (under 2 us a 1), counts the command's bytes by what the command is, takes the console's stop bit and answers after
// `delay_us`. One clock is 16 ns (62.5 MHz); times are in ns.
//   kind 0 nothing there        1 controller, no pak     2 controller with a pak (32 KiB, 0x02 / 0x03)
//        3 answers the status command with 2 bytes only (a short reply)
//        4 holds the wire low for 400 us at the first edge it sees (a shorted line)
//        5 controller that talks on: two extra bytes after every reply
//        6 as 2, with a 300 ns glitch on the wire 6 us after each of its replies (a noisy line)
`default_nettype none
module jb_dev (
    input  wire         clk,
    input  wire         line,
    output reg          low,
    input  wire  [3:0]  kind,
    input  wire  [15:0] cell_ns,            // the device's bit cell (nominal 4000)
    input  wire  [15:0] delay_us,           // before the reply
    input  wire  [15:0] btn,
    input  wire  [15:0] stick,
    output reg   [31:0] n_cmds,             // commands answered
    output reg   [31:0] n_wr,               // pak writes taken
    output reg   [15:0] last_wr_addr,
    output reg   [7:0]  last_cmd
);
reg [7:0]  cmdb [0:63];
reg [7:0]  resp [0:63];
reg [7:0]  pmem [0:32767];
integer    i0;
initial begin
    for (i0 = 0; i0 < 32768; i0 = i0 + 1) pmem[i0] = 8'((i0 * 37 + 11) & 255);
    low = 0; n_cmds = 0; n_wr = 0; last_wr_addr = 0; last_cmd = 0;
end
function automatic [7:0] crc_byte(input [7:0] c0, input [7:0] d);
    reg [7:0] c; integer k;
    begin
        c = c0;
        for (k = 7; k >= 0; k = k - 1) c = {c[6:0], d[k]} ^ (c[7] ? 8'h85 : 8'h00);
        crc_byte = c;
    end
endfunction
function automatic [7:0] crc_end(input [7:0] c0);
    reg [7:0] c; integer k;
    begin
        c = c0;
        for (k = 0; k < 8; k = k + 1) c = {c[6:0], 1'b0} ^ (c[7] ? 8'h85 : 8'h00);
        crc_end = c;
    end
endfunction
function automatic integer cmd_len(input [7:0] c);
    case (c)
    8'h02: cmd_len = 3;
    8'h03: cmd_len = 35;
    default: cmd_len = 1;
    endcase
endfunction

typedef enum logic [3:0] { M_RX, M_WAIT, M_TXL, M_TXH, M_STOP, M_POST, M_HOLD } mst_t;
mst_t       st = M_RX;
reg         line_d = 1;
reg  [31:0] t = 0, tfall = 0, tlast = 0, tmark = 0, tgo = 0;
reg  [7:0]  sh = 0;
integer     nbits = 0, nbytes = 0, elen = 0, rlen = 0, ti = 0, tb = 7, lowdur = 0, k0, a0;
reg         await_stop = 0, rbit = 0, glitch = 0;
reg  [7:0]  crc;
reg  [15:0] addr;
wire        fall = line_d && !line, rise = !line_d && line;
wire        pak = kind == 4'd2 || kind == 4'd6;
always @(posedge clk) begin
    line_d <= line; t <= t + 32'd16;
    case (st)
    M_RX: if (kind != 4'd0) begin
        if (fall) begin tfall <= t; tlast <= t; if (kind == 4'd4) begin low <= 1; tmark <= t; st <= M_HOLD; end end
        if (rise) begin
            tlast <= t;
            rbit = (t - tfall) < 32'd2000;
            if (await_stop) begin                                // the console's stop bit: build the reply
                await_stop <= 0; rlen = 0; last_cmd <= cmdb[0];
                case (cmdb[0])
                8'h00, 8'hFF: begin resp[0] = 8'h05; resp[1] = 8'h00; resp[2] = pak ? 8'h01 : 8'h02; rlen = kind == 4'd3 ? 2 : 3; end
                8'h01: begin resp[0] = btn[15:8]; resp[1] = btn[7:0]; resp[2] = stick[15:8]; resp[3] = stick[7:0]; rlen = 4; end
                8'h02: if (pak) begin
                    addr = {cmdb[1], cmdb[2] & 8'hE0};
                    crc = 0;
                    for (k0 = 0; k0 < 32; k0 = k0 + 1) begin resp[k0] = pmem[(addr + k0) & 16'h7FFF]; crc = crc_byte(crc, resp[k0]); end
                    resp[32] = crc_end(crc); rlen = 33;
                end
                8'h03: if (pak) begin
                    addr = {cmdb[1], cmdb[2] & 8'hE0};
                    crc = 0;
                    for (k0 = 0; k0 < 32; k0 = k0 + 1) begin pmem[(addr + k0) & 16'h7FFF] = cmdb[3 + k0]; crc = crc_byte(crc, cmdb[3 + k0]); end
                    resp[0] = crc_end(crc); rlen = 1; n_wr <= n_wr + 1; last_wr_addr <= addr;
                end
                default: ;
                endcase
                if (kind == 4'd5 && rlen != 0) begin resp[rlen] = 8'hAA; resp[rlen + 1] = 8'h55; rlen = rlen + 2; end
                nbits = 0; nbytes = 0; sh = 0;
                if (rlen != 0) begin tgo <= t + {16'd0, delay_us} * 1000; st <= M_WAIT; ti = 0; tb = 7; end
            end else begin
                sh = {sh[6:0], rbit}; nbits = nbits + 1;
                if (nbits == 8) begin
                    cmdb[nbytes] = sh; nbytes = nbytes + 1; nbits = 0;
                    if (nbytes == 1) elen = cmd_len(sh);
                    if (nbytes == elen) await_stop <= 1;
                end
            end
        end
        if (!fall && !rise && (t - tlast) > 32'd30000 && (nbits != 0 || nbytes != 0 || await_stop)) begin   // a half-heard command is given up
            nbits = 0; nbytes = 0; await_stop <= 0;
        end
    end
    M_WAIT: if (t >= tgo) begin
        rbit = resp[0][7]; lowdur = rbit ? (cell_ns / 4) : (3 * cell_ns / 4); low <= 1; tmark <= t; st <= M_TXL;
    end
    M_TXL: if ((t - tmark) >= lowdur) begin low <= 0; st <= M_TXH; end
    M_TXH: if ((t - tmark) >= cell_ns) begin
        if (tb == 0) begin tb = 7; ti = ti + 1; end else tb = tb - 1;
        if (ti == rlen) begin low <= 1; tmark <= t; st <= M_STOP; end
        else begin rbit = resp[ti][tb]; lowdur = rbit ? (cell_ns / 4) : (3 * cell_ns / 4); low <= 1; tmark <= t; st <= M_TXL; end
    end
    M_STOP: if ((t - tmark) >= (cell_ns / 4)) begin low <= 0; n_cmds <= n_cmds + 1; st <= M_POST; tmark <= t; glitch = 0; end
    M_POST: begin
        if (kind == 4'd6 && !glitch && (t - tmark) >= 32'd6000) begin low <= 1; glitch = 1; tgo <= t; end
        if (glitch && low && (t - tgo) >= 32'd300) low <= 0;
        if (line && (t - tmark) >= 32'd9000 && !low) begin st <= M_RX; tlast <= t; end
    end
    M_HOLD: if ((t - tmark) >= 32'd400000) begin low <= 0; st <= M_POST; tmark <= t; glitch = 1; end
    default: st <= M_RX;
    endcase
end
endmodule
`default_nettype wire
