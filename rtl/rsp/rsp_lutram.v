// rsp_lutram: the vector registers' memory (rsp_vu) -- 32 words of 16 bytes
// in LUT RAM (MLAB): one clocked write with an enable a byte (we[m]: wd[8m +: 8]),
// the read in the cycle, at an address the memory registers itself: ra_n is
// the address of the cycle to come. On the FPGA altdpram with byte enables (a
// plain array written a byte at a time is not inferred as LUT RAM), 128 bits
// in seven MLABs; in simulation the same behaviour in plain Verilog.
`default_nettype none
module rsp_lutram (
    input  wire         clk,
    input  wire [15:0]  we,
    input  wire [4:0]   wa,
    input  wire [127:0] wd,
    input  wire [4:0]   ra_n,
    output wire [127:0] q
);
`ifdef VERILATOR
    reg [127:0] m [0:31];
    reg [4:0]   ra;
    integer i;
    always @(posedge clk) begin
        for (i = 0; i < 16; i = i + 1) if (we[i]) m[wa][8 * i +: 8] <= wd[8 * i +: 8];
        ra <= ra_n;
    end
    assign q = m[ra];
`else
    altdpram #(
        .width                              (128),
        .widthad                            (5),
        .numwords                           (32),
        .width_byteena                      (16),
        .byte_size                          (8),
        .lpm_type                           ("altdpram"),
        .ram_block_type                     ("MLAB"),
        .indata_reg                         ("INCLOCK"),
        .indata_aclr                        ("OFF"),
        .wraddress_reg                      ("INCLOCK"),
        .wraddress_aclr                     ("OFF"),
        .wrcontrol_reg                      ("INCLOCK"),
        .wrcontrol_aclr                     ("OFF"),
        .rdaddress_reg                      ("OUTCLOCK"),
        .rdaddress_aclr                     ("OFF"),
        .rdcontrol_reg                      ("UNREGISTERED"),
        .rdcontrol_aclr                     ("OFF"),
        .outdata_reg                        ("UNREGISTERED"),
        .outdata_aclr                       ("OFF"),
        .read_during_write_mode_mixed_ports ("DONT_CARE"),
        .intended_device_family             ("Cyclone V")
    ) ram (
        .inclock        (clk),
        .outclock       (clk),
        .wren           (|we),
        .byteena        (we),
        .data           (wd),
        .wraddress      (wa),
        .rdaddress      (ra_n),
        .q              (q),
        .aclr           (1'b0),
        .sclr           (1'b0),
        .inclocken      (1'b1),
        .outclocken     (1'b1),
        .rdaddressstall (1'b0),
        .wraddressstall (1'b0),
        .rden           (1'b1)
    );
`endif
endmodule
`default_nettype wire
