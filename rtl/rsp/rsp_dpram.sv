// rsp_dpram: DMEM's memory -- 4 KiB as four banks of 256 words, a word of
// each 16-byte line in each (bank k holds the line's bytes 4k .. 4k + 3), so
// any 16 bytes from a word's start are one access: each bank has its own row
// (a_row[k]). A write enable per byte, by the byte's place in its line
// (a_we[b]: byte b, address mod 16 = b), the data the same way (byte b in
// a_dat_w[127 - 8b -: 8]); the read data, in that order too, the cycle after
// the row. Port b reads a word (the RDP's XBUS reads), the cycle after its
// address. On the FPGA each bank is altsyncram in M10K, true dual port
// (inferring one with byte enables is not dependable); in simulation the
// same behaviour in plain SystemVerilog, one array of 1024 words in address
// order (the testbenches read DMEM there).
`default_nettype none

module rsp_dpram (
    input  wire          clk,
    input  wire  [31:0]  a_row,             // bank k's row in [8k +: 8]
    input  wire  [15:0]  a_we,
    input  wire  [127:0] a_dat_w,
    output logic [127:0] a_dat_r,
    input  wire  [9:0]   b_adr,
    output logic [31:0]  b_dat_r
);
`ifdef VERILATOR
    logic [31:0] mem [0:1023] /* verilator public_flat_rd */;
    always_ff @(posedge clk) begin
        for (int k = 0; k < 4; k++) begin
            for (int i = 0; i < 4; i++)
                if (a_we[4 * k + i]) mem[{a_row[8 * k +: 8], 2'(k)}][31 - 8 * i -: 8] <= a_dat_w[127 - 32 * k - 8 * i -: 8];
            a_dat_r[127 - 32 * k -: 32] <= mem[{a_row[8 * k +: 8], 2'(k)}];
        end
        b_dat_r <= mem[b_adr];
    end
`else
    logic [1:0]  b_k;
    logic [31:0] qb [4];
    always_ff @(posedge clk) b_k <= b_adr[1:0];
    assign b_dat_r = qb[b_k];
    genvar k;
    generate for (k = 0; k < 4; k++) begin : bank
        wire [3:0] we = {a_we[4 * k], a_we[4 * k + 1], a_we[4 * k + 2], a_we[4 * k + 3]};   // ([3]: the word's first byte)
        altsyncram #(
            .operation_mode                     ("BIDIR_DUAL_PORT"),
            .width_a                            (32),
            .widthad_a                          (8),
            .numwords_a                         (256),
            .width_b                            (32),
            .widthad_b                          (8),
            .numwords_b                         (256),
            .width_byteena_a                    (4),
            .width_byteena_b                    (1),
            .byte_size                          (8),
            .outdata_reg_a                      ("UNREGISTERED"),
            .outdata_reg_b                      ("UNREGISTERED"),
            .address_reg_b                      ("CLOCK0"),
            .indata_reg_b                       ("CLOCK0"),
            .wrcontrol_wraddress_reg_b          ("CLOCK0"),
            .outdata_aclr_a                     ("NONE"),
            .outdata_aclr_b                     ("NONE"),
            .clock_enable_input_a               ("BYPASS"),
            .clock_enable_input_b               ("BYPASS"),
            .clock_enable_output_a              ("BYPASS"),
            .clock_enable_output_b              ("BYPASS"),
            .read_during_write_mode_mixed_ports ("DONT_CARE"),
            .read_during_write_mode_port_a      ("NEW_DATA_NO_NBE_READ"),
            .read_during_write_mode_port_b      ("NEW_DATA_NO_NBE_READ"),
            .power_up_uninitialized             ("FALSE"),
            .ram_block_type                     ("M10K"),
            .intended_device_family             ("Cyclone V"),
            .lpm_type                           ("altsyncram")
        ) ram (
            .clock0         (clk),
            .address_a      (a_row[8 * k +: 8]),
            .data_a         (a_dat_w[127 - 32 * k -: 32]),
            .wren_a         (|we),
            .byteena_a      (we | {4{~(|we)}}),
            .q_a            (a_dat_r[127 - 32 * k -: 32]),
            .address_b      (b_adr[9:2]),
            .data_b         (32'd0),
            .wren_b         (1'b0),
            .byteena_b      (1'b1),
            .q_b            (qb[k]),
            .aclr0          (1'b0),
            .aclr1          (1'b0),
            .addressstall_a (1'b0),
            .addressstall_b (1'b0),
            .clock1         (1'b1),
            .clocken0       (1'b1),
            .clocken1       (1'b1),
            .clocken2       (1'b1),
            .clocken3       (1'b1),
            .eccstatus      (),
            .rden_a         (1'b1),
            .rden_b         (1'b1)
        );
    end endgenerate
`endif
endmodule
`default_nettype wire
