`timescale 1ps/1ps
// Stress test: real bridge master, port_apf and SDRAM arbiter with a fixed
// latency synthetic memory and continuously requesting CPU/RSP ports. The
// dcfifo model is defined in stream_model.sv and does not model CDC delays.
module apf_arbiter_test #(parameter bit APF_PREEMPT=0, parameter integer WORDS=512);
    parameter integer PERIOD=80, LATENCY=10;
    logic clk74=0, clk=0, rst=1, bridge_rd=0, bridge_wr=0;
    logic [31:0] bridge_addr=0;
    logic [31:0] bridge_wr_data=0;
    wire [31:0] bridge_data, bridge_dat_w, bridge_dat_r;
    wire [29:0] bridge_adr;
    wire bridge_cyc, bridge_stb, bridge_we, bridge_ack;
    wire [3:0] bridge_sel;
    logic fix=0;
    logic [5:0] p_valid, p_we, p_urgent;
    logic [5:0][23:0] p_addr;
    logic [5:0][31:0] p_wdata;
    logic [5:0][3:0] p_be;
    wire [5:0] p_ready, p_rvalid;
    wire [31:0] p_rdata;
    wire port_valid, port_we; wire [23:0] port_addr; wire [31:0] port_wdata; wire [3:0] port_be;
    wire req_valid, req_we, la_valid; wire [23:0] req_addr, la_addr; wire [31:0] req_data; wire [3:0] req_be; wire [2:0] req_tag;
    logic [LATENCY-1:0] pipe_v='0;
    logic [2:0] pipe_tag[0:LATENCY-1];
    logic [31:0] pipe_data[0:LATENCY-1];
    integer bad=0, memory_gap=1, memory_cycle=0, writes=0, write_bad=0;
    wire memory_ready = memory_cycle % memory_gap == 0;
    logic [31:0] image[0:32767];
    logic use_image=0;
    string image_path;
    always #6734 clk74=~clk74;
    always #8000 clk=~clk;
    apf_wishbone_master master(.clk_74a(clk74), .clk_sys(clk), .bridge_addr(bridge_addr),
      .bridge_wr_data(bridge_wr_data), .bridge_rd_data(bridge_data), .bridge_wr(bridge_wr), .bridge_rd(bridge_rd),
      .bridge_endian_little(1'b1), .ram_data_address(32'd0), .current_address(),
      .addr(bridge_adr), .bte(), .cti(), .cyc(bridge_cyc), .pending(), .data_write(bridge_dat_w), .sel(bridge_sel), .stb(bridge_stb), .we(bridge_we),
      .ack(bridge_ack), .data_read(bridge_dat_r), .err(1'b0));
    port_apf port(.clk(clk), .rst(rst), .cyc(bridge_cyc), .stb(bridge_stb), .we(bridge_we), .adr(bridge_adr),
      .dat_w(bridge_dat_w), .sel(bridge_sel), .ack(bridge_ack), .dat_r(bridge_dat_r), .x_sel(1'b0), .x_valid(),
      .x_ack(1'b0), .x_rdata(32'b0), .p_valid(port_valid), .p_we(port_we), .p_addr(port_addr),
      .p_wdata(port_wdata), .p_be(port_be), .p_ready(p_ready[4]), .p_rvalid(p_rvalid[4]), .p_rdata(p_rdata));
    always_comb begin
        p_valid=6'b001010; p_valid[4]=port_valid;
        p_urgent=6'b001010; p_urgent[4]=fix && bridge_cyc;
        p_we='0; p_we[4]=port_we; p_addr='0; p_addr[4]=port_addr;
        p_wdata='0; p_wdata[4]=port_wdata; p_be='0; p_be[4]=port_be;
    end
    sdr_arb #(.N(6), .APF_PREEMPT(APF_PREEMPT)) arb(.clk(clk), .rst(rst), .p_valid(p_valid), .p_we(p_we), .p_addr(p_addr), .p_wdata(p_wdata),
      .p_be(p_be), .p_urgent(p_urgent), .apf_force(fix && bridge_cyc), .p_ready(p_ready), .p_rvalid(p_rvalid), .p_rdata(p_rdata),
      .req_valid(req_valid), .req_we(req_we), .req_addr(req_addr), .req_wdata(req_data), .req_be(req_be), .req_tag(req_tag),
      .req_ready(memory_ready), .la_valid(la_valid), .la_addr(la_addr), .rd_valid(pipe_v[LATENCY-1]),
      .rd_data(pipe_data[LATENCY-1]), .rd_tag(pipe_tag[LATENCY-1]));
    always @(posedge clk) begin
        memory_cycle <= memory_cycle + 1;
        pipe_v <= {pipe_v[LATENCY-2:0], req_valid && memory_ready && !req_we};
        if (req_valid && memory_ready && req_we && req_tag == 4) begin
            if (req_addr !== 24'hcc8000 + writes || req_data !== (32'h60000000 ^ (24'hcc8000 + writes))) write_bad <= write_bad + 1;
            writes <= writes + 1;
        end
        pipe_tag[0] <= req_tag; pipe_data[0] <= use_image ? image[req_addr[14:0]] : (32'h60000000 ^ {8'b0, req_addr});
        for (int i=1; i<LATENCY; i++) begin pipe_tag[i] <= pipe_tag[i-1]; pipe_data[i] <= pipe_data[i-1]; end
    end
    initial begin
        if ($test$plusargs("urgent")) fix=1;
        if ($value$plusargs("gap=%d", memory_gap)) $display("memory acceptance interval %0d", memory_gap);
        if ($value$plusargs("image=%s", image_path)) begin $readmemh(image_path, image); use_image=1; end
        repeat(10) @(negedge clk74); rst=0;
        repeat(10) @(negedge clk74);
        if ($test$plusargs("write")) begin
            for (int i=0; i<WORDS; i++) begin
                bridge_addr=32'h07320000+i*4;
                bridge_wr_data=32'h60000000 ^ (24'hcc8000+i);
                bridge_wr=1; @(negedge clk74); bridge_wr=0;
                repeat(PERIOD-1) @(negedge clk74);
            end
            repeat(300) @(negedge clk74);
            if (writes != WORDS || write_bad != 0) $fatal(1, "bridge write: %0d/%0d writes, %0d incorrect", writes, WORDS, write_bad);
            $display("APF arbiter write PASS: %0d words, memory interval %0d", writes, memory_gap);
            $finish;
        end
        for (int i=0; i<WORDS; i++) begin
            bridge_addr=32'h07320000+i*4; bridge_rd=1;
            @(negedge clk74); bridge_rd=0;
            repeat(PERIOD-1) @(negedge clk74);
            if (bridge_data !== (use_image ? image[i] : (32'h60000000 ^ (24'hcc8000+i)))) bad++;
        end
        if (bad != 0) $fatal(1, "bridge urgent %0d, memory interval %0d: %0d/%0d words incorrect", fix, memory_gap, bad, WORDS);
        $display("APF arbiter readback PASS: %0d words, memory interval %0d", WORDS, memory_gap);
        $finish;
    end
endmodule
