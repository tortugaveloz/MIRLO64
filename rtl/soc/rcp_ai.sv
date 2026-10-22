// The N64's AI (Audio Interface) in hardware (MIRLO_AI; docs/mips.md "The AI in hardware"): the registers, the two-buffer
// queue, the DMA reader and the resampler to the Pocket's 48 kHz FIFO.
//
// AI, 0x0450_0000 (the game's lw / sw reach it as a device access, as the MI's and the VI's: mips_core's AIW, mirlo_mips.sv's
// decode; the registers repeat through the 1 MiB):
//   00 DRAM_ADDR  write: the buffer's address (8-byte aligned; the RDRAM's 8 MiB)
//   04 LEN        write: the length in bytes (a multiple of 8; 0 is ignored): the DMA starts with it when the AI is idle,
//                 else it is queued as the second buffer (a third is dropped, as the real one's). Read: the bytes left
//                 of the buffer being played
//   08 CONTROL    bit 0 DMA enable (clear: the reader stops where it is)
//   0C STATUS     read {full [31], busy [30], 0, DMA on [25], 1 [24], 1 [20], full [0]}: full = a second buffer is queued,
//                 busy = a buffer is being played (the console's: the bits 24 and 20 read 1 there). Any write
//                 clears the AI interrupt (MI_INTR's AI bit: `clr`)
//   10 DACRATE    write [13:0]: the DAC's clock divisor: the sample rate is 48,681,812 / (DACRATE + 1). The runtime is woken
//                 (`chg`) to compute the resampler's step from it and write it (`step_we`)
//   14 BITRATE    write: ignored
// A read of any register but STATUS is AI_LEN, as the console's (the others are write-only there).
//
// What it plays is what the runtime's software AI did (n64rt_rsp.c ai_service / fetch_frame), step for step, so that the
// output is the same sample stream: the Pocket's FIFO is kept at its target (256 words, 5.3 ms: the reader leads the heard
// audio by that, and so do AI_LEN and the interrupts; 512 words, 10.7 ms, before 2026-10-09) by time, not by reading ahead -- an output sample is made whenever the FIFO is below it. A sample is made from the frame pair (previous, next)
// of the game's 16-bit stereo frames (L high, R low) by a phase accumulator, linearly:
//     out = prev + ((next - prev) * (phase >> 1) >> 15)        phase: Q16 frames, += step per output sample
// where step = rate * 65536 / 48000 (the runtime's). The reader fetches a frame, a word at a time, from the RDRAM in the SDRAM,
// as the PI's engine does (a master of the Pocket bridge's port, `req` held until `ack`). When the AI has nothing to play
// (no buffer, or the DMA disabled) it writes silence to hold the FIFO's lead, for an eighth of a second (6000 words),
// then nothing until the next buffer (and the FIFO is first primed with silence up to the target, as the software did).
// The AI interrupt (`irq`, a pulse) is the console's: it rises when a buffer is started on an
// idle AI (the LEN write) and when the buffer read to its end is replaced by the one queued; when the queue empties there is
// none. (Before 2026-10-09 it rose at the end of every buffer, the last included, and not at a start: a driver that queues
// its next buffer from the interrupt, as Nintendo's HVQM2 timekeeper does, got a silent gap after each buffer.) The
// buffers are the console's fillcount: a buffer is current from its LEN write to the fetch of its last word.
`default_nettype none

module rcp_ai (
    input  wire         clk, rst,
    // the game's / the runtime's access (mirlo_mips.sv: one cycle for a write, the address held; the read is combinational)
    input  wire  [2:0]  a,                  // x_addr[4:2]
    input  wire  [31:0] wd,
    input  wire         wr_s,
    output logic [31:0] rd,
    output wire         clr,                // a write to STATUS
    output logic        irq,                // a pulse: a buffer has been played to its end
    output logic        chg,                // a pulse: DACRATE was written
    // the runtime's: the resampler's step (Q16 source frames per output sample), DACRATE for it to read
    input  wire         step_we,
    input  wire  [19:0] step_wd,
    output wire  [13:0] dac,
    // the Pocket's audio FIFO (its write side, in this clock): the words in it, a write
    input  wire  [9:0]  fill,
    output logic        fw,
    output logic [31:0] fd,
    // the SDRAM, a word at a time (the bridge's port)
    output wire         req,
    output wire  [23:0] adr,
    input  wire         ack,
    input  wire  [31:0] rdata
);

// ---------------------------------------------------------------- the registers and the queue
logic [19:0] dram;                      // DRAM_ADDR [22:3]
logic [13:0] dac_r;
logic        ena;
logic [20:0] c_adr, n_adr;              // the buffers' words (the RDRAM's word, 21 bits)
logic [15:0] c_left, n_len;             // words left of the current, the queued one's length
logic        c_v, n_v;                  // the current is loaded (it may be read to its end and not yet gone from), a second is queued
logic [19:0] step;
assign dac = dac_r;
assign clr = wr_s && a == 3'd3;
wire         c_avail = c_v;                 // (a current buffer always has words left: it is dropped at its last)
// the last word of the current buffer is being fetched: the buffer is gone from, the one queued (if any) is next
wire         c_end   = st == S_FRD && ack && c_left == 16'd1;
wire         len_nz  = wd[17:3] != 15'd0;

always_comb begin
    rd = {14'd0, c_left, 2'b00};
    if (a == 3'd3) rd = {n_v, c_v, 4'd0, ena, 1'b1, 3'd0, 1'b1, 19'd0, n_v};
end

// ---------------------------------------------------------------- the reader and the resampler
typedef enum logic [3:0] {
    S_OFF,      // nothing played: wait for a buffer
    S_PRIME,    // the FIFO's lead: silence up to the target
    S_TOP,      // below the target: make a sample
    S_ADV,      // the pair of frames this sample falls between: the loop's head
    S_FCHK,     // a frame: is there one (the buffer read to its end is gone from, the one queued is next)
    S_FRD,      // ... its word, from the SDRAM
    S_MUL0,     // a channel: the difference and the phase,
    S_MUL1,     // the product,
    S_MUL2,     // the sample (both channels in the FIFO's word)
    S_WR,       // written
    S_DRY       // the AI is out of buffers: silence
} st_t;
st_t         st;
logic        have, fnext;               // the first pair has been fetched; the fetch under way is the first's
logic [20:0] phase;
logic [15:0] pl, pr, nl, nr;
logic [12:0] dry;
logic [2:0]  wt;                        // cycles to wait after a write for the FIFO's fill to show it
logic        priming;                   // the silence under way is the FIFO's lead
logic        sil /*verilator public_flat_rd*/;   // (the bench's: the word being written is silence; no fan-out: no logic)
logic        have_rst /*verilator public_flat_rd*/;  // (the bench's: the reader started over)
logic        rch;                       // the channel being made: 0 L, 1 R
logic signed [16:0] ma;
logic signed [15:0] mb;
logic signed [32:0] mp;
wire         below = !(fill[9] | fill[8]);  // fewer than 256 words in the FIFO
assign req = st == S_FRD;
assign adr = {3'b111, c_adr};
wire  [15:0] mix = (rch ? pr : pl) + mp[30:15];

always_ff @(posedge clk) begin
    irq <= 1'b0; chg <= 1'b0; fw <= 1'b0;
    if (rst) begin
        st <= S_OFF; sil <= 1'b0; have_rst <= 1'b0; priming <= 1'b0; have <= 1'b0; dry <= '0; c_v <= 1'b0; n_v <= 1'b0; c_left <= '0; ena <= 1'b0; wt <= '0;
        step <= 20'd43690; dac_r <= '0;
    end else begin
        if (step_we) step <= step_wd;
        if (wt != 3'd0) wt <= wt - 3'd1;
        case (st)
        S_OFF:   if (ena && (c_avail || n_v)) begin priming <= 1'b1; st <= S_PRIME; end
        S_PRIME: if (wt == 3'd0) begin
            if (below) begin fd <= 32'd0; sil <= 1'b1; st <= S_WR; end
            else begin dry <= '0; priming <= 1'b0; st <= S_TOP; end
        end
        S_TOP:   if (wt == 3'd0 && below) st <= ena ? S_ADV : S_OFF;
        S_ADV:   if (!have) begin fnext <= 1'b1; st <= S_FCHK; end
                 else if (phase[20:16] != 5'd0) begin fnext <= 1'b0; st <= S_FCHK; end
                 else begin rch <= 1'b0; st <= S_MUL0; end
        S_FCHK:  st <= c_avail ? S_FRD : S_DRY;
        S_FRD:   if (ack) begin
            c_adr <= c_adr + 21'd1; c_left <= c_left - 16'd1;
            if (c_end) begin                // NEXTDMA: the queued buffer goes on (and the interrupt), or the AI is idle
                if (n_v) begin c_adr <= n_adr; c_left <= n_len; n_v <= 1'b0; irq <= 1'b1; end
                else c_v <= 1'b0;
            end
            pl <= nl; pr <= nr; nl <= rdata[31:16]; nr <= rdata[15:0];
            if (fnext) begin have <= 1'b1; phase <= 21'h10000; end
            else phase <= phase - 21'h10000;
            st <= S_ADV;
        end
        S_MUL0:  st <= S_MUL1;     // (the operands and the product: below)
        S_MUL1:  st <= S_MUL2;
        S_MUL2: begin
            if (!rch) begin fd[31:16] <= mix; rch <= 1'b1; st <= S_MUL0; end
            else begin fd[15:0] <= mix; sil <= 1'b0; phase <= phase + {1'b0, step}; dry <= '0; st <= S_WR; end
        end
        S_WR: begin fw <= 1'b1; wt <= 3'd4; st <= priming ? S_PRIME : S_TOP; end
        S_DRY: begin
            if (dry >= 13'd6000) begin have <= 1'b0; have_rst <= !have_rst; st <= S_OFF; end
            else begin dry <= dry + 13'd1; fd <= 32'd0; sil <= 1'b1; st <= S_WR; end
        end
        default: st <= S_OFF;
        endcase
        // the game's writes (a buffer written while the reader moves on from one: see the module's header)
        if (wr_s) case (a)
        3'd0: dram <= wd[22:3];
        3'd1: if (len_nz) begin
            if (!c_v || (c_end && !n_v)) begin c_adr <= {dram, 1'b0}; c_left <= {wd[17:3], 1'b0}; c_v <= 1'b1; irq <= 1'b1; end   // an idle AI: it starts, the interrupt
            else if (!n_v || c_end) begin n_adr <= {dram, 1'b0}; n_len <= {wd[17:3], 1'b0}; n_v <= 1'b1; end
        end
        3'd2: ena <= wd[0];
        3'd4: begin dac_r <= wd[13:0]; chg <= 1'b1; end
        default: ;
        endcase
    end
end
// A channel's difference and phase (S_MUL0's), the product (S_MUL1's): taken at every clock, nothing in between -- S_MUL1 sees
// the operands S_MUL0 took (what they come from does not change in S_MUL0), S_MUL2 the product of S_MUL1 -- so that the
// multiplier's registers are its DSP block's own
always_ff @(posedge clk) begin
    ma <= rch ? $signed({nr[15], nr}) - $signed({pr[15], pr}) : $signed({nl[15], nl}) - $signed({pl[15], pl});
    mb <= $signed({1'b0, phase[15:1]});
    mp <= ma * mb;
end

endmodule
`default_nettype wire
