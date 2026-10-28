// Minimal non-showahead FIFO model: this test checks request ordering, not CDC timing.
module dcfifo #(parameter clocks_are_synchronized="FALSE", intended_device_family="Cyclone V", lpm_numwords=4, lpm_showahead="OFF", lpm_type="dcfifo", lpm_width=32, lpm_widthu=2, overflow_checking="OFF", rdsync_delaypipe=5, underflow_checking="OFF", use_eab="ON", lpm_hint="", wrsync_delaypipe=5) (
    input wrclk, rdclk, wrreq, rdreq, input [lpm_width-1:0] data,
    output logic [lpm_width-1:0] q = 0, output wire rdempty
);
    logic [lpm_width-1:0] mem[0:31]; integer wp=0, rp=0;
    assign rdempty = wp == rp;
    always @(posedge wrclk) if (wrreq) begin mem[wp % 32] <= data; wp <= wp+1; end
    always @(posedge rdclk) if (rdreq && !rdempty) begin q <= mem[rp % 32]; rp <= rp+1; end
endmodule

module apf_test;
    logic clk_74a=0, clk_sys=0, rd=0; logic [31:0] ba=0;
    wire [29:0] addr; wire cyc, stb, we; wire [31:0] read_data; logic ack=0;
    integer pending=0, seen=0, received=0; logic active=0;
    always #5 clk_74a=~clk_74a;
    always #7 clk_sys=~clk_sys;
    apf_wishbone_master dut(.clk_74a(clk_74a), .clk_sys(clk_sys), .bridge_addr(ba),
        .bridge_wr_data(32'd0), .bridge_rd_data(read_data), .bridge_wr(1'b0), .bridge_rd(rd),
        .bridge_endian_little(1'b1), .ram_data_address(32'd0), .current_address(),
        .addr(addr), .bte(), .cti(), .cyc(cyc), .data_write(), .sel(), .stb(stb), .we(we),
        .ack(ack), .data_read(32'hb0000000 | {30'd0, addr[1:0]}), .err(1'b0));
    always @(posedge clk_sys) begin
        ack <= 0;
        if (cyc && !active) begin
            active <= 1; pending <= 12;
            if (addr !== (30'h10000000 + 30'(seen))) $fatal(1,"read %0d: address %h expected %h",seen,addr,30'h10000000+30'(seen));
            seen <= seen+1;
        end else if (active && pending > 0) begin
            pending <= pending-1;
            if (pending == 1) ack <= 1;
        end else if (!cyc) active <= 0;
    end
    always @(negedge clk_74a) if (dut.sdram_read_fifo.rp > received) begin
        if (read_data !== (32'hb0000000 | 32'(received)))
            $fatal(1, "response %0d: got %h", received, read_data);
        received = received + 1;
    end
    initial begin
        #40;
        for (int i=0;i<3;i++) begin
            @(negedge clk_74a); ba=i*4; rd=1;
            @(negedge clk_74a); rd=0;
        end
        #4000;
        if (seen != 3) $fatal(1,"received %0d of 3 reads",seen);
        if (received != 3) $fatal(1,"received %0d of 3 responses",received);
        $display("APF queued reads PASS"); $finish;
    end
endmodule
