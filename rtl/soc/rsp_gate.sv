// The RSP's clock: clk_c (the game CPU's) with some of its edges left out. On the FPGA a clock-control block of the
// global network with an enable (altclkctrl; the enable is taken on the falling edge of the input clock, so the output
// never has a runt pulse); the timing analyser sees a clock that is clk_c itself, so every path between the RSP and
// clk_sys is timed against the 3/2 relation, like the CPU's (rtl/soc/cpu_port.sv), with no exception.
// In simulation, a latch open while the clock is low.
`default_nettype none

module rsp_gate (
    input  wire clk_c,
    input  wire ena,                        // from a register of clk_c, or of the clock this makes: before the edge it lets by
    output wire gclk
);
`ifdef VERILATOR
    logic ena_l;
    always_latch if (!clk_c) ena_l = ena;
    assign gclk = clk_c & ena_l;
`else
    altclkctrl #(.clock_type("Global Clock"), .ena_register_mode("falling edge"), .number_of_clocks(1)) gate (.inclk(clk_c), .ena(ena), .outclk(gclk));
`endif
endmodule
`default_nettype wire
