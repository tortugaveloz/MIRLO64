// ooc.sh's "glue" unit: mirlo_mips with its three large cores replaced by these stubs, so that what is left is the SoC's
// fabric and the blocks outside the cores, synthesised in a few minutes. Each stub's outputs are a shift register fed by the
// parity of all its inputs: nothing the SoC drives into a core is pruned, nothing it takes from one is a constant.
`default_nettype none

module mips_sys #(parameter BOOTROM = "", parameter int BOOT_WORDS = 1024, parameter bit FAST = 0, parameter bit XWB = 0) (
    input  wire         clk, rst,
    output logic        s_req, s_we, output logic [31:0] s_addr, s_wdata, output logic [3:0] s_strb, output logic s_line, s_wrun,
    input  wire         s_ack, s_rvalid, input wire [31:0] s_rdata, input wire s_busy,
    output logic        x_req, x_we, output logic [31:0] x_addr, x_wdata, output logic [3:0] x_strb,
    input  wire         x_ack, input wire [31:0] x_rdata,
    output wire         st_valid, st_dev, output wire [31:0] st_addr, st_data, output wire [3:0] st_strb,
    input  wire         st_full, st_done, irq, irq_mon, sn_t, input wire [20:0] sn_line, output wire sn_hold,
    output logic [31:0] dbg_pc, dbg_cause, dbg_epc
);
localparam int N = 1 + 1 + 32 + 32 + 4 + 1 + 1 + 1 + 1 + 32 + 32 + 4 + 2 + 32 + 32 + 4 + 1 + 96;
logic [N-1:0] s;
always_ff @(posedge clk) s <= {s[N-2:0], ^{rst, s_ack, s_rvalid, s_rdata, s_busy, x_ack, x_rdata, st_full, st_done, irq, irq_mon, sn_t, sn_line}};
assign {s_req, s_we, s_addr, s_wdata, s_strb, s_line, s_wrun, x_req, x_we, x_addr, x_wdata, x_strb, st_valid, st_dev, st_addr, st_data,
        st_strb, sn_hold, dbg_pc, dbg_cause, dbg_epc} = s;
endmodule

module rsp_top #(parameter int LANES = 4, parameter bit MULW = 0, parameter DIV_ROM = "rsp_div_rom.hex") (
    input  wire         clk, rst, h_we, input wire [3:0] h_addr, input wire [31:0] h_wdata, output logic [31:0] h_rdata, input wire h_rd,
    output logic        h_ready, irq,
    output logic        r_valid, r_we, output logic [23:2] r_addr, output logic [31:0] r_wdata, input wire r_ready, r_rvalid, input wire [31:0] r_rdata,
    output logic        dpc_start_we, dpc_end_we, dpc_status_we, output logic [31:0] dpc_wdata,
    input  wire  [31:0] dpc_start, dpc_end, dpc_current, dpc_status, input wire [9:0] dx_addr, output logic [31:0] dx_data,
    output logic        halted, unimpl,
    output logic        tr_e, output logic [11:0] tr_e_pc, output logic tr_e2, tr_e_we, output logic [4:0] tr_e_wr, output logic tr_we,
    output logic [4:0]  tr_wr, output logic [31:0] tr_val, output logic [3:0] tr_v, output logic tr_vp
);
localparam int N = 32 + 2 + 2 + 22 + 32 + 3 + 32 + 32 + 2;
logic [N-1:0] s;
always_ff @(posedge clk) s <= {s[N-2:0], ^{rst, h_we, h_addr, h_wdata, h_rd, r_ready, r_rvalid, r_rdata, dpc_start, dpc_end, dpc_current, dpc_status, dx_addr}};
assign {h_rdata, h_ready, irq, r_valid, r_we, r_addr, r_wdata, dpc_start_we, dpc_end_we, dpc_status_we, dpc_wdata, dx_data, halted, unimpl} = s;
assign {tr_e, tr_e_pc, tr_e2, tr_e_we, tr_e_wr, tr_we, tr_wr, tr_val, tr_v, tr_vp} = '0;
endmodule

module rdp_top (
    input  wire        clk, rst, cmd_valid, output wire cmd_ready, input wire [31:0] cmd_data,
    output wire        m_cmd_valid, input wire m_cmd_ready, output wire m_cmd_we, output wire [31:0] m_cmd_addr,
    output wire        m_wdata_valid, input wire m_wdata_ready, output wire [31:0] m_wdata, output wire [3:0] m_wdata_we,
    input  wire        m_rdata_valid, input wire [31:0] m_rdata,
    output wire [31:0] sync_count, output wire sync_ev, output wire [31:0] load_count, output wire [15:0] unknown_ops, output wire idle
);
localparam int N = 1 + 1 + 1 + 32 + 1 + 32 + 4 + 32 + 1 + 32 + 16 + 1;
logic [N-1:0] s;
always_ff @(posedge clk) s <= {s[N-2:0], ^{rst, cmd_valid, cmd_data, m_cmd_ready, m_wdata_ready, m_rdata_valid, m_rdata}};
assign {cmd_ready, m_cmd_valid, m_cmd_we, m_cmd_addr, m_wdata_valid, m_wdata, m_wdata_we, sync_count, sync_ev, load_count, unknown_ops, idle} = s;
endmodule
`default_nettype wire
