// an_snac_pif on the four wires of the SNAC port, with a device on each (jb_dev.sv) and the pins as core_top.sv drives them: a wire
// is pulled low by the master when its translator is outward and the FPGA's driver is on; ports 3 and 4 share bank 0, whose bits
// are driven 1 together with the low pulse of the other (checked: never against a device pulling low).
`default_nettype none
module snac_top (
    input  wire         clk, rst,
    input  wire         sel_wr, sel_rd,
    input  wire  [3:0]  a,
    input  wire  [31:0] wd,
    output wire  [31:0] rd,
    input  wire         own,
    input  wire  [15:0] k_kind,                 // 4 bits a port
    input  wire  [63:0] k_cell,                 // 16 bits a port
    input  wire  [63:0] k_delay,
    input  wire  [63:0] k_btn,
    input  wire  [63:0] k_stick,
    output wire  [3:0]  wire_l,                 // the wires' levels
    output wire  [3:0]  mlow,                   // the master pulls it low
    output wire  [3:0]  dlow,                   // a device pulls it low
    output wire         en,
    output wire  [63:0] d_ncmds,
    output wire  [63:0] d_nwr,
    output wire  [63:0] d_wraddr,
    output wire  [31:0] d_lastcmd,
    output reg   [31:0] drive_hi,               // cycles in which the master drove a 1 on a bank 0 bit
    output reg   [31:0] conflict,               // ... of them against a device pulling that wire low
    output reg   [31:0] glitch30, glitch31, glitchb0,     // cycles with the translator outward and the driver off
    output reg   [31:0] bad_oe                  // the driver on with the translator not outward
);
wire p30_dir, p30_oe, p31_dir, p31_oe, b0_dir, b0_oe;
wire [3:0] b0_o;
wire [3:0] dq_in = wire_l;
an_snac_pif pif (.clk, .rst, .sel_wr, .sel_rd, .a, .wd, .rd, .own, .en, .p30_dir, .p30_oe, .p31_dir, .p31_oe, .b0_dir, .b0_oe, .b0_o, .dq_in);
assign mlow[0] = p30_dir & p30_oe;
assign mlow[1] = p31_dir & p31_oe;
assign mlow[2] = b0_dir & b0_oe & ~b0_o[0];
assign mlow[3] = b0_dir & b0_oe & ~b0_o[3];
wire hi3 = b0_dir & b0_oe & b0_o[0], hi4 = b0_dir & b0_oe & b0_o[3];
assign wire_l = ~(mlow | dlow);
for (genvar p = 0; p < 4; p = p + 1) begin : dv
    wire [31:0] nc, nw;
    jb_dev d (.clk, .line(wire_l[p]), .low(dlow[p]), .kind(k_kind[4*p +: 4]), .cell_ns(k_cell[16*p +: 16]), .delay_us(k_delay[16*p +: 16]),
              .btn(k_btn[16*p +: 16]), .stick(k_stick[16*p +: 16]), .n_cmds(nc), .n_wr(nw),
              .last_wr_addr(d_wraddr[16*p +: 16]), .last_cmd(d_lastcmd[8*p +: 8]));
    assign d_ncmds[16*p +: 16] = nc[15:0];
    assign d_nwr[16*p +: 16] = nw[15:0];
end
always @(posedge clk) begin
    if (rst) begin drive_hi <= 0; conflict <= 0; glitch30 <= 0; glitch31 <= 0; glitchb0 <= 0; bad_oe <= 0; end
    else begin
        if (hi3 | hi4) drive_hi <= drive_hi + 1;
        if ((hi3 & dlow[2]) | (hi4 & dlow[3])) conflict <= conflict + 1;
        if (p30_dir & ~p30_oe) glitch30 <= glitch30 + 1;
        if (p31_dir & ~p31_oe) glitch31 <= glitch31 + 1;
        if (b0_dir & ~b0_oe) glitchb0 <= glitchb0 + 1;
        if ((p30_oe & ~p30_dir) | (p31_oe & ~p31_dir) | (b0_oe & ~b0_dir)) bad_oe <= bad_oe + 1;
    end
end
endmodule
`default_nettype wire
