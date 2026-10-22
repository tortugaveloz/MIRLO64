// Mirlo's registers on the MIPS SoC (tools/mirlo_regs.py: the map, and the
// firmware's csr.h), in place of the RISC-V Mirlo's CSR peripherals
// (its APF blocks, mailbox, MRDP wrapper, timer, UART, video DMA) and with
// their behaviour:
//   APF_AUDIO     the Pocket's audio FIFO (core_top's): samples, playback, flush
//   APF_BRIDGE    the Pocket's target commands (core_top's core_bridge_cmd);
//                 STATUS is set by a command's completion, cleared by its read
//   APF_INPUT     the controllers
//   APF_INTERACT  interact.json: two words the Pocket writes (whole, or a
//                 field at a time at addresses 2-15); INTERACT_CHANGEDn is set
//                 by a whole-word write and cleared by the CPU reading INTERACTn
//   APF_VIDEO     vblank: its level, a flag set at its start (cleared by the
//                 read) and a frame counter
//   MAILBOX       game <-> geom message words and doorbells
//   MRDP          its command FIFO, fed by these registers or by the geom
//                 CFU's PUSH port (a register write wins a same-cycle clash),
//                 and its counters
//   TIMER0        a free-running 64-bit cycle counter, latched by a write
//   UART          the JTAG UART
//   GB_CART       (MIRLO_GB_CART) a Game Boy cartridge in the Pocket's slot:
//                 the port on and trusted, one byte read or written at a time
//   VIDEO_FRAMEBUFFER  the scan-out's base (latched by it at the frame
//                 boundary), its enables and its mode
// One access port: an access in the cycle of r_req, its read data the next.
// With MIRLO_RSP (the RSP in the geom core's place) what only that core's
// build uses is left out -- the mailbox, MRDP's load and drop counters and
// its unknown-opcode count, the scan-out's unused LENGTH -- and reads as 0.
`default_nettype none

module mirlo_regs (
    input  wire         clk,
    input  wire         rst,
    input  wire         r_req,
    input  wire         r_we,
    input  wire  [15:0] r_addr,
    input  wire  [31:0] r_wdata,
    output logic [31:0] r_rdata,
    // APF audio
    output logic [31:0] aud_out,
    output logic        aud_wr,
    output logic        aud_playback_en,
    output logic        aud_flush,
    input  wire  [11:0] aud_fill,
    // APF bridge
    output logic        br_request_read, br_request_write, br_request_getfile, br_request_openfile,
    output logic [15:0] br_slot_id,
    output logic [31:0] br_data_offset, br_length, br_ram_data_address,
    output logic        br_file_size_wr,
    output logic [31:0] br_new_file_size,
    input  wire  [31:0] br_file_size,
    input  wire         br_complete_trigger,
    input  wire  [31:0] br_current_address,
    input  wire  [2:0]  br_command_result_code,
    input  wire         br_host_reset_n, br_host_loaded,
    // APF input
    input  wire  [3:0][31:0] cont_key, cont_joy, cont_trig,
    // APF interact
    input  wire  [3:0]  ia_address,
    input  wire  [31:0] ia_data,
    input  wire         ia_wr,
    output logic [31:0] ia_q,
    output wire  [4:0]  ia_crop_v,          // interact: "Crop Vertical" (lines; field 3) and "Crop Right" (pixels; field 5)
    output wire  [1:0]  ia_crop_r,
    // video
    input  wire         vblank,             // sys: the scan-out is in vblank
    output logic [31:0] fb_base,
    output logic [11:0] fb_width,
    output logic [7:0]  fb_y0,          // the picture's window and steps in the VI's frame (rtl/soc/n64_video.sv)
    output logic [8:0]  fb_rows,
    output logic [9:0]  fb_px0, fb_pw,
    output logic [10:0] fb_xs, fb_nw,
    output logic [11:0] fb_ys,
    output logic        fb_tall,        // (WIDTH's bit 17) the frame buffer's pixels are 32-bit, VI_STATUS type 3: n64_video's
                                        // vi_px32 (the name is a former use's)
    output logic        fb_gamma,       // (WIDTH's bit 16) the scan-out's gamma correction
    output logic        fb_pal,         // (WIDTH's bit 18) the TV system before the game programs the VI's V_SYNC: 1 PAL (the ROM's region)
    input  wire         tv_pal,         // (WIDTH's bit 19, read) the TV system the scan-out is using
    output logic        fb_dma_enable, fb_vtg_enable, fb_mode,
    // CTRL
    input  wire  [15:0] bus_errors,
    // the mailbox
    output logic        geom_irq, game_irq,
    output logic        geom_run,           // MAILBOX.GEOM_RUN: the geom core out of reset
    output logic [31:0] dbg_geom_msg,
    // MRDP's command stream and counters
    input  wire         su_valid,           // the geom CFU's PUSH
    output logic        su_ready,
    input  wire  [31:0] su_data,
    output logic        cmd_valid,
    input  wire         cmd_ready,
    output logic [31:0] cmd_data,
    input  wire  [31:0] mrdp_sync_count,
    input  wire  [31:0] mrdp_load_count,
    input  wire  [15:0] mrdp_unknown_ops,
    input  wire         mrdp_idle,
    // the JTAG UART
    output logic        uart_we,
    output logic [7:0]  uart_byte,
    input  wire         uart_txfull,
    output logic        uart_rx_pop,
    input  wire  [7:0]  uart_rx_byte,
    input  wire         uart_rxempty,
    // GB_CART: the Pocket's cartridge slot (rtl/soc/gb_cart.sv; MIRLO_GB_CART)
    // MONITOR (MIRLO_MON): the game CPU's monitor level (mips_core's MON)
    input  wire         mon_sp, mon_field,  // the RSP's interrupt (a level); a field began (a pulse)
    input  wire         mon_dp,             // the RDP completed a SYNC FULL (a pulse)
    output logic        mon_own, mon_rcp,   // IP2 is mon_rcp, not the RSP's; the line
    output logic        mon_irq,            // the monitor's interrupt
    // MIRLO_MI / MIRLO_PI: the MI in hardware and the PI's DMA engine (rcp_mi.sv)
    output logic        mon_mi,             // IP2 is the MI's line
    output wire         mi_line,
    input  wire         mi_w_mi, mi_w_pi, mi_w_we,   // the game's access (mirlo_mips.sv: one cycle)
    input  wire  [1:0]  mi_w_a,
    input  wire  [31:0] mi_w_wd,
    output wire  [31:0] mi_w_rd,
    output wire         d_req, d_we,                 // the PI engine's access to the SDRAM
    output wire  [23:0] d_adr,
    output wire         d_ps,                        // the engine's read is the PSRAM's (a 64 MiB ROM's last 16 MiB: rcp_mi.sv)
    output wire  [3:0]  d_sel,
    output wire  [31:0] d_wdata,
    input  wire  [31:0] d_rdata,
    input  wire         d_ack,
    // MIRLO_PI2: the game CPU's word access to the cartridge's bus, done by the PI's engine (rcp_mi.sv); MONITOR.PI2's bit 0:
    // the cartridge's domain 2 is a FlashRAM (its CPU reads are the status word, its writes go nowhere)
    input  wire         io_req, io_we,
    input  wire  [23:0] io_adr,
    input  wire         io_ps,
    input  wire  [31:0] io_wd,
    output wire         io_ack,
    output logic        d2_flash,
    // the PSRAMs (psram.sv) are set up -- MONITOR.ID bit 8
    input  wire         ps_ready,
    // MIRLO_VI: the VI in hardware (rcp_vi.sv; MONITOR.CTL bit 3 takes the scan-out and the MI's VI from it)
    output logic        mon_vi,
    output logic [16:0] vi_thr,
    input  wire         vi_irq, vi_chg, vi_clr,
    // MIRLO_AI: the AI in hardware (rcp_ai.sv; MONITOR.CTL bit 4: PEND's bit 3 is a change of DACRATE, not the RDP's SYNC FULL)
    output logic        mon_ai,
    input  wire         ai_irq, ai_clr, ai_chg,
    input  wire  [13:0] ai_dac,
    output wire         ai_step_we,
    output wire  [19:0] ai_step_wd,
    // MIRLO_SI: the SI and the PIF RAM in hardware (rcp_si.sv): the DMA's end and a write to SI_STATUS (the MI's SI bit), the PIF
    // having commands to run (PEND bit 4), the runtime saying it is done (SIDONE)
    input  wire         si_irq, si_clr, si_ev,
    output wire         si_done,
    output logic        gbc_on, gbc_trust, gbc_start, gbc_we,
    output logic [15:0] gbc_addr,
    output logic [7:0]  gbc_wdata,
    input  wire         gbc_busy,
    input  wire  [7:0]  gbc_rdata
);
`include "mirlo_regs.svh"

wire wr = r_req && r_we;
wire rd = r_req && !r_we;
wire [15:0] a = {r_addr[15:2], 2'b00};

// ---- APF audio
always_ff @(posedge clk) begin
    aud_wr <= wr && a == R_APF_AUDIO_OUT;
    aud_out <= r_wdata;
    aud_flush <= wr && a == R_APF_AUDIO_BUFFER_FLUSH && r_wdata[0];      // a register: core_top's FIFO clear
    if (rst) aud_playback_en <= 0;
    else if (wr && a == R_APF_AUDIO_PLAYBACK_EN) aud_playback_en <= r_wdata[0];
end

// ---- APF bridge
logic br_status, br_ct_d, br_boot_ready;
always_comb begin
    br_request_read     = wr && a == R_APF_BRIDGE_REQUEST_READ;
    br_request_write    = wr && a == R_APF_BRIDGE_REQUEST_WRITE;
    br_request_getfile  = wr && a == R_APF_BRIDGE_REQUEST_GETFILE;
    br_request_openfile = wr && a == R_APF_BRIDGE_REQUEST_OPENFILE;
    br_file_size_wr     = wr && a == R_APF_BRIDGE_FILE_SIZE;
    br_new_file_size    = r_wdata;
end
always_ff @(posedge clk) begin
    br_ct_d <= br_complete_trigger;
    if (rst) begin
        br_slot_id <= 0; br_data_offset <= 0; br_length <= 0; br_ram_data_address <= 0; br_status <= 0; br_boot_ready <= 0;
    end else begin
        if (wr && a == R_APF_BRIDGE_SLOT_ID) br_slot_id <= r_wdata[15:0];
        if (wr && a == R_APF_BRIDGE_DATA_OFFSET) br_data_offset <= r_wdata;
        if (wr && a == R_APF_BRIDGE_TRANSFER_LENGTH) br_length <= r_wdata;
        if (wr && a == R_APF_BRIDGE_RAM_DATA_ADDRESS) br_ram_data_address <= r_wdata;
        if (wr && a == R_APF_BRIDGE_BOOT_READY) br_boot_ready <= r_wdata[0];
        if (rd && a == R_APF_BRIDGE_STATUS) br_status <= 0;
        if (br_complete_trigger && !br_ct_d) br_status <= 1;
    end
end

// ---- APF interact (slot 0: A, B, Z, Start, L, R at 5 bits each; slot 1:
// C-up/down/left/right at 5 bits, stick [21:20], D-pad [23:22], Show FPS
// [24], Start = Select+Start [25]) -- docs/control.md, Interact API
logic [31:0] ia0, ia1;
logic        ia_ch0, ia_ch1;
assign ia_crop_v = ia0[9:5];
assign ia_crop_r = ia0[16:15];
always_ff @(posedge clk) begin
    if (rst) begin ia0 <= 0; ia1 <= 0; ia_ch0 <= 0; ia_ch1 <= 0; end
    else begin
        if (wr && a == R_APF_INTERACT_INTERACT0) ia0 <= r_wdata;
        if (wr && a == R_APF_INTERACT_INTERACT1) ia1 <= r_wdata;
        if (rd && a == R_APF_INTERACT_INTERACT0) ia_ch0 <= 0;
        if (rd && a == R_APF_INTERACT_INTERACT1) ia_ch1 <= 0;
        if (ia_wr) case (ia_address)
            4'd0:  begin ia0 <= ia_data; ia_ch0 <= 1; end
            4'd1:  begin ia1 <= ia_data; ia_ch1 <= 1; end
            4'd2:  ia0[4:0]   <= ia_data[4:0];
            4'd3:  ia0[9:5]   <= ia_data[4:0];
            4'd4:  ia0[14:10] <= ia_data[4:0];
            4'd5:  ia0[19:15] <= ia_data[4:0];
            4'd6:  ia0[24:20] <= ia_data[4:0];
            4'd7:  ia0[29:25] <= ia_data[4:0];
            4'd8:  ia1[4:0]   <= ia_data[4:0];
            4'd9:  ia1[9:5]   <= ia_data[4:0];
            4'd10: ia1[14:10] <= ia_data[4:0];
            4'd11: ia1[19:15] <= ia_data[4:0];
            4'd12: ia1[21:20] <= ia_data[1:0];
            4'd13: ia1[23:22] <= ia_data[1:0];
            4'd14: ia1[24]    <= ia_data[0];
            default: ia1[25]  <= ia_data[0];
        endcase
    end
    case (ia_address)
    4'd0:  ia_q <= ia0;
    4'd1:  ia_q <= ia1;
    4'd2:  ia_q <= {27'd0, ia0[4:0]};
    4'd3:  ia_q <= {27'd0, ia0[9:5]};
    4'd4:  ia_q <= {27'd0, ia0[14:10]};
    4'd5:  ia_q <= {27'd0, ia0[19:15]};
    4'd6:  ia_q <= {27'd0, ia0[24:20]};
    4'd7:  ia_q <= {27'd0, ia0[29:25]};
    4'd8:  ia_q <= {27'd0, ia1[4:0]};
    4'd9:  ia_q <= {27'd0, ia1[9:5]};
    4'd10: ia_q <= {27'd0, ia1[14:10]};
    4'd11: ia_q <= {27'd0, ia1[19:15]};
    4'd12: ia_q <= {30'd0, ia1[21:20]};
    4'd13: ia_q <= {30'd0, ia1[23:22]};
    4'd14: ia_q <= {31'd0, ia1[24]};
    default: ia_q <= {31'd0, ia1[25]};
    endcase
end

// ---- APF video
logic        vb_d, vb_trig;
logic [29:0] vb_count;
always_ff @(posedge clk) begin
    vb_d <= vblank;
    if (rst) begin vb_trig <= 0; vb_count <= 0; end
    else begin
        if (rd && a == R_APF_VIDEO_VIDEO) vb_trig <= 0;
        if (vblank && !vb_d) begin vb_trig <= 1; vb_count <= vb_count + 30'd1; end
    end
end

// ---- the scan-out's registers
logic [31:0] fb_length;
assign fb_length = 32'd0;
always_ff @(posedge clk)
    if (rst) begin fb_base <= 32'h40C0_0000;
        fb_dma_enable <= 0; fb_vtg_enable <= 0; fb_mode <= 0; fb_width <= 0; fb_gamma <= 0; fb_tall <= 0; fb_pal <= 0;
        fb_y0 <= 0; fb_rows <= 0; fb_px0 <= 0; fb_pw <= 10'd640; fb_xs <= 11'h200; fb_ys <= 12'h400; fb_nw <= 11'd170; end
    else if (wr) case (a)
        R_VIDEO_FRAMEBUFFER_DMA_BASE:   fb_base <= r_wdata;
        R_VIDEO_FRAMEBUFFER_DMA_ENABLE: fb_dma_enable <= r_wdata[0];
        R_VIDEO_FRAMEBUFFER_VTG_ENABLE: fb_vtg_enable <= r_wdata[0];
        R_VIDEO_FRAMEBUFFER_VTG_MODE:   fb_mode <= r_wdata[0];
        R_VIDEO_FRAMEBUFFER_WIDTH:      begin fb_width <= r_wdata[11:0]; fb_gamma <= r_wdata[16]; fb_tall <= r_wdata[17]; fb_pal <= r_wdata[18]; end
        R_VIDEO_FRAMEBUFFER_WINDOW:     begin fb_y0 <= r_wdata[15:8]; fb_rows <= r_wdata[24:16]; end
        R_VIDEO_FRAMEBUFFER_HWINDOW:    begin fb_px0 <= r_wdata[9:0]; fb_pw <= r_wdata[25:16]; end
        R_VIDEO_FRAMEBUFFER_SCALE:      begin fb_xs <= r_wdata[10:0]; fb_ys <= r_wdata[27:16]; end
        R_VIDEO_FRAMEBUFFER_ROWWORDS:   fb_nw <= r_wdata[10:0];
        default: ;
    endcase

// ---- CTRL
logic [31:0] scratch;
always_ff @(posedge clk) if (rst) scratch <= 32'h1234_5678; else if (wr && a == R_CTRL_SCRATCH) scratch <= r_wdata;

// ---- the mailbox
wire [31:0] game_msg = 32'd0, geom_msg = 32'd0;
assign geom_irq = 1'b0; assign game_irq = 1'b0; assign geom_run = 1'b0;
assign dbg_geom_msg = 32'd0;

// ---- TIMER0
logic [63:0] uptime, uptime_l;
always_ff @(posedge clk) begin
    if (rst) uptime <= 0; else uptime <= uptime + 64'd1;
    if (wr && a == R_TIMER0_UPTIME_LATCH) uptime_l <= uptime;
end

// ---- MONITOR: what the monitor level of a game that runs its own libultra is woken by
logic [4:0] mon_pend, mon_mask;
logic [5:0]  mi_intr, mi_mask;
logic        pi_busy;
logic [20:0] pi_cnt;
localparam int MI_PRESENT = 1;
localparam int PI_PRESENT = 1;
localparam int VI_PRESENT = MI_PRESENT;
localparam int AI_PRESENT = MI_PRESENT;
localparam int SI_PRESENT = MI_PRESENT;
localparam int PI2_PRESENT = PI_PRESENT;
// (MONITOR.ID bit 7) a ROM's first 32 MiB in the PSRAM, at the bridge's address mod 32 MiB, the rest in the SDRAM: the engine
// (PISRC bit 25: the PSRAM's) and the game CPU's reads of the cartridge reach them (rcp_mi.sv, mirlo_mips.sv)
localparam int ROM64_PRESENT = PI2_PRESENT;
localparam int MON_PRESENT = 1;
logic sp_d, tick_d;
always_ff @(posedge clk) begin
    sp_d <= mon_sp; tick_d <= uptime[18];
    if (rst) begin d2_flash <= 1'b0; mon_pend <= 0; mon_mask <= 0; mon_own <= 0; mon_rcp <= 0; mon_mi <= 0; mon_vi <= 0; mon_ai <= 0; vi_thr <= '1; end
    else begin
        // (a set of the same bit by hardware and by a clear in one cycle: it stays set)
        if (wr && a == R_MONITOR_PEND) mon_pend <= mon_pend & ~r_wdata[4:0];
        if (wr && a == R_MONITOR_KICK) mon_pend <= mon_pend | r_wdata[4:0];
        if (mon_sp && !sp_d) mon_pend[0] <= 1'b1;
        if (mon_vi ? vi_chg : mon_field) mon_pend[1] <= 1'b1;         // (the VI in hardware: a field is the VI's change of a register)
        if (uptime[18] != tick_d) mon_pend[2] <= 1'b1;                  // a tick every 4.2 ms
        if (mon_ai ? ai_chg : mon_dp) mon_pend[3] <= 1'b1;             // (the AI in hardware: bit 3 is DACRATE's change)
        if (si_ev) mon_pend[4] <= 1'b1;                                 // (the SI in hardware: the PIF has commands to run)
        if (wr && a == R_MONITOR_MASK) mon_mask <= r_wdata[4:0];
        if (wr && a == R_MONITOR_CTL) begin mon_own <= r_wdata[0]; mon_rcp <= r_wdata[1]; mon_mi <= MI_PRESENT && r_wdata[2]; mon_vi <= VI_PRESENT && r_wdata[3]; mon_ai <= AI_PRESENT && r_wdata[4]; end
        if (wr && a == R_MONITOR_VITHR) vi_thr <= r_wdata[16:0];
        if (wr && a == R_MONITOR_PI2) d2_flash <= PI2_PRESENT && r_wdata[0];
    end
end
assign mon_irq = mon_own && ((mon_pend & mon_mask) != 0);

// the MI, and the PI's engine
rcp_mi #(.PI(PI_PRESENT), .PI2(PI2_PRESENT), .ROM64(ROM64_PRESENT)) mi (.clk, .rst, .sp_lvl(mon_sp), .vi_pulse(mon_vi ? vi_irq : mon_field), .vi_clr(vi_clr), .ai_pulse(ai_irq), .ai_clr(ai_clr), .si_pulse(si_irq), .si_clr(si_clr), .dp_pulse(mon_dp),
    .raise_we(wr && a == R_MONITOR_MIRAISE), .clr_we(wr && a == R_MONITOR_MICLR), .csr_bits(r_wdata[5:0]),
    .dst_we(wr && a == R_MONITOR_PIDST), .src_we(wr && a == R_MONITOR_PISRC), .len_we(wr && a == R_MONITOR_PILEN), .csr_wd(r_wdata[25:0]),
    .intr(mi_intr), .mask_o(mi_mask), .pi_busy, .pi_cnt,
    .w_mi(mi_w_mi), .w_pi(mi_w_pi), .w_we(mi_w_we), .w_a(mi_w_a), .w_wd(mi_w_wd), .w_rd(mi_w_rd), .line(mi_line),
    .d_req, .d_we, .d_adr, .d_ps, .d_sel, .d_wdata, .d_rdata, .d_ack, .io_req, .io_we, .io_adr, .io_ps, .io_wd, .io_ack);

assign ai_step_we = wr && a == R_MONITOR_AIRATE;     // (MIRLO_AI: the runtime's resampler step, rcp_ai.sv)
assign ai_step_wd = r_wdata[19:0];
assign si_done = wr && a == R_MONITOR_SIDONE;       // (MIRLO_SI: the runtime has run the PIF's commands, rcp_si.sv)

// ---- GB_CART
always_ff @(posedge clk) begin
    gbc_start <= wr && a == R_GB_CART_ACCESS;
    gbc_we <= r_wdata[24]; gbc_addr <= r_wdata[23:8]; gbc_wdata <= r_wdata[7:0];
    if (rst) begin gbc_on <= 0; gbc_trust <= 0; end
    else if (wr && a == R_GB_CART_CTRL) begin gbc_on <= r_wdata[0]; gbc_trust <= r_wdata[1]; end
end

// ---- UART
assign uart_we = wr && a == R_UART_RXTX;
assign uart_byte = r_wdata[7:0];
assign uart_rx_pop = rd && a == R_UART_RXTX && !uart_rxempty;

// ---- MRDP's command FIFO
wire         csr_push = wr && a == R_MRDP_CMD_DATA;
logic        f_in_ready;
logic [11:0] f_level;
logic [15:0] dropped;
`ifndef CMDQ_AW
`define CMDQ_AW 12
`endif
fifo_fwft #(.W(32), .AW(`CMDQ_AW)) cmdq (
    .clk, .rst, .in_valid(csr_push || su_valid), .in_ready(f_in_ready), .in_data(csr_push ? r_wdata : su_data),
    .out_valid(cmd_valid), .out_ready(cmd_ready), .out_data(cmd_data), .level(f_level)
);
assign su_ready = !csr_push && f_in_ready;
assign dropped = 16'd0;

// ---- reads
always_ff @(posedge clk) begin
    case (a)
    R_APF_AUDIO_PLAYBACK_EN:            r_rdata <= {31'd0, aud_playback_en};
    R_APF_AUDIO_BUFFER_FILL:            r_rdata <= {20'd0, aud_fill};
    R_APF_BRIDGE_SLOT_ID:               r_rdata <= {16'd0, br_slot_id};
    R_APF_BRIDGE_DATA_OFFSET:           r_rdata <= br_data_offset;
    R_APF_BRIDGE_TRANSFER_LENGTH:       r_rdata <= br_length;
    R_APF_BRIDGE_RAM_DATA_ADDRESS:      r_rdata <= br_ram_data_address;
    R_APF_BRIDGE_FILE_SIZE:             r_rdata <= br_file_size;
    R_APF_BRIDGE_STATUS:                r_rdata <= {31'd0, br_status};
    R_APF_BRIDGE_CURRENT_ADDRESS:       r_rdata <= br_current_address;
    R_APF_BRIDGE_COMMAND_RESULT_CODE:   r_rdata <= {29'd0, br_command_result_code};
    R_APF_BRIDGE_BOOT_READY:            r_rdata <= {31'd0, br_boot_ready};
    R_APF_BRIDGE_HOST_STATE:            r_rdata <= {30'd0, br_host_loaded, br_host_reset_n};
    R_APF_INPUT_CONT1_KEY:              r_rdata <= cont_key[0];
    R_APF_INPUT_CONT2_KEY:              r_rdata <= cont_key[1];
    R_APF_INPUT_CONT3_KEY:              r_rdata <= cont_key[2];
    R_APF_INPUT_CONT4_KEY:              r_rdata <= cont_key[3];
    R_APF_INPUT_CONT1_JOY:              r_rdata <= cont_joy[0];
    R_APF_INPUT_CONT2_JOY:              r_rdata <= cont_joy[1];
    R_APF_INPUT_CONT3_JOY:              r_rdata <= cont_joy[2];
    R_APF_INPUT_CONT4_JOY:              r_rdata <= cont_joy[3];
    R_APF_INPUT_CONT1_TRIG:             r_rdata <= cont_trig[0];
    R_APF_INPUT_CONT2_TRIG:             r_rdata <= cont_trig[1];
    R_APF_INPUT_CONT3_TRIG:             r_rdata <= cont_trig[2];
    R_APF_INPUT_CONT4_TRIG:             r_rdata <= cont_trig[3];
    R_APF_INTERACT_INTERACT0:           r_rdata <= ia0;
    R_APF_INTERACT_INTERACT_CHANGED0:   r_rdata <= {31'd0, ia_ch0};
    R_APF_INTERACT_INTERACT1:           r_rdata <= ia1;
    R_APF_INTERACT_INTERACT_CHANGED1:   r_rdata <= {31'd0, ia_ch1};
    R_APF_VIDEO_VIDEO:                  r_rdata <= {vb_count, vb_trig, vb_d};
    R_CTRL_SCRATCH:                     r_rdata <= scratch;
    R_CTRL_BUS_ERRORS:                  r_rdata <= {16'd0, bus_errors};
    R_MAILBOX_GAME_MSG:                 r_rdata <= game_msg;
    R_MAILBOX_GEOM_MSG:                 r_rdata <= geom_msg;
    R_MAILBOX_STATUS:                   r_rdata <= {30'd0, game_irq, geom_irq};
    R_MAILBOX_GEOM_RUN:                 r_rdata <= {31'd0, geom_run};
    R_MRDP_CMD_STATUS:                  r_rdata <= {19'd0, !f_in_ready, f_level};
    R_MRDP_CMD_DROPPED:                 r_rdata <= {16'd0, dropped};
    R_MRDP_SYNC_COUNT:                  r_rdata <= mrdp_sync_count;
    R_MRDP_STATUS:                      r_rdata <= {31'd0, mrdp_idle && f_level == 0 && !cmd_valid};
    R_TIMER0_UPTIME_CYCLES:             r_rdata <= uptime_l[63:32];
    R_TIMER0_UPTIME_CYCLES + 16'd4:     r_rdata <= uptime_l[31:0];
    R_UART_RXTX:                        r_rdata <= {24'd0, uart_rx_byte};
    R_UART_TXFULL:                      r_rdata <= {31'd0, uart_txfull};
    R_UART_RXEMPTY:                     r_rdata <= {31'd0, uart_rxempty};
    R_VIDEO_FRAMEBUFFER_DMA_BASE:       r_rdata <= fb_base;
    R_VIDEO_FRAMEBUFFER_DMA_LENGTH:     r_rdata <= fb_length;
    R_VIDEO_FRAMEBUFFER_DMA_ENABLE:     r_rdata <= {31'd0, fb_dma_enable};
    R_VIDEO_FRAMEBUFFER_WIDTH:          r_rdata <= {12'd0, tv_pal, fb_pal, fb_tall, fb_gamma, 4'd0, fb_width};
    R_VIDEO_FRAMEBUFFER_WINDOW:         r_rdata <= {7'd0, fb_rows, fb_y0, 8'd0};
    R_VIDEO_FRAMEBUFFER_HWINDOW:        r_rdata <= {6'd0, fb_pw, 6'd0, fb_px0};
    R_VIDEO_FRAMEBUFFER_SCALE:          r_rdata <= {4'd0, fb_ys, 5'd0, fb_xs};
    R_VIDEO_FRAMEBUFFER_ROWWORDS:       r_rdata <= {21'd0, fb_nw};
    R_VIDEO_FRAMEBUFFER_VTG_ENABLE:     r_rdata <= {31'd0, fb_vtg_enable};
    R_VIDEO_FRAMEBUFFER_VTG_MODE:       r_rdata <= {31'd0, fb_mode};
    R_MONITOR_ID:                       r_rdata <= 32'h4D4F_0000 | 32'(MON_PRESENT) | 32'(MI_PRESENT) << 1 | 32'(PI_PRESENT) << 2 | 32'(VI_PRESENT) << 3 | 32'(AI_PRESENT) << 4 | 32'(SI_PRESENT) << 5 | 32'(PI2_PRESENT) << 6 | 32'(ROM64_PRESENT) << 7 | 32'(ps_ready) << 8;
    R_MONITOR_CTL:                      r_rdata <= {27'd0, mon_ai, mon_vi, mon_mi, mon_rcp, mon_own};
    R_MONITOR_AIRATE:                   r_rdata <= {18'd0, ai_dac};
    R_MONITOR_PI2:                      r_rdata <= {31'd0, d2_flash};
    R_MONITOR_PEND:                     r_rdata <= {27'd0, mon_pend};
    R_MONITOR_MASK:                     r_rdata <= {27'd0, mon_mask};
    R_MONITOR_LEVEL:                    r_rdata <= {31'd0, mon_sp};
    R_MONITOR_MIINTR:                   r_rdata <= {15'd0, pi_busy, 2'd0, mi_mask, 2'd0, mi_intr};
    R_MONITOR_PILEN:                    r_rdata <= {pi_busy, 10'd0, pi_cnt};
    R_GB_CART_ID:                       r_rdata <= 32'h4742_0001;
    R_GB_CART_CTRL:                     r_rdata <= {30'd0, gbc_trust, gbc_on};
    // (BUSY from the write on: the engine's own busy is a cycle behind gbc_start)
    R_GB_CART_ACCESS:                   r_rdata <= {gbc_busy || gbc_start, 23'd0, gbc_rdata};
    default:                            r_rdata <= 32'd0;
    endcase
end

endmodule
`default_nettype wire
