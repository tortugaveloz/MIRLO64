// The DPC registers as the console shows them to a reader, over rdp_dpc.sv's:
//   - DPC_START written while START is valid (written and END not yet) is ignored (start_ok; systemtest "Writes to start
//     address should be ignored while start-valid is set"),
//   - DPC_STATUS' layout: 0 XBUS, 1 FREEZE, 2 FLUSH, 3 START_GCLK, 4 TMEM busy (a load running), 5 PIPE busy, 6 cmd busy
//     (a command in the FIFO or under way), 7 CBUF ready (always), 8 DMA busy (commands between CURRENT and END), 9 END valid, 10 START valid. START_GCLK and
//     PIPE_BUSY are set by a write of END (not while frozen) and by the unfreeze, and stay until the RDP completes a SYNC FULL
//     (PIPE_BUSY also by the STATUS write's bit 7); so after a list the register reads 0xA8, as the systemtest waits for.
// (rdp_dpc.sv's own status had the bits 3..6 all as "the RDP is busy" and no DMA busy.)
`default_nettype none

module rcp_dpc_view (
    input  wire         clk, rst,
    input  wire         end_we, status_we,  // the writes that reach rdp_dpc (the microcode's or the CPU's)
    input  wire  [31:0] wdata,
    input  wire         sync_full,          // the RDP completed a SYNC FULL (a pulse)
    input  wire         cmd_busy,           // a command waits in the FIFO or is under way
    input  wire         tmem_busy,          // a texture load runs
    input  wire  [31:0] st_raw,             // rdp_dpc's status
    input  wire  [23:0] cur, end_,
    output wire         start_ok,           // a write of START is taken
    output wire  [31:0] st
);
logic gclk, pipe;
always_ff @(posedge clk) begin
    if (rst) begin gclk <= 1'b0; pipe <= 1'b0; end
    else begin
        if (end_we && !st_raw[1]) begin gclk <= 1'b1; pipe <= 1'b1; end
        if (status_we && wdata[2] && !wdata[3]) begin gclk <= 1'b1; pipe <= 1'b1; end    // unfrozen
        if (status_we && wdata[7]) pipe <= 1'b0;
        if (sync_full) begin gclk <= 1'b0; pipe <= 1'b0; end
    end
end
assign start_ok = !st_raw[10];
assign st = {21'd0, st_raw[10], st_raw[9], cur < end_, 1'b1, cmd_busy, pipe, tmem_busy, gclk, st_raw[2:0]};
endmodule
`default_nettype wire
