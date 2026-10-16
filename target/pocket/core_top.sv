//
// User core top-level
//
// Instantiated by the real top-level: apf_top
//

`default_nettype none

module core_top (

    //
    // physical connections
    //

    ///////////////////////////////////////////////////
    // clock inputs 74.25mhz. not phase aligned, so treat these domains as asynchronous

    input wire clk_74a,  // mainclk1
    input wire clk_74b,  // mainclk1

    ///////////////////////////////////////////////////
    // cartridge interface
    // switches between 3.3v and 5v mechanically
    // output enable for multibit translators controlled by pic32

    // GBA AD[15:8]
    inout  wire [7:0] cart_tran_bank2,
    output wire       cart_tran_bank2_dir,

    // GBA AD[7:0]
    inout  wire [7:0] cart_tran_bank3,
    output wire       cart_tran_bank3_dir,

    // GBA A[23:16]
    inout  wire [7:0] cart_tran_bank1,
    output wire       cart_tran_bank1_dir,

    // GBA [7] PHI#
    // GBA [6] WR#
    // GBA [5] RD#
    // GBA [4] CS1#/CS#
    //     [3:0] unwired
    inout  wire [7:4] cart_tran_bank0,
    output wire       cart_tran_bank0_dir,

    // GBA CS2#/RES#
    inout  wire cart_tran_pin30,
    output wire cart_tran_pin30_dir,
    // when GBC cart is inserted, this signal when low or weak will pull GBC /RES low with a special circuit
    // the goal is that when unconfigured, the FPGA weak pullups won't interfere.
    // thus, if GBC cart is inserted, FPGA must drive this high in order to let the level translators
    // and general IO drive this pin.
    output wire cart_pin30_pwroff_reset,

    // GBA IRQ/DRQ
    inout  wire cart_tran_pin31,
    output wire cart_tran_pin31_dir,

    // infrared
    input  wire port_ir_rx,
    output wire port_ir_tx,
    output wire port_ir_rx_disable,

    // GBA link port
    inout  wire port_tran_si,
    output wire port_tran_si_dir,
    inout  wire port_tran_so,
    output wire port_tran_so_dir,
    inout  wire port_tran_sck,
    output wire port_tran_sck_dir,
    inout  wire port_tran_sd,
    output wire port_tran_sd_dir,

    ///////////////////////////////////////////////////
    // cellular psram 0 and 1, two chips (64mbit x2 dual die per chip)

    output wire [21:16] cram0_a,
    inout  wire [ 15:0] cram0_dq,
    input  wire         cram0_wait,
    output wire         cram0_clk,
    output wire         cram0_adv_n,
    output wire         cram0_cre,
    output wire         cram0_ce0_n,
    output wire         cram0_ce1_n,
    output wire         cram0_oe_n,
    output wire         cram0_we_n,
    output wire         cram0_ub_n,
    output wire         cram0_lb_n,

    output wire [21:16] cram1_a,
    inout  wire [ 15:0] cram1_dq,
    input  wire         cram1_wait,
    output wire         cram1_clk,
    output wire         cram1_adv_n,
    output wire         cram1_cre,
    output wire         cram1_ce0_n,
    output wire         cram1_ce1_n,
    output wire         cram1_oe_n,
    output wire         cram1_we_n,
    output wire         cram1_ub_n,
    output wire         cram1_lb_n,

    ///////////////////////////////////////////////////
    // sdram, 512mbit 16bit

    output wire [12:0] dram_a,
    output wire [ 1:0] dram_ba,
    inout  wire [15:0] dram_dq,
    output wire [ 1:0] dram_dqm,
    output wire        dram_clk,
    output wire        dram_cke,
    output wire        dram_ras_n,
    output wire        dram_cas_n,
    output wire        dram_we_n,

    ///////////////////////////////////////////////////
    // sram, 1mbit 16bit

    output wire [16:0] sram_a,
    inout  wire [15:0] sram_dq,
    output wire        sram_oe_n,
    output wire        sram_we_n,
    output wire        sram_ub_n,
    output wire        sram_lb_n,

    ///////////////////////////////////////////////////
    // vblank driven by dock for sync in a certain mode

    input wire vblank,

    ///////////////////////////////////////////////////
    // i/o to 6515D breakout usb uart

    output wire dbg_tx,
    input  wire dbg_rx,

    ///////////////////////////////////////////////////
    // i/o pads near jtag connector user can solder to

    output wire user1,
    input  wire user2,

    ///////////////////////////////////////////////////
    // RFU internal i2c bus

    inout  wire aux_sda,
    output wire aux_scl,

    ///////////////////////////////////////////////////
    // RFU, do not use
    output wire vpll_feed,


    //
    // logical connections
    //

    ///////////////////////////////////////////////////
    // video, audio output to scaler
    output wire [23:0] video_rgb,
    output wire        video_rgb_clock,
    output wire        video_rgb_clock_90,
    output wire        video_de,
    output wire        video_skip,
    output wire        video_vs,
    output wire        video_hs,

    output wire audio_mclk,
    input  wire audio_adc,
    output wire audio_dac,
    output wire audio_lrck,

    ///////////////////////////////////////////////////
    // bridge bus connection
    // synchronous to clk_74a
    output wire        bridge_endian_little,
    input  wire [31:0] bridge_addr,
    input  wire        bridge_rd,
    output reg  [31:0] bridge_rd_data,
    input  wire        bridge_wr,
    input  wire [31:0] bridge_wr_data,

    ///////////////////////////////////////////////////
    // controller data
    //
    // key bitmap:
    //   [0]    dpad_up
    //   [1]    dpad_down
    //   [2]    dpad_left
    //   [3]    dpad_right
    //   [4]    face_a
    //   [5]    face_b
    //   [6]    face_x
    //   [7]    face_y
    //   [8]    trig_l1
    //   [9]    trig_r1
    //   [10]   trig_l2
    //   [11]   trig_r2
    //   [12]   trig_l3
    //   [13]   trig_r3
    //   [14]   face_select
    //   [15]   face_start
    //   [28:16] <unused>
    //   [31:29] type
    // joy values - unsigned
    //   [ 7: 0] lstick_x
    //   [15: 8] lstick_y
    //   [23:16] rstick_x
    //   [31:24] rstick_y
    // trigger values - unsigned
    //   [ 7: 0] ltrig
    //   [15: 8] rtrig
    //
    input wire [31:0] cont1_key,
    input wire [31:0] cont2_key,
    input wire [31:0] cont3_key,
    input wire [31:0] cont4_key,
    input wire [31:0] cont1_joy,
    input wire [31:0] cont2_joy,
    input wire [31:0] cont3_joy,
    input wire [31:0] cont4_joy,
    input wire [15:0] cont1_trig,
    input wire [15:0] cont2_trig,
    input wire [15:0] cont3_trig,
    input wire [15:0] cont4_trig,

    input  wire altera_reserved_tck,
    input  wire altera_reserved_tdi,
    output wire altera_reserved_tdo,
    input  wire altera_reserved_tms

);

  // not using the IR port, so turn off both the LED, and
  // disable the receive circuit to save power
  assign port_ir_tx              = 0;
  assign port_ir_rx_disable      = 1;

  // bridge endianness
  assign bridge_endian_little    = 0;

  // The cartridge port is the SoC's (rtl/soc/gb_cart.sv, with the macro
  // MIRLO_GB_CART: a Game Boy cartridge). Directions are 0:IN, 1:OUT. Game Boy
  // pins: bank3 A7-A0, bank2 A15-A8, bank1 D7-D0, bank0 PHI, /WR, /RD, /CS;
  // pin 30 is /RES, which the board holds low until cart_pin30_pwroff_reset is
  // driven high -- the pin itself is never driven, nor is pin 31 (VIN).
  // Without the macro, and whenever the SoC has the port off, this is the
  // template's idle state: banks 1-3 inputs, bank0 driven high, the cartridge
  // held in reset.
  wire        gb_on, gb_a_dir, gb_a_oe, gb_d_dir, gb_d_oe, gb_phi, gb_wr_n, gb_rd_n, gb_cs_n;
  wire [15:0] gb_a;
  wire [7:0]  gb_d_o;
  assign cart_tran_bank3         = gb_a_oe ? gb_a[7:0] : 8'hzz;
  assign cart_tran_bank3_dir     = gb_a_dir;
  assign cart_tran_bank2         = gb_a_oe ? gb_a[15:8] : 8'hzz;
  assign cart_tran_bank2_dir     = gb_a_dir;
  assign cart_tran_bank1         = gb_d_oe ? gb_d_o : 8'hzz;
  assign cart_tran_bank1_dir     = gb_d_dir;
  assign cart_tran_bank0         = {gb_phi, gb_wr_n, gb_rd_n, gb_cs_n};
  assign cart_tran_bank0_dir     = 1'b1;
  assign cart_tran_pin30         = 1'b0;
  assign cart_tran_pin30_dir     = gb_on ? 1'b0 : 1'bz;   // (off: as the template leaves it)
  assign cart_pin30_pwroff_reset = gb_on;
  assign cart_tran_pin31         = 1'bz;
  assign cart_tran_pin31_dir     = 1'b0;

  // link port is input only
  assign port_tran_so            = 1'bz;
  assign port_tran_so_dir        = 1'b0;  // SO is output only
  assign port_tran_si            = 1'bz;
  assign port_tran_si_dir        = 1'b0;  // SI is input only
  assign port_tran_sck           = 1'bz;
  assign port_tran_sck_dir       = 1'b0;  // clock direction can change
  assign port_tran_sd            = 1'bz;
  assign port_tran_sd_dir        = 1'b0;  // SD is input and not used

  // tie off the rest of the pins we are not using
  assign cram0_a                 = 'h0;
  assign cram0_dq                = {16{1'bZ}};
  assign cram0_clk               = 0;
  assign cram0_adv_n             = 1;
  assign cram0_cre               = 0;
  assign cram0_ce0_n             = 1;
  assign cram0_ce1_n             = 1;
  assign cram0_oe_n              = 1;
  assign cram0_we_n              = 1;
  assign cram0_ub_n              = 1;
  assign cram0_lb_n              = 1;

  assign cram1_a                 = 'h0;
  assign cram1_dq                = {16{1'bZ}};
  assign cram1_clk               = 0;
  assign cram1_adv_n             = 1;
  assign cram1_cre               = 0;
  assign cram1_ce0_n             = 1;
  assign cram1_ce1_n             = 1;
  assign cram1_oe_n              = 1;
  assign cram1_we_n              = 1;
  assign cram1_ub_n              = 1;
  assign cram1_lb_n              = 1;

  // assign dram_a                  = 'h0;
  // assign dram_ba                 = 'h0;
  // assign dram_dq                 = {16{1'bZ}};
  // assign dram_dqm                = 'h0;
  // assign dram_clk                = 'h0;
  // assign dram_cke                = 'h0;
  // assign dram_ras_n              = 'h1;
  // assign dram_cas_n              = 'h1;
  // assign dram_we_n               = 'h1;

  assign sram_a                  = 'h0;
  assign sram_dq                 = {16{1'bZ}};
  assign sram_oe_n               = 1;
  assign sram_we_n               = 1;
  assign sram_ub_n               = 1;
  assign sram_lb_n               = 1;

  assign dbg_tx                  = 1'bZ;
  assign user1                   = 1'bZ;
  assign aux_scl                 = 1'bZ;
  assign vpll_feed               = 1'bZ;

  wire [31:0] apf_wishbone_bridge_rd_data;

  // for bridge write data, we just broadcast it to all bus devices
  // for bridge read data, we have to mux it
  // add your own devices here
  always @(*) begin
    casex (bridge_addr)
      default: begin
        bridge_rd_data <= 0;
      end
      32'h0XXX_XXXX: begin
        bridge_rd_data <= apf_wishbone_bridge_rd_data;
      end
      32'h1000_00XX: begin
        bridge_rd_data <= 0;
      end
      32'h1000_01XX: begin
        bridge_rd_data <= interact_read_data;
      end
      32'hF8XX_XXXX: begin
        bridge_rd_data <= cmd_bridge_rd_data;
      end
    endcase
  end

  always @(posedge clk_74a) begin
    if (bridge_addr[31:28] == 4'h1 && bridge_wr) begin
      casex (bridge_addr[27:0])
        28'h8: begin
          use_jtag <= bridge_wr_data[0];
        end
        28'h1XX: begin
          // Directly in FIFO
        end
      endcase
    end
  end

  //
  // host/target command handler
  //
  wire reset_n;  // driven by host commands, can be used as core-wide reset
  wire [31:0] cmd_bridge_rd_data;

  // bridge host commands
  // synchronous to clk_74a
  // Slot 0 is loaded by the Pocket (its own loading bar) through
  // apf_wishbone_master straight into SDRAM, which only works once the BIOS
  // has initialised it (apf_bridge.boot_ready). Sticky: a core reset re-runs
  // the BIOS, but the Pocket is not told.
  wire boot_ready;
  reg boot_ready_s1 = 0, boot_ready_s2 = 0, sdram_ready_74 = 0;
  always @(posedge clk_74a) begin
    boot_ready_s1 <= boot_ready;
    boot_ready_s2 <= boot_ready_s1;
    if (boot_ready_s2) sdram_ready_74 <= 1;
  end
  // Reporting "booting" until then, or answering the Pocket's data slot
  // write request "try again later" (result 2), both failed the launch ("RS:
  // Host commands ignored" -- it requests one at launch even when every slot
  // is deferload: the nonvolatile save). The status is the template's, the
  // request is always OK, and it is only acknowledged once the SDRAM is ready:
  // until then core_bridge_cmd holds it busy ("BU"), which the Pocket waits on.
  wire status_boot_done = pll_core_locked_s74;
  wire status_setup_done = pll_core_locked_s74;  // rising edge triggers a target command
  wire status_running = reset_n;  // we are running as soon as reset_n goes high

  wire dataslot_requestread;
  wire [15:0] dataslot_requestread_id;
  wire dataslot_requestread_ack = 1;
  wire dataslot_requestread_ok = 1;

  wire dataslot_requestwrite;
  wire [15:0] dataslot_requestwrite_id;
  wire [31:0] dataslot_requestwrite_size;
  wire dataslot_requestwrite_ack = sdram_ready_74;
  wire dataslot_requestwrite_ok = 1;

  wire dataslot_update;
  wire [15:0] dataslot_update_id;
  wire [31:0] dataslot_update_size;

  wire dataslot_allcomplete;

  wire [31:0] rtc_epoch_seconds;
  wire [31:0] rtc_date_bcd;
  wire [31:0] rtc_time_bcd;
  wire rtc_valid;

  wire savestate_supported;
  wire [31:0] savestate_addr;
  wire [31:0] savestate_size;
  wire [31:0] savestate_maxloadsize;

  wire savestate_start;
  wire savestate_start_ack;
  wire savestate_start_busy;
  wire savestate_start_ok;
  wire savestate_start_err;

  wire savestate_load;
  wire savestate_load_ack;
  wire savestate_load_busy;
  wire savestate_load_ok;
  wire savestate_load_err;

  wire osnotify_inmenu;

  // bridge target commands
  // synchronous to clk_74a

  wire target_dataslot_request;
  wire [1:0] target_dataslot_request_type;

  wire target_dataslot_read = target_dataslot_request && target_dataslot_request_type == 2'h0;
  wire target_dataslot_write = target_dataslot_request && target_dataslot_request_type == 2'h1;
  wire target_dataslot_getfile = target_dataslot_request && target_dataslot_request_type == 2'h2;
  wire target_dataslot_openfile = target_dataslot_request && target_dataslot_request_type == 2'h3;

  wire target_dataslot_ack;
  wire target_dataslot_done;
  wire [2:0] target_dataslot_err;

  wire [15:0] target_dataslot_id;
  wire [31:0] target_dataslot_slotoffset;
  wire [31:0] target_dataslot_bridgeaddr;
  wire [31:0] target_dataslot_length;

  wire [31:0] target_buffer_param_struct = target_dataslot_bridgeaddr;
  wire [31:0] target_buffer_resp_struct = target_dataslot_bridgeaddr;

  // bridge data slot access
  // synchronous to clk_74a

  reg [9:0] datatable_addr;
  reg datatable_wren;
  reg [31:0] datatable_data;
  wire [31:0] datatable_q;

  core_bridge_cmd icb (

      .clk    (clk_74a),
      .reset_n(reset_n),

      .bridge_endian_little(bridge_endian_little),
      .bridge_addr         (bridge_addr),
      .bridge_rd           (bridge_rd),
      .bridge_rd_data      (cmd_bridge_rd_data),
      .bridge_wr           (bridge_wr),
      .bridge_wr_data      (bridge_wr_data),

      .status_boot_done (status_boot_done),
      .status_setup_done(status_setup_done),
      .status_running   (status_running),

      .dataslot_requestread    (dataslot_requestread),
      .dataslot_requestread_id (dataslot_requestread_id),
      .dataslot_requestread_ack(dataslot_requestread_ack),
      .dataslot_requestread_ok (dataslot_requestread_ok),

      .dataslot_requestwrite     (dataslot_requestwrite),
      .dataslot_requestwrite_id  (dataslot_requestwrite_id),
      .dataslot_requestwrite_size(dataslot_requestwrite_size),
      .dataslot_requestwrite_ack (dataslot_requestwrite_ack),
      .dataslot_requestwrite_ok  (dataslot_requestwrite_ok),

      .dataslot_update     (dataslot_update),
      .dataslot_update_id  (dataslot_update_id),
      .dataslot_update_size(dataslot_update_size),

      .dataslot_allcomplete(dataslot_allcomplete),

      .rtc_epoch_seconds(rtc_epoch_seconds),
      .rtc_date_bcd     (rtc_date_bcd),
      .rtc_time_bcd     (rtc_time_bcd),
      .rtc_valid        (rtc_valid),

      .savestate_supported  (savestate_supported),
      .savestate_addr       (savestate_addr),
      .savestate_size       (savestate_size),
      .savestate_maxloadsize(savestate_maxloadsize),

      .savestate_start     (savestate_start),
      .savestate_start_ack (savestate_start_ack),
      .savestate_start_busy(savestate_start_busy),
      .savestate_start_ok  (savestate_start_ok),
      .savestate_start_err (savestate_start_err),

      .savestate_load     (savestate_load),
      .savestate_load_ack (savestate_load_ack),
      .savestate_load_busy(savestate_load_busy),
      .savestate_load_ok  (savestate_load_ok),
      .savestate_load_err (savestate_load_err),

      .osnotify_inmenu(osnotify_inmenu),

      .target_dataslot_read    (target_dataslot_read),
      .target_dataslot_write   (target_dataslot_write),
      .target_dataslot_getfile (target_dataslot_getfile),
      .target_dataslot_openfile(target_dataslot_openfile),

      .target_dataslot_ack (target_dataslot_ack),
      .target_dataslot_done(target_dataslot_done),
      .target_dataslot_err (target_dataslot_err),

      .target_dataslot_id        (target_dataslot_id),
      .target_dataslot_slotoffset(target_dataslot_slotoffset),
      .target_dataslot_bridgeaddr(target_dataslot_bridgeaddr),
      .target_dataslot_length    (target_dataslot_length),

      .target_buffer_param_struct(target_buffer_param_struct),
      .target_buffer_resp_struct (target_buffer_resp_struct),

      .datatable_addr(datatable_addr),
      .datatable_wren(datatable_wren),
      .datatable_data(datatable_data),
      .datatable_q   (datatable_q)

  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Interact CSR

  wire interact_region = bridge_addr >= 32'h1000_0100 && bridge_addr < 32'h1000_0200;

  wire interact_rd;
  wire interact_wr;
  wire [5:0] interact_addr = interact_wr ? interact_wr_addr : interact_rd_addr;
  wire [31:0] interact_data;

  wire [5:0] interact_rd_addr;
  wire [5:0] interact_wr_addr;

  wire [31:0] interact_q;

  wire [31:0] interact_read_data;

  sync_fifo #(
      .WIDTH(38)
  ) interact_write_sync_fifo (
      .clk_write(clk_74a),
      .clk_read (clk_sys),

      .write_en(interact_region && bridge_wr),
      .data({bridge_addr[7:2], bridge_wr_data}),

      .data_s({interact_wr_addr, interact_data}),
      .write_en_s(interact_wr)
  );

  sync_fifo #(
      .WIDTH(6)
  ) interact_read_addr_sync_fifo (
      .clk_write(clk_74a),
      .clk_read (clk_sys),

      .write_en(interact_region && bridge_rd),
      .data(bridge_addr[7:2]),

      .data_s(interact_rd_addr),
      .write_en_s(interact_rd)
  );

  reg interact_q_rd = 0;

  sync_fifo #(
      .WIDTH(32)
  ) interact_read_data_sync_fifo (
      .clk_write(clk_sys),
      .clk_read (clk_74a),

      .write_en(interact_q_rd),
      .data(interact_q),

      .data_s(interact_read_data)
      // .write_en_s()
  );

  reg [5:0] prev_interact_addr = 0;

  always @(posedge clk_sys) begin
    prev_interact_addr <= interact_addr;

    interact_q_rd <= 0;

    if (interact_addr != prev_interact_addr) begin
      // A cycle after we receive a new address, send the read data
      interact_q_rd <= 1;
    end
  end

  ////////////////////////////////////////////////////////////////////////////////////////
  // Dataslot Management

  reg [31:0] active_file_size = 0;
  reg prev_dataslot_update = 0;

  always @(posedge clk_74a) begin
    if (~pll_core_locked) begin
      datatable_addr <= 0;
      datatable_data <= 0;
      datatable_wren <= 0;
    end else begin
      prev_dataslot_update = dataslot_update;

      datatable_wren   <= 0;

      // Read asset size for data slot index set by ID
      // These are indicies, not IDs, so you must make sure the index matches the ID
      datatable_addr   <= target_dataslot_id[9:0] * 10'h2 + 10'h1;
      active_file_size <= datatable_q;

      if (apf_bridge_file_size_wr_s) begin
        // Set ID index
        datatable_addr <= target_dataslot_id[9:0] * 10'h2 + 10'h1;

        datatable_data <= apf_bridge_new_file_size_data_s;

        datatable_wren <= 1;
      end else if (dataslot_update && ~prev_dataslot_update) begin
        // Set ID index
        datatable_addr <= dataslot_update_id * 10'h2 + 10'h1;

        datatable_data <= dataslot_update_size;

        datatable_wren <= 1;
      end
    end
  end

  wire apf_bridge_file_size_wr;
  wire [31:0] apf_bridge_new_file_size_data;

  wire apf_bridge_file_size_wr_s;
  wire [31:0] apf_bridge_new_file_size_data_s;

  sync_fifo #(
      .WIDTH(32)
  ) dataslot_update_sync_fifo (
      .clk_write(clk_sys),
      .clk_read (clk_74a),

      .write_en(apf_bridge_file_size_wr),
      .data(apf_bridge_new_file_size_data),

      .data_s(apf_bridge_new_file_size_data_s),
      .write_en_s(apf_bridge_file_size_wr_s)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Data loading

  reg ioctl_download = 0;
  reg slot0_written = 0;
  reg host_loaded = 0;  // the Pocket has written slot 0, the game (sticky, for the BIOS)

  always @(posedge clk_74a) begin
    if (dataslot_requestwrite && dataslot_requestwrite_ack) begin
      ioctl_download <= 1;
      if (dataslot_requestwrite_id == 16'h0) slot0_written <= 1;
    end else if (dataslot_allcomplete) begin
      ioctl_download <= 0;
      if (slot0_written) host_loaded <= 1;
    end
  end


  ////////////////////////////////////////////////////////////////////////////////////////
  // Debug UART

  wire reload_rom_active;
  wire uart_rx;
  wire uart_tx;

  // (no Debug Key: it took the cartridge port's strobes -- /RD and /WR of a
  // cartridge left in the slot -- for its LED and its UART, which nothing uses)
  assign uart_rx = 1'b1;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Chip ID

  wire [63:0] chip_id;

  chip_id chip_id_controller (
      .clkin  (clk_sys),
      .reset  (~reset_n_s),
      .chip_id(chip_id)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Settings and Sync

  // No core reset at all: the Pocket loads the game into
  // SDRAM once per launch and the program there cannot restart itself (its
  // .data has run), so a reset would have to read the whole file again.
  // Quit + relaunch instead. (Was: an L+R+Select+Start chord, a "Reset core"
  // action at 0x10000000 and a "+ resets" setting at 0x10000004.)
  // Resets to 1 (JTAG UART), not 0: the APF writes the saved "Enable JTAG
  // UART" setting (interact.json id 101) when it launches the core, so the
  // normal flow is unaffected, but a bitstream loaded straight over the
  // USB-Blaster (quartus_pgm) never gets that write -- with 0 here its
  // console went to the cartridge pins and the JTAG firmware upload that
  // follows had nothing to talk to without a trip through the in-core menu.
  reg use_jtag = 1;

  wire reset_n_s;
  wire use_jtag_s;

  synch_3 #(
      .WIDTH(2)
  ) settings_synch (
      {reset_n, use_jtag},
      {reset_n_s, use_jtag_s},
      clk_sys
  );

  wire rtc_valid_s;
  wire [31:0] rtc_epoch_seconds_s;
  wire [31:0] rtc_date_bcd_s;
  wire [31:0] rtc_time_bcd_s;

  // These values won't change, so normal multibit synchs are fine
  synch_3 #(
      .WIDTH(97)
  ) rtc_synch (
      {rtc_valid, rtc_epoch_seconds, rtc_date_bcd, rtc_time_bcd},
      {rtc_valid_s, rtc_epoch_seconds_s, rtc_date_bcd_s, rtc_time_bcd_s},
      clk_sys
  );

  reg [31:0] rtc_counter = 0;
  localparam SYS_HZ = 62_500_000;   // clk_sys with the N64's figures (mf_pllbase_0002.v)
  reg [31:0] rtc_second_counter = 0;

  reg prev_rtc_valid = 0;

  always @(posedge clk_sys) begin
    prev_rtc_valid <= rtc_valid_s;

    if (rtc_second_counter > 0) begin
      rtc_second_counter <= rtc_second_counter - 32'h1;
    end else begin
      rtc_second_counter <= SYS_HZ - 1;
      rtc_counter <= rtc_counter + 32'h1;
    end

    if (rtc_valid_s && ~prev_rtc_valid) begin
      rtc_counter <= rtc_epoch_seconds_s;
      rtc_second_counter <= SYS_HZ - 1;
    end
  end

  // Only at the bitstream load: the SoC is not held by the Pocket's reset_n
  // (the BIOS has to run while the Pocket loads slot 0 into SDRAM), and
  // there is no core reset.
  reg [15:0] reset_timer = 16'hFFFF;

  always @(posedge clk_sys) begin
    if (reset_timer > 0) reset_timer <= reset_timer - 16'h1;
  end

  wire apf_bridge_request_read;
  wire apf_bridge_request_write;
  wire apf_bridge_request_getfile;
  wire apf_bridge_request_openfile;

  wire [31:0] apf_bridge_data_offset;
  wire [31:0] apf_bridge_length;
  wire [15:0] apf_bridge_slot_id;

  reg [2:0] apf_bridge_request_counter = 0;
  reg [1:0] apf_bridge_request_type = 0;

  reg prev_apf_bridge_request_read = 0;
  reg prev_apf_bridge_request_write = 0;
  reg prev_apf_bridge_request_getfile = 0;
  reg prev_apf_bridge_request_openfile = 0;

  // Signal lengthener for faster bridge clock domain
  always @(posedge clk_sys) begin
    prev_apf_bridge_request_read <= apf_bridge_request_read;
    prev_apf_bridge_request_write <= apf_bridge_request_write;
    prev_apf_bridge_request_getfile <= apf_bridge_request_getfile;
    prev_apf_bridge_request_openfile <= apf_bridge_request_openfile;

    if (apf_bridge_request_read && ~prev_apf_bridge_request_read) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h0;
    end else if (apf_bridge_request_write && ~prev_apf_bridge_request_write) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h1;
    end else if (apf_bridge_request_getfile && ~prev_apf_bridge_request_getfile) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h2;
    end else if (apf_bridge_request_openfile && ~prev_apf_bridge_request_openfile) begin
      apf_bridge_request_counter <= 3'h7;
      apf_bridge_request_type <= 2'h3;
    end

    if (apf_bridge_request_counter > 3'h0) begin
      apf_bridge_request_counter <= apf_bridge_request_counter - 3'h1;
    end
  end

  wire apf_bridge_request = apf_bridge_request_counter > 3'h0;

  wire [31:0] active_file_size_s;
  wire target_dataslot_done_s;

  // (area, docs/mips.md: the file size is not read until the command's DONE has
  // crossed -- three clocks behind it -- so it is long stable, and one register
  // is the whole crossing. It was three stages of 32 bits.)
  synch_3 #(
      .WIDTH(1)
  ) from_bridge_s (
      target_dataslot_done,
      target_dataslot_done_s,
      clk_sys
  );
  reg [31:0] active_file_size_r;
  always @(posedge clk_sys) active_file_size_r <= active_file_size;
  assign active_file_size_s = active_file_size_r;

  // The RAM address the core programmed, as a bridge address (apf_wishbone_master
  // maps bridge 0 to SDRAM 0x4000_0000). A CSR the CPU writes before the
  // request, so it is stable when core_bridge_cmd samples it (clk_74a and
  // clk_sys are asynchronous groups in core_constraints.sdc). From 49 MiB up
  // with bit 26 set: there the bridge's plain addresses are a game file's end,
  // which is not the SDRAM's (rtl/soc/mirlo_mips.sv, the bridge's map).
  wire ram_data_high = ram_data_address[25:24] == 2'b11 && ram_data_address[23:20] != 4'd0;
  assign target_dataslot_bridgeaddr = {5'h0, ram_data_high, ram_data_address[25:0]};

  // Only the request and its type are synchronised. The slot, offset and length are
  // CSRs the CPU writes before the request (as the RAM address above, which was never
  // synchronised) and the request reaches clk_74a three of its clocks after they are
  // stable. (Area: 80 bits of three stages.)
  synch_3 #(
      .WIDTH(3)
  ) to_bridge_s (
      {
        apf_bridge_request,
        apf_bridge_request_type
      },
      {
        target_dataslot_request,
        target_dataslot_request_type
      },
      clk_74a
  );
  assign target_dataslot_slotoffset = apf_bridge_data_offset;
  assign target_dataslot_length = apf_bridge_length;
  assign target_dataslot_id = apf_bridge_slot_id;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Core

  // Registered: this fans out to every sys and sys2x register in the SoC, and
  // the unregistered compare+OR (reset_timer > 0) is what limits it at higher
  // clocks (branch clk68). ioctl_download is a clk_74a signal and gets a
  // synchronizer on the way -- it used to enter the sys reset unsynchronised.
  // Neither the Pocket's reset_n nor a slot download resets the SoC any more:
  // the BIOS initialises the SDRAM, reports ready, and waits for the Pocket's
  // Reset Exit (apf_bridge.host_state) while it loads slot 0.
  reg ioctl_download_s1 = 0, ioctl_download_s2 = 0;
  reg host_loaded_s1 = 0, host_loaded_s2 = 0;
  reg reset_r = 1;
  always @(posedge clk_sys) begin
    ioctl_download_s1 <= ioctl_download;
    ioctl_download_s2 <= ioctl_download_s1;
    host_loaded_s1 <= host_loaded;
    host_loaded_s2 <= host_loaded_s1;
    reset_r <= reset_timer != 16'h0;
  end
  wire reset = reset_r;

  // wire ack;
  // wire [29:0] addr;
  // wire [1:0] bte;
  // wire [2:0] cti;
  // wire cyc;
  // wire [31:0] data_read;
  // wire [31:0] data_write;
  // wire err;
  // wire [3:0] sel;
  // wire stb;
  // wire we;

  wire apf_master_ack;
  wire [29:0] apf_master_addr;
  wire [1:0] apf_master_bte;
  wire [2:0] apf_master_cti;
  wire apf_master_cyc;
  wire [31:0] apf_master_data_read;
  wire [31:0] apf_master_data_write;
  wire apf_master_err;
  wire [3:0] apf_master_sel;
  wire apf_master_stb;
  wire apf_master_we;

  wire [31:0] ram_data_address;
  reg [31:0] latched_ram_data_address = 0;
  wire [25:0] current_address;

  wire [31:0] audio_bus_out;
  wire audio_bus_wr;
  wire audio_playback_en;
  wire audio_flush;
  wire [11:0] audio_buffer_fill;

  reg prev_target_dataslot_done_s = 0;

  always @(posedge clk_sys) begin
    prev_target_dataslot_done_s <= target_dataslot_done_s;

    if (apf_bridge_request_read || apf_bridge_request_write || apf_bridge_request_getfile || apf_bridge_request_openfile) begin
      latched_ram_data_address <= ram_data_address;
    end
  end

  // the SoC (rtl/soc/mirlo_mips.sv)
  mirlo_mips soc (
      .clk_sys(clk_sys),
      .clk_cpu(clk_cpu),
      .clk_sys2x(clk_mem),
      .clk_sys2x_90deg(clk_mem_180deg),
      .clk_vid(clk_vid),

      .reset(reset),

      .apf_audio_bus_out(audio_bus_out),
      .apf_audio_bus_wr(audio_bus_wr),
      .apf_audio_playback_en(audio_playback_en),
      .apf_audio_flush(audio_flush),
      .apf_audio_buffer_fill(audio_buffer_fill),

      .apf_bridge_request_read(apf_bridge_request_read),
      .apf_bridge_request_write(apf_bridge_request_write),
      .apf_bridge_request_getfile(apf_bridge_request_getfile),
      .apf_bridge_request_openfile(apf_bridge_request_openfile),

      .apf_bridge_data_offset(apf_bridge_data_offset),
      .apf_bridge_length(apf_bridge_length),
      .apf_bridge_ram_data_address(ram_data_address),
      .apf_bridge_slot_id(apf_bridge_slot_id),
      .apf_bridge_file_size(active_file_size_s),
      .apf_bridge_file_size_wr(apf_bridge_file_size_wr),
      .apf_bridge_new_file_size_data(apf_bridge_new_file_size_data),
      .apf_bridge_current_address(current_address),
      // Pulse complete on rising edge of done
      .apf_bridge_complete_trigger(target_dataslot_done_s && ~prev_target_dataslot_done_s),
      .apf_bridge_command_result_code(target_dataslot_err),
      .apf_bridge_boot_ready(boot_ready),
      .apf_bridge_host_reset_n(reset_n_s),
      .apf_bridge_host_loaded(host_loaded_s2),

      .apf_id_chip_id(chip_id),

      .apf_input_cont1_key (cont1_key),
      .apf_input_cont1_joy (cont1_joy),
      .apf_input_cont1_trig(cont1_trig),

      .apf_input_cont2_key (cont2_key),
      .apf_input_cont2_joy (cont2_joy),
      .apf_input_cont2_trig(cont2_trig),

      .apf_input_cont3_key (cont3_key),
      .apf_input_cont3_joy (cont3_joy),
      .apf_input_cont3_trig(cont3_trig),

      .apf_input_cont4_key (cont4_key),
      .apf_input_cont4_joy (cont4_joy),
      .apf_input_cont4_trig(cont4_trig),

      .apf_interact_address(interact_addr),
      .apf_interact_data(interact_data),
      .apf_interact_q(interact_q),
      .apf_interact_wr(interact_wr),

      .apf_rtc_date_bcd(rtc_date_bcd_s),
      .apf_rtc_time_bcd(rtc_time_bcd_s),
      .apf_rtc_unix_seconds(rtc_counter),

      // .wishbone_ack(ack),
      // .wishbone_adr(addr),
      // .wishbone_bte(bte),
      // .wishbone_cti(cti),
      // .wishbone_cyc(cyc),
      // .wishbone_dat_r(data_read),
      // .wishbone_dat_w(data_write),
      // .wishbone_err(err),
      // .wishbone_sel(sel),
      // .wishbone_stb(stb),
      // .wishbone_we(we),

      .wishbone_master_ack(apf_master_ack),
      .wishbone_master_adr(apf_master_addr),
      .wishbone_master_bte(apf_master_bte),
      .wishbone_master_cti(apf_master_cti),
      .wishbone_master_cyc(apf_master_cyc),
      .wishbone_master_dat_r(apf_master_data_read),
      .wishbone_master_dat_w(apf_master_data_write),
      .wishbone_master_err(apf_master_err),
      .wishbone_master_sel(apf_master_sel),
      .wishbone_master_stb(apf_master_stb),
      .wishbone_master_we(apf_master_we),

      .vga_r(rgb888[23:16]),
      .vga_g(rgb888[15:8]),
      .vga_b(rgb888[7:0]),
      .vga_hsync(hsync),
      .vga_vsync(vsync),
      .vga_de(de),
      .vga_skip(skip),

      .use_jtag(use_jtag_s),

      // Altera JTAG UART
      .altera_reserved_tck(altera_reserved_tck),
      .altera_reserved_tdi(altera_reserved_tdi),
      .altera_reserved_tdo(altera_reserved_tdo),
      .altera_reserved_tms(altera_reserved_tms),

      // Dev cart UART
      .serial_rx(uart_rx),
      .serial_tx(uart_tx),

      // the Game Boy cartridge slot (MIRLO_GB_CART)
      .gb_on(gb_on), .gb_a_dir(gb_a_dir), .gb_a_oe(gb_a_oe), .gb_a(gb_a), .gb_d_o(gb_d_o), .gb_d_dir(gb_d_dir),
      .gb_d_oe(gb_d_oe), .gb_d_i(cart_tran_bank1), .gb_phi(gb_phi), .gb_wr_n(gb_wr_n), .gb_rd_n(gb_rd_n), .gb_cs_n(gb_cs_n),

      .sdram_a(dram_a),
      .sdram_ba(dram_ba),
      .sdram_cas_n(dram_cas_n),
      .sdram_cke(dram_cke),
      .sdram_clock(dram_clk),
      .sdram_dm(dram_dqm),
      .sdram_dq(dram_dq),
      .sdram_ras_n(dram_ras_n),
      .sdram_we_n(dram_we_n)
  );

  // wishbone wishbone (
  //     .clk(clk_sys),

  //     .reset(reset),

  //     .addr(addr),
  //     .bte(bte),
  //     .cti(cti),
  //     .cyc(cyc),
  //     .data_write(data_write),
  //     .data_read(data_read),
  //     .sel(sel),
  //     .stb(stb),
  //     .we(we),
  //     .ack(ack),
  //     .err(err)
  // );

  apf_wishbone_master apf_wishbone_master (
      .clk_74a(clk_74a),
      .clk_sys(clk_sys),

      .bridge_addr(bridge_addr),
      .bridge_wr_data(bridge_wr_data),
      .bridge_rd_data(apf_wishbone_bridge_rd_data),
      .bridge_wr(bridge_wr),
      .bridge_rd(bridge_rd),
      // the game CPU is big-endian, as the N64: SDRAM holds the bridge's
      // big-endian words as their values, so no swap
      .bridge_endian_little(1'b1),

      // Write to start of SDRAM when uploading data
      .ram_data_address(latched_ram_data_address),

      .current_address(current_address),

      .addr(apf_master_addr),
      .bte(apf_master_bte),
      .cti(apf_master_cti),
      .cyc(apf_master_cyc),
      .data_write(apf_master_data_write),
      .data_read(apf_master_data_read),
      .sel(apf_master_sel),
      .stb(apf_master_stb),
      .we(apf_master_we),
      .ack(apf_master_ack),
      .err(apf_master_err)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // Video

  wire vsync;
  wire hsync;
  wire de;
  wire skip;
  wire [23:0] rgb888;                   // the scan-out's 8:8:8 (rtl/soc/n64_video.sv)

  reg [23:0] rgb_delay = 0;

  reg de_delay = 0;
  reg de_delay2 = 0;
  reg vs_delay = 0;
  reg hs_delay = 0;
  reg skip_delay = 0;

  always @(posedge clk_vid) begin
    rgb_delay <= rgb888;

    de_delay  <= de; // period when rgb_delay is valid
    de_delay2 <= de_delay;
    vs_delay  <= vsync;
    hs_delay  <= hsync;
    skip_delay <= skip;
  end

  reg [3:0] de_counter = 0;

  assign video_rgb_clock = clk_vid;
  assign video_rgb_clock_90 = clk_vid_90deg;
  // Outside DE: the scan-out's words for the Pocket -- after a line's DE the scaler slot of the next frame on rgb[14:13], during VS
  // the next frame's features on rgb[3:1] (rtl/soc/n64_video.sv) -- else 0.
  assign video_rgb = de_delay ? rgb_delay : {9'd0, rgb_delay[14:13], 9'd0, rgb_delay[3:1], 1'b0};
  // DE exactly where the (one-cycle-delayed) pixels are: 640 a line, the N64 VI's
  // own line (rtl/soc/n64_video.sv), a pixel a clock of the 12.5 MHz pixel clock.
  assign video_de = de_delay;
  // (no pixel is skipped: the scan-out's vga_skip is 0)
  assign video_skip = skip_delay;
  assign video_vs = vs_delay;
  assign video_hs = hs_delay;

  ////////////////////////////////////////////////////////////////////////////////////////
  // Sound

  audio audio (
      .clk_74b(clk_74b),
      .clk_sys(clk_sys),

      .reset(reset),

      .audio_bus_in(audio_bus_out),
      .audio_bus_wr(audio_bus_wr),

      .audio_playback_en(audio_playback_en),
      .audio_flush(audio_flush),

      .audio_buffer_fill(audio_buffer_fill),

      .audio_mclk(audio_mclk),
      .audio_lrck(audio_lrck),
      .audio_dac (audio_dac)
  );

  ////////////////////////////////////////////////////////////////////////////////////////
  // PLL

  wire clk_sys;
  wire clk_mem;
  wire clk_mem_180deg;
  wire clk_vid;
  wire clk_vid_90deg;
  wire clk_cpu;                     // the game CPU's own: a ratio of clk_sys (rtl/soc/cpu_port.sv)

  wire pll_core_locked;
  wire pll_core_locked_s74;
  synch_3 s01 (
      pll_core_locked,
      pll_core_locked_s74,
      clk_74a
  );

  mf_pllbase mp1 (
      .refclk(clk_74a),
      .rst   (0),

      .outclk_0(clk_sys),
      .outclk_1(clk_mem),
      .outclk_2(clk_mem_180deg),
      .outclk_3(clk_vid),
      .outclk_4(clk_vid_90deg),
      .outclk_5(clk_cpu),

      .locked(pll_core_locked)
  );

endmodule
