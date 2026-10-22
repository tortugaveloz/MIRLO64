// Simple dual-port RAM: one write port (with lane enables), one read port
// with a registered address (data one cycle after the address), as Quartus
// infers into M10K blocks.
//
// Each lane is its own memory. M10K byte enables only exist for 8/9/10-bit
// lanes: written as one array with 16-bit (or 1-bit) lanes, Quartus built
// the line buffers from registers -- 16k flip-flops each, 243 % of the
// device (2026-09-27, first RDP fit).
module rdp_sdpram #(
    parameter AW = 10,
    parameter DW = 16,
    parameter NB = 1          // lanes (DW must be a multiple of NB)
) (
    input  wire               clk,
    input  wire               we,
    input  wire [AW-1:0]      waddr,
    input  wire [DW-1:0]      wdata,
    input  wire [NB-1:0]      wbe,
    input  wire [AW-1:0]      raddr,
    output wire [DW-1:0]      rdata
);
    localparam LW = DW / NB;
    genvar g;
    generate for (g = 0; g < NB; g = g + 1) begin : lane
        reg [LW-1:0] mem [0:(1<<AW)-1];
        reg [LW-1:0] q;
        always @(posedge clk) begin
            if (we && wbe[g]) mem[waddr] <= wdata[g*LW +: LW];
            q <= mem[raddr];
        end
        assign rdata[g*LW +: LW] = q;
    end endgenerate
endmodule
