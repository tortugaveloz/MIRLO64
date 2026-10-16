// The N64's MI (MIPS Interface) in hardware, and with PI the cartridge DMA's engine
// (MIRLO_MI, MIRLO_PI; docs/mips.md "The MI and the PI DMA in hardware").
//
// MI, 0x0430_0000 (the game's lw / sw reach it as a device access: mips_core's MI,
// mirlo_mips.sv's decode; the 1 MiB of its block is the registers, repeated):
//   +0 MODE     read {upper, ebus, init, count[6:0]}; write: [6:0] count, 7/8 clear/set init, 9/10 clear/set
//               ebus test, 11 clear the DP interrupt, 12/13 clear/set upper mode
//   +4 VERSION  0x02020102
//   +8 INTR     read: the pending interrupts [0] SP [1] SI [2] AI [3] VI [4] PI [5] DP
//   +C MASK     read: the mask; write: bit 2i clears and 2i+1 sets the mask bit of the interrupt i
// The line, (INTR & MASK) != 0, is the game's Cause.IP2.
//   SP     the RSP's own interrupt, as it is (a level)
//   VI, DP a pulse of the scan-out's field / of the RDP's SYNC FULL sets them. The game clears VI with a write
//          to VI_CURRENT, AI with one to AI_STATUS, SI with one to SI_STATUS -- those registers are the
//          runtime's, which does it through MIRAISE / MICLR below (and sets SI, AI, and PI of a copy it did itself);
//          with the AI in hardware (rcp_ai.sv) AI is set by a buffer played to its end and cleared by the write to AI_STATUS;
//          with the SI in hardware (rcp_si.sv) SI is set at the end of each DMA and cleared by the write to SI_STATUS
//   PI     the engine's end, or the runtime's (a copy it did itself); cleared by a write of bit 1 to PI_STATUS
// PI_STATUS, 0x0460_0010: read {intr, error 0, io busy 0, dma busy}; write bit 0 aborts the engine's copy,
// bit 1 clears the interrupt.
// The engine copies a run of words from the ROM in the SDRAM to the RDRAM, a word at a time: PIDST (the
// RDRAM's word, 21 bits), PISRC (the SDRAM's word), PILEN (words: the write starts it). Its accesses are the
// Pocket bridge's port's, when the bridge is idle (mirlo_mips.sv): d_req held until d_ack.
// PI2 (MIRLO_PI2; docs/mips.md "The PI's domain 2 and the cartridge's reads"): the copy is any SDRAM's to any SDRAM's --
// PIDST is the SDRAM's word when its bits [23:21] are not 0 (0: the RDRAM's word, as before), so the runtime also writes
// the RDRAM into the save's memory (PI_RD_LEN) -- from a source that may start at a word's second half (PISRC bit 24:
// a cartridge address 2 mod 4; a 16-bit carry register holds the half that goes into the next word, and the copy reads
// one word more), and of any length in bytes (PILEN [23:22]: the bytes of the last word, 0 a whole one; its byte enables).
// It also does the game CPU's word accesses to the cartridge's bus (mirlo_mips.sv's X_CART: io_*), one at a time
// between copies -- a read waits for the copy under way to end, as libultra's osPiReadIo waits for the PI.
// ROM64 (with PI2; docs/mips.md "The ROM"): a ROM's first 32 MiB are in the PSRAM, at the Pocket's bridge address mod 32 MiB
// -- the SDRAM word the ROM's word would have at 1 MiB, its 23 low bits. PISRC bit 25 says the copy's source is there (src's
// 23 low bits: the PSRAM's word; a copy that would start there and end in the SDRAM is the runtime's), and io_ps that a CPU
// read is (io_adr's 23 low bits); d_ps marks such a read for mirlo_mips.sv, which does it at the PSRAM in place of the
// SDRAM. Every write is the SDRAM's.
`default_nettype none

module rcp_mi #(
    parameter bit PI = 0,
    parameter bit PI2 = 0,
    parameter bit ROM64 = 0
) (
    input  wire         clk, rst,
    input  wire         sp_lvl, vi_pulse, dp_pulse,
    input  wire         vi_clr,                 // a write to VI_CURRENT (rcp_vi.sv)
    input  wire         ai_pulse, ai_clr,       // MIRLO_AI (rcp_ai.sv): a buffer played to its end; a write to AI_STATUS
    input  wire         si_pulse, si_clr,       // MIRLO_SI (rcp_si.sv): a DMA done; a write to SI_STATUS
    // the runtime's registers (mirlo_regs.sv)
    input  wire         raise_we, clr_we,
    input  wire  [5:0]  csr_bits,
    input  wire         dst_we, src_we, len_we,
    input  wire  [25:0] csr_wd,
    output wire  [5:0]  intr, mask_o,
    output wire         pi_busy,
    output wire  [20:0] pi_cnt,
    // the game's access (one cycle; the answer is the read data of the same cycle)
    input  wire         w_mi, w_pi, w_we,
    input  wire  [1:0]  w_a,
    input  wire  [31:0] w_wd,
    output logic [31:0] w_rd,
    output wire         line,
    // the engine's access to the SDRAM (via the bridge's port)
    output wire         d_req, d_we,
    output wire  [23:0] d_adr,
    output wire         d_ps,                   // ROM64: the read is the PSRAM's (d_adr's 23 low bits)
    output wire  [3:0]  d_sel,
    output wire  [31:0] d_wdata,
    input  wire  [31:0] d_rdata,                // (the port's read data: in the cycle of d_ack and after it)
    input  wire         d_ack,
    // PI2: the game CPU's access to the cartridge's bus (held until io_ack, which is d_ack's cycle: the data is d_rdata)
    input  wire         io_req, io_we,
    input  wire  [23:0] io_adr,
    input  wire         io_ps,                  // ROM64: the read is the PSRAM's
    input  wire  [31:0] io_wd,
    output wire         io_ack
);

typedef enum logic [2:0] { P_IDLE, P_RD, P_WR, P_PRIME, P_IO } pst_t;
logic [5:1] pend;
logic [5:0] mask;
logic [6:0] count;
logic       init_m, ebus, upper;
assign intr   = {pend[5:1], sp_lvl};
assign mask_o = mask;
assign line   = |(intr & mask);

wire mi_wr = w_mi && w_we, pi_wr = w_pi && w_we;
logic pi_done;
logic [5:1] set_b, clr_b;
always_comb begin
    set_b = '0; clr_b = '0;
    if (raise_we) set_b = csr_bits[5:1];
    if (vi_pulse) set_b[3] = 1'b1;
    if (dp_pulse) set_b[5] = 1'b1;
    if (ai_pulse) set_b[2] = 1'b1;
    if (si_pulse) set_b[1] = 1'b1;
    if (pi_done)  set_b[4] = 1'b1;
    if (clr_we) clr_b = csr_bits[5:1];
    if (vi_clr) clr_b[3] = 1'b1;
    if (ai_clr) clr_b[2] = 1'b1;
    if (si_clr) clr_b[1] = 1'b1;
    if (mi_wr && w_a == 2'd0 && w_wd[11]) clr_b[5] = 1'b1;
    if (pi_wr && w_wd[1]) clr_b[4] = 1'b1;
end
always_ff @(posedge clk) begin
    if (rst) begin pend <= '0; mask <= '0; count <= '0; init_m <= 1'b0; ebus <= 1'b0; upper <= 1'b0; end
    else begin
        pend <= (pend & ~clr_b) | set_b;
        if (mi_wr && w_a == 2'd3)
            for (int i = 0; i < 6; i++) begin
                if (w_wd[2*i])     mask[i] <= 1'b0;
                if (w_wd[2*i + 1]) mask[i] <= 1'b1;
            end
        if (mi_wr && w_a == 2'd0) begin
            count <= w_wd[6:0];
            if (w_wd[7]) init_m <= 1'b0;
            if (w_wd[8]) init_m <= 1'b1;
            if (w_wd[9]) ebus <= 1'b0;
            if (w_wd[10]) ebus <= 1'b1;
            if (w_wd[12]) upper <= 1'b0;                    // MI_CLR_RDRAM (libultra 0x1000)
            if (w_wd[13]) upper <= 1'b1;                    // MI_SET_RDRAM (0x2000)
        end
    end
end

always_comb begin
    w_rd = 32'd0;
    if (w_mi) case (w_a)
        2'd0: w_rd = {22'd0, upper, ebus, init_m, count};
        2'd1: w_rd = 32'h0202_0102;
        2'd2: w_rd = {26'd0, intr};
        default: w_rd = {26'd0, mask};
    endcase
    else if (w_pi) w_rd = {28'd0, pend[4], 2'b00, pi_busy};
end

// ---------------------------------------------------------------- the PI's engine
generate if (PI) begin : g_pi
    pst_t        st;
    logic [20:0] cnt;
    logic [23:0] dst, src;
    logic        abort;
    logic [1:0]  tail;                          // PI2: the last word's bytes (0: all four)
    logic        half, half_n;                  // PI2: the source starts at a word's second half (the next copy's)
    logic [15:0] carry;                         // PI2: the source word's second half, the next word's first
    logic        ps;                            // ROM64: the source is the PSRAM's
    wire         last = cnt == 21'd1;
    assign pi_busy = st != P_IDLE && st != P_IO;
    assign pi_cnt  = cnt;
    assign d_req   = st != P_IDLE;
    assign d_we    = st == P_WR || (PI2 && st == P_IO && io_we);
    assign d_adr   = st == P_WR ? dst : (PI2 && st == P_IO) ? io_adr : src;
    assign d_ps    = PI2 && ROM64 && (st == P_IO ? io_ps : st != P_WR && ps);
    assign d_sel   = PI2 && st == P_WR && last && tail != 2'd0 ? {1'b1, tail[1], tail == 2'd3, 1'b0} : 4'hF;
    assign d_wdata = (PI2 && st == P_IO) ? io_wd : (PI2 && half) ? {carry, d_rdata[31:16]} : d_rdata;
    assign io_ack  = PI2 && st == P_IO && d_ack;
    always_ff @(posedge clk) begin
        pi_done <= 1'b0;
        if (rst) begin st <= P_IDLE; abort <= 1'b0; half_n <= 1'b0; end
        else begin
            if (pi_wr && w_wd[0] && pi_busy) abort <= 1'b1;
            if (dst_we) dst <= {PI2 && csr_wd[23:21] != 3'd0 ? csr_wd[23:21] : 3'b111, csr_wd[20:0]};
            if (src_we) begin src <= csr_wd[23:0]; half_n <= PI2 && csr_wd[24]; ps <= PI2 && ROM64 && csr_wd[25]; end
            case (st)
            P_IDLE: begin
                abort <= 1'b0;
                if (len_we && csr_wd[20:0] != 21'd0) begin
                    cnt <= csr_wd[20:0]; tail <= csr_wd[23:22]; half <= half_n;
                    st <= (PI2 && half_n) ? P_PRIME : P_RD;
                end else if (PI2 && io_req && !io_ack) st <= P_IO;
            end
            P_PRIME: if (d_ack) begin              // (PI2) the first word: only its second half is copied, with the next
                carry <= d_rdata[15:0]; src <= src + 24'd1;
                st <= abort ? P_IDLE : P_RD;
            end
            P_RD: if (d_ack) begin
                if (abort) st <= P_IDLE; else st <= P_WR;
            end
            P_WR: if (d_ack) begin
                carry <= d_rdata[15:0];
                src <= src + 24'd1; dst <= dst + 24'd1; cnt <= cnt - 21'd1;
                if (abort || last) begin st <= P_IDLE; pi_done <= !abort; end
                else st <= P_RD;
            end
            P_IO: if (d_ack) st <= P_IDLE;
            default: st <= P_IDLE;
            endcase
        end
    end
end else begin : g_nopi
    assign pi_busy = 1'b0; assign pi_cnt = 21'd0;
    assign d_req = 1'b0; assign d_we = 1'b0; assign d_adr = 24'd0; assign d_ps = 1'b0;
    assign d_sel = 4'hF; assign d_wdata = 32'd0; assign io_ack = 1'b0;
    assign pi_done = 1'b0;
end endgenerate

endmodule
`default_nettype wire
