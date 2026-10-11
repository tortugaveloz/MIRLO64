// The interact side of mirlo_regs (rtl/soc/mirlo_regs.sv) under Icarus: the Pocket's writes of the core's settings -- the two slots'
// fields (2-15), the controller layout lists (16-20) and its actions (21) -- and what the CPU reads of them (INTERACT0-2), against a
// model of the packing written here from docs/control.md.   sim/interact/run.sh
`timescale 1ns/1ps
module interact_test;
    `include "mirlo_regs.svh"
    logic clk = 0;
    always #8 clk = ~clk;                      // clk_sys, 62.5 MHz
    logic rst = 1;
    logic r_req = 0, r_we = 0;
    logic [15:0] r_addr = 0;
    logic [31:0] r_wdata = 0;
    wire  [31:0] r_rdata;
    logic ia_wr = 0;
    logic [5:0]  ia_address = 0;
    logic [31:0] ia_data = 0;
    wire  [31:0] ia_q;
    wire  [4:0]  ia_crop_v;
    wire  [1:0]  ia_crop_r;
    wire  [3:0][31:0] cont0 = '0;             // the four pads: nothing pressed
    mirlo_regs regs (.clk, .rst, .r_req, .r_we, .r_addr, .r_wdata, .r_rdata,
        .ia_address, .ia_data, .ia_wr, .ia_q, .ia_crop_v, .ia_crop_r,
        .aud_fill(12'd0), .br_file_size(32'd0), .br_complete_trigger(1'b0), .br_current_address(32'd0), .br_command_result_code(3'd0),
        .br_host_reset_n(1'b1), .br_host_loaded(1'b1), .cont_key(cont0), .cont_joy(cont0), .cont_trig(cont0),
        .vblank(1'b0), .tv_pal(1'b0), .bus_errors(16'd0), .su_valid(1'b0), .su_data(32'd0), .cmd_ready(1'b1),
        .mrdp_sync_count(32'd0), .mrdp_load_count(32'd0), .mrdp_unknown_ops(16'd0), .mrdp_idle(1'b1),
        .uart_txfull(1'b0), .uart_rx_byte(8'd0), .uart_rxempty(1'b1), .mon_sp(1'b0), .mon_field(1'b0), .mon_dp(1'b0),
        .mi_w_mi(1'b0), .mi_w_pi(1'b0), .mi_w_we(1'b0), .mi_w_a(2'd0), .mi_w_wd(32'd0), .d_rdata(32'd0), .d_ack(1'b0),
        .io_req(1'b0), .io_we(1'b0), .io_adr(24'd0), .io_wd(32'd0), .vi_irq(1'b0), .vi_chg(1'b0), .vi_clr(1'b0),
        .ai_irq(1'b0), .ai_clr(1'b0), .ai_chg(1'b0), .ai_dac(14'd0), .si_irq(1'b0), .si_clr(1'b0), .si_ev(1'b0),
        .gbc_busy(1'b0), .gbc_rdata(8'd0));

    int errors = 0;
    task check(input string what, input logic [31:0] got, input logic [31:0] want);
        if (got !== want) begin errors++; $display("FAIL %s: got %08x, want %08x", what, got, want); end
    endtask
    // the Pocket writes an interact.json address (a bus word), as core_top's interact FIFO delivers it: one cycle of ia_wr
    task ia_write(input logic [5:0] word, input logic [31:0] data);
        @(negedge clk); ia_address = word; ia_data = data; ia_wr = 1;
        @(negedge clk); ia_wr = 0; ia_data = 32'hDEADBEEF;
    endtask
    // what the Pocket would read back of a word (ia_q follows ia_address a cycle later)
    task ia_read(input logic [5:0] word, output logic [31:0] q);
        @(negedge clk); ia_address = word;
        @(negedge clk); q = ia_q;
    endtask
    // the CPU's read of a register (one access port: the data the cycle after r_req)
    task csr_read(input logic [15:0] addr, output logic [31:0] q);
        @(negedge clk); r_req = 1; r_we = 0; r_addr = addr;
        @(negedge clk); r_req = 0; q = r_rdata;
    endtask
    task csr_write(input logic [15:0] addr, input logic [31:0] data);
        @(negedge clk); r_req = 1; r_we = 1; r_addr = addr; r_wdata = data;
        @(negedge clk); r_req = 0; r_we = 0;
    endtask
    // the model: bus words 16-20 are five-bit fields (Normal, Shift, Shift: D-pad, D-pad keys, spare) in INTERACT2 [4:0], [9:5], [14:10], [19:15], [24:20];
    // word 21 flips INTERACT2 [28] (value bit 0) and [29] (value bit 1)
    logic [4:0]  model [0:4];
    logic        act_a = 0, act_b = 0;
    function logic [31:0] expected();
        expected = 0;
        for (int i = 0; i < 5; i++) expected |= 32'(model[i]) << (5 * i);
        expected |= 32'(act_a) << 28 | 32'(act_b) << 29;
    endfunction
    task check_regs(input string when);
        logic [31:0] q;
        csr_read(R_APF_INTERACT_INTERACT2, q);
        check({when, ": INTERACT2"}, q, expected());
    endtask

    logic [31:0] q;
    initial begin
        for (int i = 0; i < 5; i++) model[i] = 0;
        repeat (3) @(negedge clk);
        rst = 0;
        @(negedge clk);
        for (int i = 0; i < 5; i++) model[i] = 5'd0;     // (every list resets to Auto)
        check_regs("reset");
        csr_read(R_APF_INTERACT_INTERACT0, q); check("reset: INTERACT0", q, 0);

        // every field, a distinct value each (bits above [4:0] are ignored)
        for (int f = 0; f < 5; f++) begin
            model[f] = 5'(f * 7 + 3);
            ia_write(6'(16 + f), 32'hFFFF_FFE0 | 32'(model[f]));
        end
        check_regs("five fields written");
        model[3] = 5'd31; ia_write(19, 32'd31);
        check_regs("field 3 rewritten");
        // words 22-63 are nobody's, and 21 is the actions': the fields do not change
        for (int w = 22; w < 64; w++) ia_write(6'(w), 32'h1F);
        check_regs("words 22-63 written");
        // the CPU cannot write it
        csr_write(R_APF_INTERACT_INTERACT2, 32'hFFFF_FFFF);
        check_regs("CPU write to INTERACT2");

        // the actions: bit 0 of the value flips A (unused), bit 1 flips B (Save); 3 flips both (Forget); 0 and the higher bits nothing
        ia_write(21, 1); act_a = !act_a; check_regs("write 1 (nothing for the CPU)");
        ia_write(21, 2); act_b = !act_b; check_regs("save pressed");
        ia_write(21, 3); act_a = !act_a; act_b = !act_b; check_regs("forget pressed");
        ia_write(21, 1); act_a = !act_a; check_regs("write 1 again");
        ia_write(21, 0); ia_write(21, 32'h1C); check_regs("other values written to 21");
        ia_write(21, 32'hFFFF_FFFD); act_a = !act_a; check_regs("a value with bit 0 set, bit 1 clear");

        // the Pocket reads the lists' fields back (16-20), as it reads 2-15; the actions (21) and the rest read 0
        for (int w = 16; w < 64; w++) begin ia_read(6'(w), q); check($sformatf("ia_q of word %0d", w), q, (w < 21) ? 32'(model[w - 16]) : 32'd0); end

        // the legacy fields still work as before: Crop Vertical (3), Crop Right (5), and their readback
        ia_write(3, 8);  check("crop_v", 32'(ia_crop_v), 8);
        ia_write(5, 1);  check("crop_r", 32'(ia_crop_r), 1);
        ia_read(3, q);  check("ia_q of field 3", q, 8);
        ia_write(4, 6);                                           // Modifier = Select
        csr_read(R_APF_INTERACT_INTERACT0, q); check("INTERACT0 fields", q, 32'd6 << 10 | 32'd8 << 5 | 32'd1 << 15 | 32'd0 << 15);
        // a whole-slot write sets CHANGED, the CPU's read of the slot clears it
        ia_write(1, 32'h0123_4567);
        csr_read(R_APF_INTERACT_INTERACT_CHANGED1, q); check("CHANGED1 set", q, 1);
        csr_read(R_APF_INTERACT_INTERACT1, q);         check("INTERACT1 whole", q, 32'h0123_4567);
        csr_read(R_APF_INTERACT_INTERACT_CHANGED1, q); check("CHANGED1 cleared", q, 0);
        ia_read(1, q); check("ia_q of slot 1", q, 32'h0123_4567);
        check_regs("after the legacy fields");                    // (the slots and the lists do not touch each other)

        // reset clears everything
        ia_write(21, 1);
        @(negedge clk); rst = 1; @(negedge clk); rst = 0; @(negedge clk);
        for (int i = 0; i < 5; i++) model[i] = 5'd0;
        act_a = 0; act_b = 0;
        check_regs("second reset");
        csr_read(R_APF_INTERACT_INTERACT0, q); check("second reset: INTERACT0", q, 0);

        if (errors) begin $display("interact_test: FAIL (%0d)", errors); $finish(1); end
        $display("interact_test: PASS");
        $finish;
    end
endmodule
