// Same pixel test with the real PSRAM controller (pad electrical timing is
// outside this digital test). The C++ backing store returns each read word
// at the controller's specified latency.
module pocket_video_test (
    input wire clk_vid, rst_vid, clk_sys, rst_sys,
    input wire [2:0] screen_mode,
    input wire docked,
    input wire [9:0] source_x, source_y, source_w, source_h,
    input wire [23:0] rgb,
    input wire de, hs, vs, skip,
    output wire [23:0] video_rgb,
    output wire video_de, video_hs, video_vs, video_skip,
    output wire m_valid, m_we, m_last,
    output wire [21:0] m_addr,
    output wire [31:0] m_wdata,
    output wire m_ready, m_rvalid, m_idle, memory_ready,
    input wire [31:0] m_rdata
);
    wire [31:0] read_data;
    pocket_video display (.*,.m_rdata(read_data));
    pocket_video_psram ram (
        .clk(clk_sys), .rst(rst_sys), .z_valid(m_valid), .z_ready(m_ready),
        .z_we(m_we), .z_addr(m_addr), .z_wdata(m_wdata), .z_wmask(4'hf),
        .z_last(m_last), .z_rvalid(m_rvalid), .z_rdata(read_data), .z_idle(m_idle), .cfg(4'b0101), .cfg_wr(1'b0), .ready(memory_ready),
        .wait_err(), .o_a(), .o_dq(), .o_dq_oe(), .o_adv_n(), .o_oe_n(),
        .o_we_n(), .o_cre(), .o_ce_n(), .o_bm_n(), .o_clk_en(),
        .i_dq(m_rdata), .i_wait(2'b00)
    );
endmodule
