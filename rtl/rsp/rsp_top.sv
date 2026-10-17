// rsp_top: Mirlo's RSP. The N64's Reality Signal Processor as software sees
// it -- the scalar unit (rsp_su), the vector unit (rsp_vu), IMEM and DMEM
// (4 KiB each), COP0's registers and the DMA engine -- so a game's own
// graphics and audio microcode runs on it. The contract is the reference
// model, sim/rsp/rsp.c.
//
// Memory holds whole words, as the SoC's SDRAM does (a big-endian word's
// first byte in [31:24]). DMEM is 16 bytes wide, as the N64's: an access
// takes the 16 bytes from addr's word on, so any 1 to 4 bytes at any byte
// address are one cycle (the RSP's loads and stores need no alignment), and
// so is every vector load and store but LTV / STV.
//
// The host (the game CPU) reaches the SP registers through h_*:
//   0 SP_MEM_ADDR  1 SP_DRAM_ADDR  2 SP_RD_LEN  3 SP_WR_LEN  4 SP_STATUS
//   5 SP_DMA_FULL  6 SP_DMA_BUSY   7 SP_SEMAPHORE            8 SP_PC
// with the N64's meanings (a write to 2 or 3 starts a DMA; SP_STATUS's write
// bits set and clear halt, broke, the interrupt, the signals). It loads the
// memories by DMA, as libultra does.
//
// The DMA registers are double: a write while a DMA runs goes to the next
// one's (SP_MEM_ADDR, SP_DRAM_ADDR; RD_LEN / WR_LEN queue it: SP_DMA_FULL is
// set, it starts when the running one is done, and a write while FULL replaces it),
// the reads give the running one's, as on the console. The microcode's own
// DMA is queued the same way and runs while the microcode goes on, as the
// console's: it waits on SP_DMA_BUSY / SP_DMA_FULL itself.
// A DMA into or out of DMEM while the microcode runs shares DMEM's port
// with it: the DMA has it (a beat of a transfer in; the whole transfer out) and the
// scalar and vector units' loads and stores wait (m_stall) instead of being lost.
// SP_SEMAPHORE is taken by a read of the host's too (h_rd), as the microcode's.
//
// The RDP's side (DPC) is outside: the microcode's writes to DPC_START,
// DPC_END and DPC_STATUS come out as strobes, DPC_CURRENT and DPC_STATUS as
// it reads them come in.
`default_nettype none

module rsp_top #(
    parameter int LANES = 4,
    parameter bit MULW = 0,                 // (4 lanes) the multiplies on eight
    parameter     DIV_ROM = "rsp_div_rom.hex"
) (
    input  wire         clk,
    input  wire         rst,

    // the host's register port (one cycle; reads combinational)
    input  wire         h_we,
    input  wire  [3:0]  h_addr,
    input  wire  [31:0] h_wdata,
    output logic [31:0] h_rdata,
    input  wire         h_rd,               // the host reads a register this cycle (SP_SEMAPHORE is taken by it)
    output logic        h_ready,            // a write is taken (not while the microcode writes a register itself)
    output logic        irq,                // SP interrupt (BREAK with intr-on-break, or set by SP_STATUS)

    // RDRAM (the SoC's SDRAM, a port of its arbiter): word requests taken
    // with r_ready, a read's word back later with r_rvalid, in order
    output logic        r_valid,
    output logic        r_we,
    output logic [23:2] r_addr,
    output logic [31:0] r_wdata,
    input  wire         r_ready,
    input  wire         r_rvalid,
    input  wire  [31:0] r_rdata,

    // the RDP's registers
    output logic        dpc_start_we,       // DPC_START written (dpc_wdata)
    output logic        dpc_end_we,         // DPC_END written
    output logic        dpc_status_we,      // DPC_STATUS written (its set / clear bits)
    output logic [31:0] dpc_wdata,
    input  wire  [31:0] dpc_start,
    input  wire  [31:0] dpc_end,
    input  wire  [31:0] dpc_current,
    input  wire  [31:0] dpc_status,
    // the RDP's reads of DMEM (XBUS: its commands taken from DMEM, not
    // RDRAM): a word, the cycle after its address
    input  wire  [9:0]  dx_addr,
    output logic [31:0] dx_data,

    output logic        halted,
    output logic        unimpl,             // an instruction the vector unit does not implement ran

    // (simulation) the instructions as they leave E, the register writes
    output logic        tr_e,
    output logic [11:0] tr_e_pc,
    output logic        tr_e2,              // ... and the next one with it (a pair: tr_e_pc + 4)
    output logic        tr_e_we,
    output logic [4:0]  tr_e_wr,
    output logic        tr_we,
    output logic [4:0]  tr_wr,
    output logic [31:0] tr_val,
    output logic [3:0]  tr_v,               // vector-unit instructions whose effects are all in with this cycle
    output logic        tr_vp               // ... the registers' only: the accumulator and flags are a later one's already
);

// ---------------------------------------------------------------- units --
logic        su_run, su_brk, su_idle, pc_load;
logic [11:0] pc_next;
logic [9:0]  i_addr;
logic [31:0] i_data, i_data2;
logic        i2_ok;
logic        sm_valid, sm_we;       logic [11:0] sm_addr; logic [1:0] sm_n; logic [31:0] sm_wdata;
logic        vm_valid, vm_we;       logic [11:0] vm_addr; logic [15:0] vm_be; logic [127:0] vm_wdata; logic [3:0] vm_rot;
logic [31:0] m_rdata;
logic [127:0] m_rdata16;
logic        c_valid, c_done;
logic [31:0] c_ins, c_a, c_b, c_res, c_lins;
logic        c_lv;
logic [4:0]  c_ra_n, c_rb_n, c_rb;
logic        dm_valid, dm_we;       logic [11:0] dm_addr; logic [31:0] dm_wdata;   // the DMA's (whole words): see the DMEM

rsp_su #(.LPAIR(LANES == 8)) su (.clk, .rst, .run(su_run), .pc_load, .pc_in(h_wdata[11:0]), .pc_next, .brk(su_brk), .idle(su_idle),
    .i_addr, .i_data, .i_data2, .i2_ok,
    .m_valid(sm_valid), .m_we(sm_we), .m_addr(sm_addr), .m_n(sm_n), .m_wdata(sm_wdata), .m_rdata, .m_stall(dm_valid),
    .c_valid, .c_ins, .c_a, .c_b, .c_lv, .c_lins, .c_done, .c_res, .c_ra_n, .c_rb_n, .c_rb,
    .tr_e, .tr_e_pc, .tr_e2, .tr_e_we, .tr_e_wr, .tr_we, .tr_wr, .tr_val);

// (what is not a computation -- COP0, a vector load, store or move -- is never a pair's computation: E's own instruction,
// c_lins, which c_ins only adds a select to)
wire         is_cop0 = c_lins[31:26] == 6'h10;
logic        v_done;
logic [31:0] v_res;
logic [9:0]  rom_addr;
logic [15:0] rom_q;
rsp_vu #(.LANES(LANES), .MULW(MULW)) vu (.clk, .rst, .valid(c_valid && !is_cop0), .ins(c_ins), .a(c_a), .b(c_b),
    .l_valid(c_valid && c_lv), .l_ins(c_lins), .ra_n(c_ra_n), .rb_n(c_rb_n), .rb(c_rb), .done(v_done), .res(v_res),
    .m_valid(vm_valid), .m_we(vm_we), .m_addr(vm_addr), .m_be(vm_be), .m_wdata(vm_wdata), .m_rot(vm_rot), .m_rdata(m_rdata16), .m_stall(dm_valid),
    .rom_addr, .rom_q, .unimpl, .commit(tr_v), .commit_part(tr_vp));

// the reciprocal ROM: 512 reciprocals, 512 inverse square roots (sim/rsp/make_div_rom.py)
logic [15:0] div_rom [0:1023];
initial $readmemh(DIV_ROM, div_rom);
always_ff @(posedge clk) rom_q <= div_rom[rom_addr];

// ----------------------------------------------------------------- IMEM --
// two ports: the fetch's, and the DMA's -- its writes (a transfer in) and
// its reads (a transfer out of IMEM: a word a cycle). With no DMA of IMEM's
// the second reads the word after the fetch's, for the scalar unit's pairs
logic [31:0] imem [0:1023];
logic        iw_we;
logic [9:0]  iw_addr;
logic [31:0] iw_data, ia_q;
logic        im_dma;                  // (below: a DMA of IMEM runs)
wire  [9:0]  ib_addr = im_dma ? iw_addr : i_addr + 10'd1;    // (one address for the port: a write is a DMA's, im_dma)
always_ff @(posedge clk) begin
    if (iw_we) begin imem[ib_addr] <= iw_data; ia_q <= iw_data; end
    else ia_q <= imem[ib_addr];
    i2_ok <= !rst && !im_dma && !iw_we;
end
always_ff @(posedge clk) i_data <= imem[i_addr];
assign i_data2 = ia_q;

// ----------------------------------------------------------------- DMEM --
// one access a cycle: the DMA engine's, the vector unit's or the scalar
// unit's. DMEM is 16 bytes wide (rsp_dpram: four banks, a word of each line
// in each), so an access takes the 16 bytes from x_addr's word on -- what the
// RSP's vector loads and stores need, each in one cycle: what they move lies
// in those 16 bytes. Read data comes back rotated by x_rot bytes (byte m of
// m_rdata16 is the one at DMEM address mod 16 = m + x_rot), so the vector
// unit has its register's bytes in place and the scalar unit its 1 to 4
// bytes in the top word; writes go by the byte's address mod 16 (x_be16,
// x_wd: byte b in [127 - 8b -: 8]).
// the instruction in E is the vector unit's load or store (a computation's, alone or in a pair, does not use DMEM)
wire         vu_own = c_valid && (c_lins[31:26] == 6'h32 || c_lins[31:26] == 6'h3A);
logic        x_valid, x_we;
logic [11:0] x_addr;
logic [15:0] x_be16;
logic [127:0] x_wd;
logic [3:0]  x_rot_n;
// the scalar unit's bytes: m_n + 1 from the address on; its word (and the
// DMA's) goes to all four banks rotated to the address, the enables pick
wire  [3:0]  sa = sm_addr[3:0];
wire  [31:0] s_rw = {sm_wdata, sm_wdata} >> {sa[1:0], 3'b000};
logic [15:0] s_be;
always_comb
    for (int b = 0; b < 16; b++) s_be[b] = 4'(4'(b) - sa) <= {2'd0, sm_n};
always_comb begin
    if (dm_valid)    begin x_valid = 1'b1; x_we = dm_we; x_addr = dm_addr; x_be16 = 16'h000F << {dm_addr[3:2], 2'b00};
                           x_wd = {4{dm_wdata}}; x_rot_n = dm_addr[3:0]; end
    else if (vu_own) begin x_valid = vm_valid; x_we = vm_we; x_addr = vm_addr; x_be16 = vm_be;
                           x_wd = vm_wdata; x_rot_n = vm_rot; end
    else             begin x_valid = sm_valid; x_we = sm_we; x_addr = sm_addr; x_be16 = s_be;
                           x_wd = {4{s_rw}}; x_rot_n = sa; end
end
// (x_be16 is in address order, bit b the byte at address mod 16 = b; bank k
// takes x_addr's row, or the next one for the words before x_addr's in its line)
wire  [9:0]  x_wa  = x_addr[11:2];
wire  [7:0]  x_wa1 = x_wa[9:2] + 8'd1;
logic [31:0] x_row;
always_comb
    for (int k = 0; k < 4; k++) x_row[8 * k +: 8] = (2'(k) < x_wa[1:0]) ? x_wa1 : x_wa[9:2];
logic [127:0] q16;
logic [3:0]  x_rot;
rsp_dpram dmem (.clk, .a_row(x_row), .a_we((x_valid && x_we) ? x_be16 : 16'd0), .a_dat_w(x_wd), .a_dat_r(q16),
    .b_adr(dx_addr), .b_dat_r(dx_data));
always_ff @(posedge clk) x_rot <= x_rot_n;
// byte m of the result is byte (m + x_rot) mod 16 of the line: by words, then bytes
logic [127:0] q_w, x_rd;
always_comb begin
    for (int m = 0; m < 16; m++) q_w[127 - 8 * m -: 8] = q16[127 - 8 * ((m + 4 * int'(x_rot[3:2])) & 15) -: 8];
    for (int m = 0; m < 16; m++) x_rd[127 - 8 * m -: 8] = q_w[127 - 8 * ((m + int'(x_rot[1:0])) & 15) -: 8];
end
assign m_rdata   = x_rd[127:96];
assign m_rdata16 = x_rd;

// ------------------------------------------------------------ registers --
logic        st_halt, st_broke, st_sstep, st_intbrk;
logic [7:0]  st_sig;
logic        sem;
logic [12:3] sp_addr, sp_addr_w;      // [12] IMEM; as it runs (what a read gives), and as last written (where a DMA starts)
logic [23:3] ram_addr, ram_addr_w;
logic [11:3] d_skip;
logic [31:0] status;
logic        dma_busy;                // a DMA runs
logic        p_full;                  // ... and one is queued behind it (SP_DMA_FULL)
wire         dma_bf = dma_busy || p_full;
assign status = {17'd0, st_sig, st_intbrk, st_sstep, 1'b0, p_full, dma_bf, st_broke, st_halt};
assign halted = st_halt;
assign su_run = !st_halt;

function automatic logic [31:0] cop0_rd(input logic [3:0] r);
    unique case (r)
    4'd0:  return {19'd0, sp_addr, 3'd0};
    4'd1:  return {8'd0, ram_addr, 3'd0};
    4'd2, 4'd3: return {d_skip, 3'd0, 20'h00FF8};          // as a DMA leaves them: the skip, the count 0, the length run out
    4'd4:  return status;
    4'd5:  return {31'd0, p_full};
    4'd6:  return {31'd0, dma_bf};          // (games check for busy right after starting)
    4'd7:  return {31'd0, sem};
    4'd8:  return dpc_start;
    4'd9:  return dpc_end;
    4'd10: return dpc_current;
    4'd11: return dpc_status;
    default: return 32'd0;
    endcase
endfunction

// COP0, from the scalar unit's E stage
wire         mfc0 = c_valid && is_cop0 && c_lins[25:21] == 5'd0;
wire         mtc0 = c_valid && is_cop0 && c_lins[25:21] == 5'd4;
wire  [3:0]  c0r  = c_lins[14:11];
wire         mt_dma = mtc0 && (c0r == 4'd2 || c0r == 4'd3);

// ------------------------------------------------------------------ DMA --
// rows of (len + 1) bytes, in words; `skip` bytes between rows in RDRAM. A
// write of SP_RD_LEN / SP_WR_LEN, the microcode's MTC0 or the host's, returns
// at once: the transfer runs behind it, and the writer polls SP_DMA_BUSY /
// SP_DMA_FULL (SP_STATUS) for its end.
//   in (RDRAM -> SP): a read request a cycle, each word written to DMEM /
//       IMEM as it comes back
//   out (DMEM -> RDRAM): DMEM is read 8 bytes at a time into a FIFO of two
//       pairs (pf), ahead of the pair being written, so a write request goes
//       out every cycle the memory takes one; DMEM's port is the DMA's only
//       in the cycles of those reads (one in two at most), and the
//       microcode's loads and stores go on in the others
//   out of IMEM: the pair's two words one after the other on IMEM's second
//       port (iq), so a pair every three cycles. Only a boot microcode's
//       check of the console does it (a CIC-6105 game's: eight bytes).
typedef enum logic [2:0] { D_IDLE, D_IN, D_OUT, D_END } dst_t;
dst_t        ds;
logic        d_half, q_half;          // the second word of the 8 bytes: in SP memory, in RDRAM
logic [9:0]  d_words;                 // requests left in the row, less one
logic [9:0]  d_row;                   // a row's words, less one
logic [7:0]  d_rows;                  // rows left, less one
logic        q_done;                  // every request is out
logic [9:0]  g_words;                 // words still to move in the row, less one
logic [7:0]  g_rows;                  // rows still to move, less one
wire         d_last = g_words == 10'd0 && g_rows == 8'd0;   // the word being moved is the last
// (counted as the requests are, by rows: the words in all, rows times a
// row's, took a multiplier from the length just written -- the design's
// longest path)
logic [63:0] ob;                      // out: the pair being sent
logic [1:0]  ob_n;                    // its words still to send
logic [8:0]  np;                      // the pair SP memory is asked for next
logic [63:0] pf [2];                  // out of DMEM: the pairs read ahead, a FIFO (pf_h its head, pf_n how many)
logic        pf_h, pf_rd;             // ... and a read went out last cycle (its pair on m_rdata16 now)
logic [1:0]  pf_n;
logic [1:0]  iq;                      // (out of IMEM) the pair's words read: 2, both
logic [31:0] ih_q;                    // ... the first
wire         d_im    = sp_addr[12];   // the transfer is IMEM's
assign im_dma = ds != D_IDLE && d_im;
wire         p_ok    = d_im ? iq == 2'd2 : pf_n != 2'd0;
// RD_LEN / WR_LEN (the microcode's or the host's) is taken into the pending registers (p_*: the length, count and
// skip, which transfer); the DMA starts from them when the running one is done -- the next cycle on an idle engine
logic [31:0] p_len;                   // the pending DMA's length word
logic        p_rd0;                   // ... and it is a read (RDRAM -> SP)
wire         len_w     = mt_dma || (h_we && !mtc0 && (h_addr == 4'd2 || h_addr == 4'd3));
wire         d_start_p = p_full && ds == D_IDLE;
wire  [31:0] d_len   = p_len;
wire         d_rd0   = p_rd0;
wire  [9:0]  sp_word = {sp_addr[11:3], d_half};
assign dma_busy = ds != D_IDLE;

assign r_valid  = (ds == D_IN && !q_done) || (ds == D_OUT && ob_n != 2'd0);
assign r_we     = ds == D_OUT;
assign r_addr   = {ram_addr, q_half};
assign r_wdata  = ob_n == 2'd2 ? ob[63:32] : ob[31:0];
wire         r_go    = r_valid && r_ready;                 // a request taken
wire         o_sent  = ds == D_OUT && r_go;                // out: a word written
wire         o_load  = ds == D_OUT && p_ok && (ob_n == 2'd0 || (ob_n == 2'd1 && r_go));
// a word back is written into SP memory the cycle after (wq_*): the units that use DMEM's port are told one cycle
// ahead, from a register (m_stall = dm_valid), and wait for the beat
logic        wq_v;
logic [31:0] wq_d;
always_ff @(posedge clk) begin
    wq_v <= !rst && ds == D_IN && r_rvalid;
    wq_d <= r_rdata;
end
wire         sp_wr   = wq_v;                               // a word is written into SP memory
// SP memory: DMEM through the access above, IMEM through its second port
// (IMEM is only ever written by a DMA)
wire         dm_rd   = ds == D_OUT && !d_im && 2'(pf_n + 2'(pf_rd)) < 2'd2;   // out of DMEM: a pair read (from registers: m_stall's)
assign dm_valid = (sp_wr && !d_im) || dm_rd;
assign dm_we    = sp_wr;
assign dm_addr  = ds == D_OUT ? {np, 3'b000} : {sp_word, 2'b00};
assign dm_wdata = wq_d;
assign iw_we    = sp_wr && sp_addr[12];
assign iw_addr  = ds == D_OUT ? {np, iq != 2'd0} : sp_word;
assign iw_data  = wq_d;

assign c_res  = is_cop0 ? cop0_rd(c0r) : v_res;
assign c_done = !is_cop0 ? v_done : 1'b1;

// one writer of the SP registers a cycle: the microcode (MTC0) first, the
// host when it is not there (h_ready)
assign h_ready = !mtc0;
wire         w_en   = (mtc0 && !mt_dma) || (h_we && h_ready);
wire  [3:0]  w_reg  = mtc0 ? c0r : h_addr;
wire  [31:0] w_data = mtc0 ? c_b : h_wdata;
assign pc_load = h_we && h_ready && h_addr == 4'd8 && st_halt;
assign dpc_start_we  = mtc0 && c0r == 4'd8;
assign dpc_end_we    = mtc0 && c0r == 4'd9;
assign dpc_status_we = mtc0 && c0r == 4'd11;
assign dpc_wdata = c_b;
assign h_rdata = h_addr == 4'd8 ? {20'd0, pc_next} : cop0_rd(h_addr);

always_ff @(posedge clk) begin
    if (rst) begin
        st_halt <= 1'b1; st_broke <= 1'b0; st_sstep <= 1'b0; st_intbrk <= 1'b0; st_sig <= 8'd0;
        p_full <= 1'b0; p_len <= '0; p_rd0 <= 1'b0;
        sem <= 1'b0; irq <= 1'b0; sp_addr <= '0; ram_addr <= '0; sp_addr_w <= '0; ram_addr_w <= '0; d_skip <= '0;
        ds <= D_IDLE; d_half <= 1'b0; q_half <= 1'b0; q_done <= 1'b0;
    end else begin
        // ---- register writes
        if (w_en) unique case (w_reg)
        // (while a DMA runs or is queued the write is the next one's: the running one's pointers go on, and are what a read gives)
        4'd0: begin if (!dma_bf) sp_addr <= w_data[12:3]; sp_addr_w <= w_data[12:3]; end
        4'd1: begin if (!dma_bf) ram_addr <= w_data[23:3]; ram_addr_w <= w_data[23:3]; end
        4'd4: begin
            // a clear / set pair: one of them alone acts, both together leave the bit
            if (w_data[1:0] == 2'b01) st_halt <= 1'b0;
            if (w_data[1:0] == 2'b10) st_halt <= 1'b1;
            if (w_data[2])  st_broke <= 1'b0;
            if (w_data[4:3] == 2'b01) irq <= 1'b0;
            if (w_data[4:3] == 2'b10) irq <= 1'b1;
            if (w_data[6:5] == 2'b01) st_sstep <= 1'b0;
            if (w_data[6:5] == 2'b10) st_sstep <= 1'b1;
            if (w_data[8:7] == 2'b01) st_intbrk <= 1'b0;
            if (w_data[8:7] == 2'b10) st_intbrk <= 1'b1;
            for (int i = 0; i < 8; i++) begin
                if (w_data[9 + 2 * i +: 2] == 2'b01) st_sig[i] <= 1'b0;
                if (w_data[9 + 2 * i +: 2] == 2'b10) st_sig[i] <= 1'b1;
            end
        end
        4'd7: sem <= 1'b0;
        default: ;
        endcase
        if ((mfc0 && c0r == 4'd7) || (h_rd && h_addr == 4'd7)) sem <= 1'b1;   // the semaphore: read (the microcode's or the host's), then taken
        if (su_brk) begin
            st_halt <= 1'b1; st_broke <= 1'b1;
            if (st_intbrk) irq <= 1'b1;
        end

        // ---- the DMA
        // the RDRAM pointer: a word on with each request taken, the skip at a row's end (not after the last)
        if (r_go) begin
            q_half <= !q_half;
            if (q_half) ram_addr <= ram_addr + 21'd1 + ((d_words == 10'd0 && d_rows != 8'd0) ? {12'd0, d_skip} : 21'd0);
            if (d_words == 10'd0) begin
                d_words <= d_row; d_rows <= d_rows - 8'd1;
                if (d_rows == 8'd0) q_done <= 1'b1;
            end else d_words <= d_words - 10'd1;
        end
        // the SP pointer: a word on with each word moved (it wraps inside its 4 KiB)
        if (sp_wr || o_sent) begin
            d_half <= !d_half;
            if (d_half) sp_addr[11:3] <= sp_addr[11:3] + 9'd1;
            if (g_words == 10'd0) begin g_words <= d_row; g_rows <= g_rows - 8'd1; end
            else g_words <= g_words - 10'd1;
        end
        // a LEN write: pending (a second one before the first starts replaces it); it leaves with its start
        if (d_start_p) p_full <= 1'b0;
        if (len_w) begin p_len <= w_data; p_rd0 <= w_reg == 4'd2; p_full <= 1'b1; end
        unique case (ds)
        D_IDLE: if (d_start_p) begin
            d_row   <= {d_len[11:3], 1'b1};
            d_words <= {d_len[11:3], 1'b1};
            d_rows  <= d_len[19:12];
            d_skip  <= d_len[31:23];
            g_words <= {d_len[11:3], 1'b1};
            g_rows  <= d_len[19:12];
            d_half  <= 1'b0; q_half <= 1'b0; q_done <= 1'b0;
            sp_addr <= sp_addr_w; ram_addr <= ram_addr_w;      // from the addresses last written
            np <= sp_addr_w[11:3]; ob_n <= 2'd0; iq <= 2'd0; pf_h <= 1'b0; pf_n <= 2'd0; pf_rd <= 1'b0;
            ds <= d_rd0 ? D_IN : D_OUT;
        end
        D_IN:    if (sp_wr && d_last) ds <= D_END;
        D_OUT: begin
            iq <= o_load ? 2'd0 : iq == 2'd2 ? 2'd2 : iq + 2'd1;
            // (DMEM: a read's pair into the FIFO the cycle after; the head to ob as it empties)
            pf_rd <= dm_rd;
            if (pf_rd) pf[pf_h ^ pf_n[0]] <= m_rdata16[127:64];
            pf_n <= pf_n + 2'(pf_rd) - 2'(o_load && !d_im);
            if (o_load && !d_im) pf_h <= !pf_h;
            if (d_im ? o_load : dm_rd) np <= np + 9'd1;
            if (iq == 2'd1) ih_q <= ia_q;
            if (o_load) begin ob <= d_im ? {ih_q, ia_q} : pf[pf_h]; ob_n <= 2'd2; end
            else if (r_go) ob_n <= ob_n - 2'd1;
            if (r_go && d_last) ds <= D_END;
        end
        D_END:   ds <= D_IDLE;
        default: ds <= D_IDLE;
        endcase
    end
end

endmodule
`default_nettype wire
