`timescale 1ps/1ps
// Bridge -> real arbiter and SDRAM controller, including refresh and bank
// conflicts. The PHY is a fixed-latency read-data model, not an SDRAM chip.
// Compile with Intel's altera_mf.v to exercise the actual asynchronous FIFOs.
module sdr_deadline_test #(parameter integer WORDS=8192, RL=5);
    logic clk74=0, clk=0, rst=1, bridge_rd=0, bridge_wr=0;
    logic [31:0] bridge_addr=0, bridge_wr_data=0;
    wire [31:0] bridge_data, wb_wdata, wb_rdata;
    wire [29:0] wb_addr;
    wire wb_cyc, wb_stb, wb_we, wb_ack, wb_pending, pa_ack;
    wire [3:0] wb_sel;
    logic [5:0] p_valid, p_we, p_urgent;
    logic [5:0][23:0] p_addr;
    logic [5:0][31:0] p_wdata;
    logic [5:0][3:0] p_be;
    wire [5:0] p_ready, p_rvalid;
    wire [31:0] p_rdata;
    wire pv, pw; wire [23:0] pa; wire [31:0] pd; wire [3:0] pb;
    wire req_valid, req_we, req_ready, la_valid, rd_valid, ready;
    wire [23:0] req_addr, la_addr;
    wire [31:0] req_data, rd_data;
    wire [3:0] req_be;
    wire [2:0] req_tag, rd_tag;
    wire [1:0] dfi_ras, dfi_cas, dfi_we;
    wire [3:0] dfi_ba;
    wire [25:0] dfi_addr;
    wire [3:0] dfi_mask;
    wire dfi_cke, dfi_wren, dfi_rden;
    wire [31:0] dfi_wdata;
    logic [31:0] data_pipe[0:RL-1];
    logic [23:0] addr1=24'h000000, addr3=24'h200400;
    logic [31:0] count1=0, count3=0;
    integer period=80, bad=0, writes=0, write_bad=0, max_latency=0, phase_ps=0;
    integer sys_cycle=0, wb_start=0, max_wb=0, grant_start=0, max_grant=0;
    integer ctrl_stall=0, max_ctrl_stall=0;
    integer dma_wait=0, max_dma_wait=0;
    logic prev_wb=0, waiting_grant=0, pa_dma=0, dma_stress=0, psram_stress=0, drain_dma=0, protect_pending=0, random_traffic=0;
    integer traffic_seed=1;
    function automatic logic [23:0] next_addr(input logic [23:0] x);
        logic [23:0] y;
        y = x ^ (x << 7); y = y ^ (y >> 9); y = y ^ (y << 8);
        return y;
    endfunction
    wire x_valid, x_ack, ps_ready;
    wire [5:0] ps_a;
    wire [31:0] ps_dq, ps_rdata;
    wire ps_oe, ps_adv, ps_oe_n, ps_we_n, ps_cre;
    wire [1:0] ps_ce;
    wire [3:0] ps_bm;
    assign wb_ack = pa_ack && !pa_dma;
    initial begin
        if ($value$plusargs("phase_ps=%d", phase_ps)) $display("bridge clock phase %0d ps", phase_ps);
        #(phase_ps); forever #6734 clk74=~clk74;
    end
    always #8000 clk=~clk;

    apf_wishbone_master master(.clk_74a(clk74), .clk_sys(clk), .bridge_addr(bridge_addr),
      .bridge_wr_data(bridge_wr_data), .bridge_rd_data(bridge_data), .bridge_wr(bridge_wr), .bridge_rd(bridge_rd),
      .bridge_endian_little(1'b1), .ram_data_address(32'd0), .current_address(),
      .addr(wb_addr), .bte(), .cti(), .cyc(wb_cyc), .pending(wb_pending), .data_write(wb_wdata), .sel(wb_sel), .stb(wb_stb), .we(wb_we),
      .ack(wb_ack), .data_read(wb_rdata), .err(1'b0));
    port_apf port(.clk(clk), .rst(rst), .cyc(pa_dma || wb_cyc), .stb(pa_dma || wb_stb), .we(pa_dma ? 1'b0 : wb_we),
      .adr(pa_dma ? 30'h00123456 : wb_addr), .dat_w(wb_wdata), .sel(wb_sel), .ack(pa_ack), .dat_r(wb_rdata),
      .x_sel(pa_dma && psram_stress), .x_valid(x_valid), .x_ack(x_ack), .x_rdata(32'b0), .p_valid(pv), .p_we(pw), .p_addr(pa),
      .p_wdata(pd), .p_be(pb), .p_ready(p_ready[4]), .p_rvalid(p_rvalid[4]), .p_rdata(p_rdata));
    always_comb begin
        p_valid=6'b001010; p_valid[4]=pv;
        p_urgent=6'b001010;
        p_we='0; p_we[1]=count1[0]; p_we[3]=count3[0]; p_we[4]=pw;
        p_addr='0; p_addr[1]=addr1; p_addr[3]=addr3; p_addr[4]=pa;
        p_wdata='0; p_wdata[4]=pd; p_be='1; p_be[4]=pb;
    end
    sdr_arb #(.N(6), .APF_PREEMPT(1)) arb(.clk(clk), .rst(rst), .p_valid(p_valid), .p_we(p_we), .p_addr(p_addr),
      .p_wdata(p_wdata), .p_be(p_be), .p_urgent(p_urgent), .apf_force((wb_cyc || (protect_pending && wb_pending)) && (!pa_dma || drain_dma)), .p_ready(p_ready), .p_rvalid(p_rvalid), .p_rdata(p_rdata),
      .req_valid(req_valid), .req_we(req_we), .req_addr(req_addr), .req_wdata(req_data), .req_be(req_be), .req_tag(req_tag),
      .req_ready(req_ready), .la_valid(la_valid), .la_addr(la_addr), .rd_valid(rd_valid), .rd_data(rd_data), .rd_tag(rd_tag));
    sdr_ctrl #(.T_INIT(100), .RL(RL)) ctrl(.clk(clk), .rst(rst), .ready(ready), .req_valid(req_valid), .req_we(req_we),
      .req_addr(req_addr), .req_wdata(req_data), .req_be(req_be), .req_tag(req_tag), .req_ready(req_ready),
      .la_valid(la_valid), .la_addr(la_addr), .rd_valid(rd_valid), .rd_data(rd_data), .rd_tag(rd_tag), .rd_next(), .rd_next_tag(),
      .dfi_ras_n(dfi_ras), .dfi_cas_n(dfi_cas), .dfi_we_n(dfi_we), .dfi_addr(dfi_addr), .dfi_ba(dfi_ba), .dfi_cke(dfi_cke),
      .dfi_wrdata_en(dfi_wren), .dfi_wrdata(dfi_wdata), .dfi_wrdata_mask(dfi_mask), .dfi_rddata_en(dfi_rden),
      .dfi_rddata(data_pipe[RL-1]), .dfi_rddata_valid(1'b1));
    // The controller is real; the external chip's read data is unused for
    // the competing DMA. Its latency includes the controller's idle gap.
    psram ps(.clk(clk), .rst(rst), .wake(ready), .x_valid(x_valid), .x_we(1'b0),
      .x_addr(23'd0), .x_wdata(32'd0), .x_be(4'hf), .x_ack(x_ack), .x_rdata(ps_rdata), .ready(ps_ready),
      .o_a(ps_a), .o_dq(ps_dq), .o_dq_oe(ps_oe), .o_adv_n(ps_adv), .o_oe_n(ps_oe_n), .o_we_n(ps_we_n),
      .o_cre(ps_cre), .o_ce_n(ps_ce), .o_bm_n(ps_bm), .i_dq(32'd0));
    always @(posedge clk) begin
        if (wb_cyc && !wb_pending) $fatal(1, "bridge cycle is not marked pending");
        if (rst) pa_dma <= 0;
        else if (pa_dma) begin if (pa_ack) pa_dma <= 0; end
        else if (dma_stress && !wb_cyc && (!protect_pending || !wb_pending)) pa_dma <= 1;
        if (pa_dma && wb_pending) begin
            dma_wait <= dma_wait + 1;
            if (dma_wait + 1 > max_dma_wait) max_dma_wait <= dma_wait + 1;
            if (drain_dma && dma_wait >= 100) $fatal(1, "DMA did not release the shared port within 100 sys cycles");
        end else dma_wait <= 0;
        sys_cycle <= sys_cycle + 1;
        if (ready && req_valid && !req_ready) begin
            ctrl_stall <= ctrl_stall + 1;
            if (ctrl_stall + 1 > max_ctrl_stall) max_ctrl_stall <= ctrl_stall + 1;
        end else ctrl_stall <= 0;
        prev_wb <= wb_cyc;
        if (wb_cyc && !prev_wb) wb_start <= sys_cycle;
        if (wb_ack && wb_cyc && sys_cycle-wb_start > max_wb) max_wb <= sys_cycle-wb_start;
        if (pv && !pa_dma && !waiting_grant) begin grant_start <= sys_cycle; waiting_grant <= 1; end
        if (pv && !pa_dma && p_ready[4]) begin
            if (waiting_grant && sys_cycle-grant_start > max_grant) max_grant <= sys_cycle-grant_start;
            waiting_grant <= 0;
        end
        data_pipe[0] <= 32'h60000000 ^ {8'b0, req_addr};
        for (int i=1; i<RL; i++) data_pipe[i] <= data_pipe[i-1];
        if (p_ready[1]) begin addr1 <= random_traffic ? next_addr(addr1) : addr1 + 24'h080201; count1 <= count1 + 1; end
        if (p_ready[3]) begin addr3 <= random_traffic ? next_addr(addr3) : addr3 + 24'h100601; count3 <= count3 + 1; end
        if (req_valid && req_ready && req_we && req_tag == 4) begin
            if (req_addr !== 24'hcc8000 + writes || req_data !== (32'h60000000 ^ (24'hcc8000 + writes))) write_bad <= write_bad + 1;
            writes <= writes + 1;
        end
    end
    initial begin
        dma_stress = $test$plusargs("dma");
        psram_stress = $test$plusargs("psram");
        drain_dma = $test$plusargs("drain_dma");
        protect_pending = $test$plusargs("protect_pending");
        random_traffic = $test$plusargs("random_traffic");
        if ($value$plusargs("traffic_seed=%d", traffic_seed)) $display("traffic seed %0d", traffic_seed);
        if (random_traffic) begin addr1=24'(traffic_seed) | 24'h1; addr3=24'(traffic_seed) ^ 24'h51d235; end
        if ($value$plusargs("period=%d", period)) $display("bridge period %0d clk74 cycles", period);
        repeat(10) @(negedge clk74); rst=0;
        wait(ready && ps_ready); repeat(10) @(negedge clk74);
        if ($test$plusargs("write")) begin
            for (int i=0; i<WORDS; i++) begin
                bridge_addr=32'h07320000+i*4; bridge_wr_data=32'h60000000 ^ (24'hcc8000+i);
                bridge_wr=1; @(negedge clk74); bridge_wr=0; repeat(period-1) @(negedge clk74);
            end
            repeat(300) @(negedge clk74);
            if (writes != WORDS || write_bad != 0) $fatal(1, "write: %0d/%0d words, %0d incorrect", writes, WORDS, write_bad);
            $display("write PASS: %0d words; max WB %0d sys cycles; grant wait %0d; controller stall %0d; DMA drain %0d", writes, max_wb, max_grant, max_ctrl_stall, max_dma_wait); $finish;
        end
        for (int i=0; i<WORDS; i++) begin
            integer latency;
            bridge_addr=32'h07320000+i*4; bridge_rd=1;
            @(negedge clk74); bridge_rd=0;
            latency=0;
            for (int t=1; t<period; t++) begin
                @(negedge clk74);
                if (latency==0 && bridge_data === (32'h60000000 ^ (24'hcc8000+i))) latency=t;
            end
            if (latency==0) bad++; else if (latency>max_latency) max_latency=latency;
        end
        if (bad != 0) $fatal(1, "read: %0d/%0d words missed deadline; max observed %0d cycles", bad, WORDS, max_latency);
        $display("read PASS: %0d words; max observed latency %0d clk74 cycles; max WB %0d sys cycles; grant wait %0d; controller stall %0d; DMA drain %0d", WORDS, max_latency, max_wb, max_grant, max_ctrl_stall, max_dma_wait); $finish;
    end
endmodule
