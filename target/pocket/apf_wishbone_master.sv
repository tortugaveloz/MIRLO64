module apf_wishbone_master (
    input wire clk_74a,
    input wire clk_sys,

    input wire [31:0] bridge_addr,
    input wire [31:0] bridge_wr_data,
    output wire [31:0] bridge_rd_data,
    input wire bridge_wr,
    input wire bridge_rd,
    input wire bridge_endian_little,

    input wire [31:0] ram_data_address,

    output wire [26:0] current_address,

    output reg [29:0] addr = 0,
    // Wishbone registered feedback flags
    // Burst style extension. Specifies the type of burst
    output reg [1:0] bte = 0,
    // Cycle type identifier. Indicates what kind of burst cycle is being used
    output reg [2:0] cti = 0,
    // Cycle. High during the duration of the bus operations
    output reg cyc = 0,
    output wire [31:0] data_write,
    // Which bytes are active in read/write
    output reg [3:0] sel = 0,
    // Strobe. Need to receive ack or err after this
    output reg stb = 0,
    // Work already visible in the system clock domain, including queued work
    // before cyc rises. Used to keep another DMA from taking the shared port.
    output wire pending,
    // Write enable
    output reg we = 0,

    input wire ack,
    input wire [31:0] data_read,
    input reg err
);

  wire [25:0] write_addr;

  assign current_address = write_addr;


  ////////////////////////////////////////////////////////////////////////////////////////
  // FIFO
  // Derived from data loader

  localparam ADDRESS_MASK_UPPER_4 = 4'h0;

  wire mem_write_empty;
  wire mem_read_empty;
  wire addr_read_empty;

  wire [57:0] fifo_write_out;
  wire [31:0] fifo_read_out;
  wire [25:0] bridge_addr_s;

  assign {write_addr, data_write} = fifo_write_out;

  wire [31:0] endian_corrected_bridge_wr_data = bridge_endian_little ? bridge_wr_data :
    {
      bridge_wr_data[7:0],
      bridge_wr_data[15:8],
      bridge_wr_data[23:16],
      bridge_wr_data[31:24]
    };

  assign bridge_rd_data = bridge_endian_little ? fifo_read_out :
    {
      fifo_read_out[7:0],
      fifo_read_out[15:8],
      fifo_read_out[23:16],
      fifo_read_out[31:24]
    };

  // (the three FIFOs are four words deep and were made of logic -- ~330 ALMs
  // of a device that has none to spare: a block of RAM each, which it has)
  dcfifo sdram_write_fifo (
      .wrclk(clk_74a),
      .rdclk(clk_sys),

      .wrreq(bridge_wrreq),
      .data ({bridge_addr[27:2], endian_corrected_bridge_wr_data}),

      // Immediately request if we're idle
      .rdreq(~mem_write_empty && state == NONE),
      .q(fifo_write_out),

      .rdempty(mem_write_empty)
      // .wrempty(),
      // .aclr(),
      // .eccstatus(),
      // .rdfull(),
      // .rdusedw(),
      // .wrfull(),
      // .wrusedw()
  );
  defparam sdram_write_fifo.clocks_are_synchronized = "FALSE",
      sdram_write_fifo.intended_device_family = "Cyclone V", sdram_write_fifo.lpm_numwords = 4,
      sdram_write_fifo.lpm_showahead = "OFF", sdram_write_fifo.lpm_type = "dcfifo",
      sdram_write_fifo.lpm_width = 58, sdram_write_fifo.lpm_widthu = 2,
      sdram_write_fifo.overflow_checking = "OFF", sdram_write_fifo.rdsync_delaypipe = 5,
      sdram_write_fifo.underflow_checking = "OFF", sdram_write_fifo.use_eab = "ON", sdram_write_fifo.lpm_hint = "RAM_BLOCK_TYPE=M10K",
      sdram_write_fifo.wrsync_delaypipe = 5;

  dcfifo sdram_read_fifo (
      .wrclk(clk_sys),
      .rdclk(clk_74a),

      // As soon as we receive data from the Wishbone bus, stick it into FIFO
      .wrreq(ack && state == READ),
      .data (data_read),

      .rdreq(~mem_read_empty),
      .q(fifo_read_out),

      .rdempty(mem_read_empty)
      // .wrempty(),
      // .aclr(),
      // .eccstatus(),
      // .rdfull(),
      // .rdusedw(),
      // .wrfull(),
      // .wrusedw()
  );
  defparam sdram_read_fifo.clocks_are_synchronized = "FALSE",
      sdram_read_fifo.intended_device_family = "Cyclone V", sdram_read_fifo.lpm_numwords = 4,
      sdram_read_fifo.lpm_showahead = "OFF", sdram_read_fifo.lpm_type = "dcfifo",
      sdram_read_fifo.lpm_width = 32, sdram_read_fifo.lpm_widthu = 2,
      sdram_read_fifo.overflow_checking = "OFF", sdram_read_fifo.rdsync_delaypipe = 5,
      sdram_read_fifo.underflow_checking = "OFF", sdram_read_fifo.use_eab = "ON", sdram_read_fifo.lpm_hint = "RAM_BLOCK_TYPE=M10K",
      sdram_read_fifo.wrsync_delaypipe = 5;

  dcfifo bridge_addr_fifo (
      .wrclk(clk_74a),
      .rdclk(clk_sys),

      .wrreq(bridge_rdreq),
      .data (bridge_addr[27:2]),

      // Only remove an address when the idle master will start that read.
      .rdreq(~addr_read_empty && state == NONE && mem_write_empty),
      .q(bridge_addr_s),

      .rdempty(addr_read_empty)
      // .wrempty(),
      // .aclr(),
      // .eccstatus(),
      // .rdfull(),
      // .rdusedw(),
      // .wrfull(),
      // .wrusedw()
  );
  defparam bridge_addr_fifo.clocks_are_synchronized = "FALSE",
      bridge_addr_fifo.intended_device_family = "Cyclone V", bridge_addr_fifo.lpm_numwords = 4,
      bridge_addr_fifo.lpm_showahead = "OFF", bridge_addr_fifo.lpm_type = "dcfifo",
      bridge_addr_fifo.lpm_width = 26, bridge_addr_fifo.lpm_widthu = 2,
      bridge_addr_fifo.overflow_checking = "OFF", bridge_addr_fifo.rdsync_delaypipe = 5,
      bridge_addr_fifo.underflow_checking = "OFF", bridge_addr_fifo.use_eab = "ON", bridge_addr_fifo.lpm_hint = "RAM_BLOCK_TYPE=M10K",
      bridge_addr_fifo.wrsync_delaypipe = 5;

  reg prev_bridge_wr = 0;
  reg prev_bridge_rd = 0;

  wire bridge_wrreq = ~prev_bridge_wr && bridge_wr && bridge_addr[31:28] == ADDRESS_MASK_UPPER_4;
  wire bridge_rdreq = ~prev_bridge_rd && bridge_rd && bridge_addr[31:28] == ADDRESS_MASK_UPPER_4;

  // Receive APF writes and write to FIFO
  always @(posedge clk_74a) begin
    prev_bridge_wr <= bridge_wr;
    prev_bridge_rd <= bridge_rd;
  end

  localparam NONE = 0;
  localparam WRITE = 1;
  localparam READ = 2;
  // localparam WRITE_WAIT_ACK = 2;

  reg [2:0] state = 3'h0;
  assign pending = state != NONE || !mem_write_empty || !addr_read_empty;

  // Registered (68.5 MHz): combinationally, the FIFO output + offset add
  // reached the SoC's Wishbone arbiter/decoder too late. The FIFO data is
  // valid from the first cycle of WRITE/READ and cyc rises one cycle later,
  // so the registered address is there when the bus sees the request.
  //
  // Every bridge access is absolute: bridge 0 = SDRAM 0x4000_0000. The
  // Pocket's own ones (a non-deferload slot load, a nonvolatile slot's flush
  // on quit) use the slot's data.json address; the core's data slot transfers
  // pass their RAM address as the bridge address (core_top's
  // target_dataslot_bridgeaddr). 0x1000_0000 words is a single bit above the
  // 25-bit offset: no adder (the old base + offset add did not fit, 2026-09-27).
  wire [25:0] word = state == READ ? bridge_addr_s : write_addr;
  always @(posedge clk_sys)
    addr <= {5'b01000, word[24:0]};  // bit 28: word 0x1000_0000 = byte 0x4000_0000

  always @(posedge clk_sys) begin
    // Write all bytes
    sel <= 4'hF;

    cti <= 3'h0;

    case (state)
      NONE: begin
        cyc <= 0;
        stb <= 0;

        we  <= 0;

        if (~mem_write_empty) begin
          // Send request to SDRAM slave
          // FIFO read has already begun, will be done when we enter next state
          state <= WRITE;
        end else if (~addr_read_empty) begin
          // Receive read addr from bridge
          // Start request to SDRAM slave
          // FIFO read has already begin, will be done when we enter next state
          state <= READ;
        end
      end
      WRITE: begin
        cyc <= 1;
        stb <= 1;

        we  <= 1;

        if (ack) begin
          state <= NONE;

          cyc   <= 0;
          stb   <= 0;
        end
      end
      READ: begin
        cyc <= 1;
        stb <= 1;

        we  <= 0;

        if (ack) begin
          state <= NONE;

          cyc   <= 0;
          stb   <= 0;
        end
      end
    endcase
  end

endmodule
