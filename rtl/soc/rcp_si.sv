// The N64's SI (Serial Interface) and its PIF RAM in hardware (MIRLO_SI; docs/mips.md "The SI in hardware"): the registers, the
// 64-byte DMA to and from the RDRAM, the PIF RAM itself, and the handshake with the runtime that plays the PIF's command
// interpreter (the joybus: controllers, paks, EEPROM stay software).
//
// SI, 0x0480_0000 (the game's lw / sw reach it as a device access, as the MI's: mips_core's SIW, mirlo_mips.sv's decode; the
// registers repeat every 0x20 bytes):
//   00 DRAM_ADDR       write: the RDRAM address (8-byte aligned, 24 bits; ignored while a DMA is under way); read it back. The DMA
//                      counts it up as it goes: after a DMA it reads 64 bytes on
//   04 PIF_ADDR_RD64B  write: starts the DMA PIF RAM -> RDRAM, 64 bytes (the written address is ignored: one PIF RAM)
//   10 PIF_ADDR_WR64B  write: starts the DMA RDRAM -> PIF RAM, 64 bytes
//   18 STATUS          read {irq [12], 0, error [3], 0, IO busy [1], DMA busy [0]}; any write clears the interrupt (and the error)
// A start while a DMA is under way is ignored and sets the error bit (the console's DMA error).
//
// PIF RAM, 0x1FC0_07C0 .. 0x1FC0_07FF (KSEG0 or KSEG1): 16 words, the CPU's lw / sw (mips_core's SIW window: no address error),
// a block RAM, one word a read (c_q the cycle after the address). The CPU stalls (c_busy) while the DMA owns the RAM. Byte 63 (the
// low byte of word 15) is the PIF's status byte: a write leaves it 0, as the software PIF did ("taken": a game that looks
// finds nothing pending), but for bit 1, which the runtime clears when it has answered; the run flag that was in it (bit 0)
// and the challenge (bit 1) are what the PIF acts on:
//   `joy`  a block with the run flag has been written (by the DMA or by the CPU): the block holds joybus commands. Once set, it
//          stays: the PIF repeats the commands at every read of the block (libultra writes the request once and then only
//          reads, a frame after the other -- the replies it finds are of that read).
//   IO busy (STATUS bit 1) is the PIF working: the runtime is told (`ev`, a pulse: MONITOR.PEND's bit 4) and clears it when it
//          has written the replies into the PIF RAM (`done_we`, MONITOR.SIDONE). It is set at the end of a WR64B that carried
//          the run flag or the challenge, at a CPU write of word 15 with the challenge (a CIC-6105 cartridge's: the runtime
//          answers it in bytes 46-63 and clears the bit; the read that follows finds the answer, as after a WR64B's run of the
//          commands), and by an RD64B when `joy` is set and the PIF is not busy already (the commands run again before the
//          block is read: that read is deferred -- DMA busy, nothing moved -- until the PIF is done, as the console's SI waits
//          for the PIF; one that finds the PIF busy waits for that run, and the first read after the run a WR64B made does not
//          run them again: its replies are there). A start of either DMA waits for the PIF to be done.
//   `pif_n` the console's time: SIDONE's write carries how long the console's PIF takes over those commands (RCP cycles, the
//          runtime's estimate from what answered), counted down from then; an RD64B moves nothing until it is out (DMA busy,
//          as the console's read is held while the PIF talks to the joybus: ~0.6 ms for one controller read, ~1.55 ms for four), so
//          a game's thread that polls the controllers in a loop leaves the others the time it would on the console. The run
//          a WR64B made is counted from there, which is the console's time for libultra's write-then-read. A WR64B moves its
//          16 words WR_WAIT cycles after the start, the console's 4,065 to the interrupt less the move's own.
// The SI interrupt (`irq`, a pulse: MI_INTR's SI bit; the flag in STATUS bit 12) rises at the end of each DMA.
//
// The DMA is a master of the Pocket bridge's port (mirlo_mips.sv), a word at a time, ~20 cycles each: `req` is held until `ack`
// (a read: the word is `rdata` in the cycle of the ack, and goes into the PIF RAM in it; a write: `wdata` is the PIF RAM's word,
// held while the request is up).
`default_nettype none

module rcp_si (
    input  wire         clk, rst,
    // the game's / the runtime's access to the registers (one cycle for a write, the address held; the read is combinational)
    input  wire  [2:0]  a,                  // x_addr[4:2]
    input  wire  [31:0] wd,
    input  wire         wr_s,
    output logic [31:0] rd,
    output wire         clr,                // a write to STATUS
    output logic        irq,                // a pulse: a DMA is done
    // the CPU's word access to the PIF RAM: the address held, a write strobed for one cycle (only when !c_busy)
    input  wire  [3:0]  ca,                 // x_addr[5:2]
    input  wire         c_wr_s,
    input  wire  [31:0] c_wd,
    output wire  [31:0] c_q,
    output wire         c_busy,
    // the runtime's
    input  wire         done_we,            // the PIF is done: IO busy clears
    input  wire  [17:0] done_wd,            // with it: the console's time for the commands (RCP cycles)
    output logic        ev,                 // a pulse: the PIF has commands to run
    // the SDRAM, a word at a time (the bridge's port)
    output wire         req, we,
    output wire  [23:0] adr,
    output wire  [31:0] wdata,
    input  wire         ack,
    input  wire  [31:0] rdata
);

localparam logic [17:0] WR_WAIT = 18'd3830;   // (a WR64B's 4,065 to the interrupt, less the ~235 the move takes here)
typedef enum logic [1:0] { S_IDLE, S_WAIT, S_A, S_REQ } st_t;
st_t         st;
logic        dir_rd;                    // the DMA under way is PIF RAM -> RDRAM
logic [3:0]  idx;                       // the word
logic [21:0] dram;                      // DRAM_ADDR [23:2]: the RDRAM's word the DMA is at -- it counts up as the DMA goes (a register: the port's address comes from it)
logic        polled;                    // the PIF has run its commands for this RD64B
logic        io_busy, err, irq_f, joy;
logic        wr_org, fresh;             // the PIF's run in progress began at the end of a WR64B; its replies are there for the next RD64B, not yet read
logic [17:0] pif_n;                     // the console's PIF (or WR64B) time still to go
wire         busy = st == S_A || st == S_REQ;       // the DMA owns the PIF RAM
assign c_busy = busy;
assign clr = wr_s && a == 3'd6;

// ---------------------------------------------------------------- PIF RAM: a block RAM of 16 words
// Two ports, never writing in one cycle: the CPU's (port A: its word, at ca) and the DMA's (port B: the engine's word, at idx), each
// read at its own address at every clock -- no choice of address or data in front of the RAM
(* ramstyle = "M10K, no_rw_check" *) reg [31:0] m [0:15];
reg [31:0] qa, qb;
integer ii;
initial for (ii = 0; ii < 16; ii = ii + 1) m[ii] = 32'd0;
function automatic logic [31:0] w15m(input logic [3:0] wa, input logic [31:0] wd);     // word 15's byte 63 as the PIF leaves it
    return wa == 4'd15 ? {wd[31:8], 6'd0, wd[1], 1'b0} : wd;
endfunction
wire         eng_wr = st == S_REQ && ack && !dir_rd;
always @(posedge clk) begin
    if (c_wr_s && !busy) m[ca] <= w15m(ca, c_wd);
    qa <= m[ca];
end
always @(posedge clk) begin
    if (eng_wr) m[idx] <= w15m(idx, rdata);
    qb <= m[idx];
end
assign c_q   = qa;
assign req   = st == S_REQ;
assign we    = dir_rd;
assign adr   = {3'b111, dram[20:0]};
assign wdata = qb;

// ---------------------------------------------------------------- the registers, the DMA
always_comb begin
    rd = 32'd0;
    if (a == 3'd0) rd = {8'd0, dram, 2'b00};
    else if (a == 3'd6) rd = {19'd0, irq_f, 8'd0, err, 1'b0, io_busy, st != S_IDLE};
end
wire start_rd = wr_s && a == 3'd1;
wire start_wr = wr_s && a == 3'd4;
always_ff @(posedge clk) begin
    irq <= 1'b0; ev <= 1'b0;
    if (rst) begin
        st <= S_IDLE; io_busy <= 1'b0; err <= 1'b0; irq_f <= 1'b0; joy <= 1'b0; polled <= 1'b0; wr_org <= 1'b0; fresh <= 1'b0; dram <= '0; dir_rd <= 1'b0; idx <= '0;
        pif_n <= '0;
    end else begin
        if (pif_n != 18'd0) pif_n <= pif_n - 18'd1;
        if (done_we) begin io_busy <= 1'b0; pif_n <= done_wd; if (wr_org && st == S_IDLE) fresh <= 1'b1; end
        if (clr) begin irq_f <= 1'b0; err <= 1'b0; end
        if (wr_s && a == 3'd0 && st == S_IDLE) dram <= {wd[23:3], 1'b0};       // (the DMA owns it while it is under way)
        // the CPU's word 15 with the run flag: the block holds commands
        if (c_wr_s && !busy && ca == 4'd15 && c_wd[0]) begin joy <= 1'b1; fresh <= 1'b0; end
        // ... or with the challenge's bit: the PIF answers it at once
        if (c_wr_s && !busy && ca == 4'd15 && c_wd[1]) begin io_busy <= 1'b1; ev <= 1'b1; wr_org <= 1'b1; end
        if (start_rd || start_wr) begin
            if (st != S_IDLE) err <= 1'b1;
            else begin
                dir_rd <= start_rd; polled <= io_busy || (start_rd && fresh); fresh <= 1'b0; st <= S_WAIT;    // (the PIF is running its commands, or has just run them: that run is the read's)
                if (start_wr && !io_busy) pif_n <= WR_WAIT;
            end
        end
        case (st)
        S_WAIT: if (!io_busy) begin
            if (dir_rd && joy && !polled) begin polled <= 1'b1; io_busy <= 1'b1; ev <= 1'b1; wr_org <= 1'b0; end   // the commands run again, then the read
            else if (pif_n == 18'd0) begin idx <= 4'd0; st <= S_A; end
        end
        S_A: st <= S_REQ;
        S_REQ: if (ack) begin
            dram <= dram + 22'd1;
            if (!dir_rd && idx == 4'd15 && rdata[0]) joy <= 1'b1;
            if (!dir_rd && idx == 4'd15 && rdata[1:0] != 2'd0) begin io_busy <= 1'b1; ev <= 1'b1; wr_org <= 1'b1; end
            if (idx == 4'd15) begin st <= S_IDLE; irq <= 1'b1; irq_f <= 1'b1; end
            else begin idx <= idx + 4'd1; st <= S_A; end
        end
        default: ;
        endcase
    end
end

endmodule
`default_nettype wire
