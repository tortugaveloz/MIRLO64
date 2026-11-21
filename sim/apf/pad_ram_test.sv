`timescale 1ns/1ps
// The pad bus: the controller drives the line high or low (it releases it to high, as the bus' pull-up does); the device pulls it
// low. Every bit is a cell of 60 clocks that starts with a falling edge; the receiver takes the bit 26 clocks on. A device
// answers a poll (0x4A10000C) with twelve words: key, stick, trigger of the pad 1, then of the pads 2, 3 and 4.
module pad_ram_test;
    reg clk = 0;
    always #5 clk = ~clk;
    reg reset_n = 0;
    wire line, pad_o;
    reg dev_low = 0;
    assign line = dev_low ? 1'b0 : pad_o;
    wire pad_we; wire [3:0] pad_wa; wire [31:0] pad_wd; wire timed_out;
    io_pad_controller dut (.clk, .reset_n, .pad_1wire(line), .pad_o, .pad_we, .pad_wa, .pad_wd, .rx_timed_out(timed_out));
    reg [31:0] ram [0:15];
    initial for (int i = 0; i < 16; i++) ram[i] = 32'd0;
    always @(posedge clk) if (pad_we) ram[pad_wa] <= pad_wd;

    reg [31:0] sent [0:11];
    int errs = 0, polls = 0, beats = 0;

    task automatic send_word(input [31:0] w);
        for (int b = 31; b >= 0; b--) begin
            dev_low = 1; repeat (10) @(posedge clk);
            dev_low = !w[b]; repeat (36) @(posedge clk);
            dev_low = 0; repeat (14) @(posedge clk);
        end
    endtask

    // the controller's words: a cell starts at a falling edge of the line it drives, the bit is the line 30 clocks on
    reg [31:0] txw; int txn = 0; reg txing = 0;
    always @(negedge pad_o) if (!dev_low && !txing && dut.state != 0 && dut.tr_state == 3 /* TR_TX_CONTINUE */) begin
        txing = 1; txw = 0;
        for (int b = 0; b < 32; b++) begin
            repeat (30) @(posedge clk); txw = {txw[30:0], pad_o};
            if (b < 31) begin @(negedge pad_o); end
        end
        txing = 0;
        if (txw == 32'h4A10000C) polls++;
        else if (txw == 32'h4AFE0000) beats++;
        else begin errs++; $display("unknown word sent: %h", txw); end
    end

    initial begin
        repeat (20) @(posedge clk); reset_n = 1;
        for (int n = 0; n < 4; n++) begin
            wait (dut.state == 3);                       // ST_RX_BUTTON_2: the poll has been sent
            repeat (40) @(posedge clk);
            for (int w = 0; w < 12; w++) begin
                sent[w] = $urandom;
                send_word(sent[w]);
                repeat (30 + (w % 5) * 7) @(posedge clk);
            end
            repeat (300) @(posedge clk);
            // pad k (0..3): key at k, stick at 4 + k, trigger at 8 + k (16 bits)
            for (int k = 0; k < 4; k++) begin
                if (ram[k] !== sent[3 * k])                  begin errs++; $display("key %0d: %h, sent %h", k, ram[k], sent[3 * k]); end
                if (ram[4 + k] !== sent[3 * k + 1])          begin errs++; $display("stick %0d: %h, sent %h", k, ram[4 + k], sent[3 * k + 1]); end
                if (ram[8 + k] !== {16'd0, sent[3 * k + 2][15:0]}) begin errs++; $display("trigger %0d: %h, sent %h", k, ram[8 + k], sent[3 * k + 2]); end
            end
            for (int k = 12; k < 16; k++) if (ram[k] !== 32'd0) begin errs++; $display("word %0d written: %h", k, ram[k]); end
        end
        // the heartbeat (every 2^19 clocks)
        repeat (600000) @(posedge clk);
        if (beats == 0) begin errs++; $display("no heartbeat word was sent"); end
        if (polls < 4) begin errs++; $display("%0d polls sent", polls); end
        if (timed_out) begin errs++; $display("timed out"); end
        $display("pad_ram_test: %0d polls, %0d heartbeats sent, %0d errors", polls, beats, errs);
        if (errs == 0) $display("PASS"); else $display("FAIL");
        $finish;
    end
    initial begin #50000000; $display("FAIL: timeout"); $finish; end
endmodule
