`timescale 1ps/1ps
// Minimal FIFO model; this deliberately does not model the Intel FIFO's CDC delay.
module dcfifo #(parameter clocks_are_synchronized="FALSE", intended_device_family="Cyclone V", lpm_numwords=4, lpm_showahead="OFF", lpm_type="dcfifo", lpm_width=32, lpm_widthu=2, overflow_checking="OFF", rdsync_delaypipe=5, underflow_checking="OFF", use_eab="ON", lpm_hint="", wrsync_delaypipe=5) (
    input wrclk, rdclk, wrreq, rdreq, input [lpm_width-1:0] data,
    output logic [lpm_width-1:0] q = 0, output wire rdempty
);
    logic [lpm_width-1:0] mem[0:lpm_numwords-1]; integer wp=0, rp=0;
    assign rdempty = wp == rp;
    always @(posedge wrclk) if (wrreq) begin
        if (wp - rp >= lpm_numwords) $fatal(1, "FIFO overflow");
        mem[wp % lpm_numwords] <= data; wp <= wp+1;
    end
    always @(posedge rdclk) if (rdreq && !rdempty) begin q <= mem[rp % lpm_numwords]; rp <= rp+1; end
endmodule
