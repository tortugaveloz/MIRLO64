`timescale 1ns/1ps
// Synthetic AI DMA address test. Neither ROM nor game code is used.
// MiSTer and ares preserve a carry when an AI DMA ends at an 8 KiB boundary;
// the carry is applied to the next DMA's address.
module dma_boundary_tb;
  logic clk=0; always #5 clk=~clk;
  logic rst=1; logic [2:0] a=0; logic [31:0] wd=0; logic wr_s=0;
  wire [31:0] rd; wire clr, irq, chg;
  logic step_we=0; logic [19:0] step_wd=20'h10000;
  wire [13:0] dac; logic [9:0] fill=256; wire fw; wire [31:0] fd;
  wire req; wire [23:0] adr;
  logic slow=0; logic [2:0] phase=0;
  wire ack=req && (!slow || phase==0);
  integer n=0; logic [23:0] seen[4];
  logic waiting=0; logic [23:0] held_adr;
  rcp_ai dut(.clk,.rst,.a,.wd,.wr_s,.rd,.clr,.irq,.chg,.step_we,.step_wd,.dac,.fill,.fw,.fd,.req,.adr,.ack,.rdata(32'h12345678));
  always @(posedge clk) phase <= phase + 3'd1;
  always @(posedge clk) if (!rst && req && ack && n<4) begin seen[n]=adr; n=n+1; end
  always @(posedge clk) begin
    if (rst) waiting <= 0;
    else begin
      if (waiting && (!req || adr!=held_adr)) $fatal(1,"AI request changed before acknowledgement");
      waiting <= req && !ack;
      if (req && !ack) held_adr <= adr;
    end
  end
  task wr(input logic [2:0] regno, input logic [31:0] value);
    @(negedge clk); a=regno; wd=value; wr_s=1;
    @(negedge clk); wr_s=0;
  endtask
  initial begin
    slow=$test$plusargs("stall");
    repeat(4) @(negedge clk); rst=0;
    @(negedge clk); step_we=1; @(negedge clk); step_we=0;
    wr(0,32'h1ff8); wr(1,8); wr(0,32'h4000); wr(1,8); wr(2,1);
    repeat(20) @(negedge clk); fill=0;
    for (int t=0;t<1000 && n<4;t++) @(negedge clk);
    if (n!=4) $fatal(1,"expected four DMA reads, got %0d",n);
    $display("AI byte addresses: %h %h %h %h",(seen[0]&24'h1fffff)<<2,(seen[1]&24'h1fffff)<<2,(seen[2]&24'h1fffff)<<2,(seen[3]&24'h1fffff)<<2);
    if (seen[0] != 24'he007fe || seen[1] != 24'he007ff || seen[2] != 24'he01800 || seen[3] != 24'he01801)
      $fatal(1,"AI DMA carry differs from MiSTer/ares after an 8 KiB boundary");
    // A boundary crossed within one buffer must advance normally.
    @(negedge clk); rst=1; fill=256; n=0;
    repeat(4) @(negedge clk); rst=0;
    @(negedge clk); step_we=1; @(negedge clk); step_we=0;
    wr(0,32'h1ff8); wr(1,16); wr(2,1);
    repeat(20) @(negedge clk); fill=0;
    for (int t=0;t<1000 && n<4;t++) @(negedge clk);
    if (n!=4 || seen[0]!=24'he007fe || seen[1]!=24'he007ff || seen[2]!=24'he00800 || seen[3]!=24'he00801)
      $fatal(1,"continuous DMA boundary increment is wrong");

    // The carry must survive an empty queue until a later buffer arrives.
    @(negedge clk); rst=1; fill=256; n=0;
    repeat(4) @(negedge clk); rst=0;
    @(negedge clk); step_we=1; @(negedge clk); step_we=0;
    wr(0,32'h1ff8); wr(1,8); wr(2,1);
    repeat(20) @(negedge clk); fill=0;
    for (int t=0;t<1000 && n<2;t++) @(negedge clk);
    if (n!=2) $fatal(1,"first DMA did not finish");
    repeat(30) @(negedge clk); wr(0,32'h4000); wr(1,8);
    for (int t=0;t<1000 && n<4;t++) @(negedge clk);
    if (n!=4 || seen[2]!=24'he01800 || seen[3]!=24'he01801)
      $fatal(1,"AI DMA carry lost when queue was empty");

    // A buffer that ends away from a boundary must not shift the next one.
    @(negedge clk); rst=1; fill=256; n=0;
    repeat(4) @(negedge clk); rst=0;
    @(negedge clk); step_we=1; @(negedge clk); step_we=0;
    wr(0,32'h1000); wr(1,8); wr(0,32'h4000); wr(1,8); wr(2,1);
    repeat(20) @(negedge clk); fill=0;
    for (int t=0;t<1000 && n<4;t++) @(negedge clk);
    if (n!=4 || seen[2]!=24'he01000 || seen[3]!=24'he01001)
      $fatal(1,"spurious AI DMA carry");

    // Submit the next LEN on the exact cycle the preceding DMA ends.
    @(negedge clk); rst=1; fill=256; n=0; slow=1;
    repeat(4) @(negedge clk); rst=0;
    @(negedge clk); step_we=1; @(negedge clk); step_we=0;
    wr(0,32'h1ff8); wr(1,8); wr(0,32'h4000); wr(2,1);
    repeat(20) @(negedge clk); fill=0;
    begin
      bit submitted=0;
      for (int t=0;t<1000 && !submitted;t++) begin
        @(negedge clk);
        if (n==1 && req && ack) begin
          a=1; wd=8; wr_s=1; submitted=1;
          @(negedge clk); wr_s=0;
        end
      end
      if (!submitted) $fatal(1,"could not align the new LEN with the last read");
    end
    for (int t=0;t<1000 && n<4;t++) @(negedge clk);
    if (n!=4 || seen[2]!=24'he01800 || seen[3]!=24'he01801)
      $fatal(1,"AI DMA carry lost when LEN and completion coincide");
    $display("PASS AI DMA boundary carry (queued, continuous, delayed, no carry, simultaneous)"); $finish;
  end
endmodule
