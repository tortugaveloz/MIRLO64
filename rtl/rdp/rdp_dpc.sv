// The RDP's command registers (the N64's DPC_START, DPC_END, DPC_CURRENT,
// DPC_STATUS) and its command fetch: the 64-bit commands between CURRENT and
// END are read from the N64's memory, as the RSP's microcode leaves them
// there, and handed on as 32-bit words (to the RDP's command FIFO).
//
// As the N64's: a write of START is held (start_valid) until the next write
// of END, which makes CURRENT = START and sets END; while commands are still
// being fetched that pair waits (end_valid) for them. A write of END alone
// extends the run. CURRENT moves as commands are fetched.
//
// With XBUS set (DPC_STATUS) the commands are in the RSP's DMEM instead, and
// START, END and CURRENT are addresses in it: they are read from the RSP's
// copy of DMEM for the RDP (rsp_top's dx port), a word a cycle.
//
// A read of START or END is the last value written, a pair waiting included (end_rd; START is that already); CURRENT is the
// transfer under way. A microcode that begins its output where the RDP's list ends (a task or a microcode that starts while the
// RDP still has a list) takes DPC_END, and with the END of the list under way it would write over what the waiting pair is to run.
`default_nettype none

module rdp_dpc (
    input  wire         clk,
    input  wire         rst,
    input  wire         start_we, end_we, status_we,
    input  wire  [31:0] wdata,
    output logic [23:0] start, end_, current,
    output wire  [23:0] end_rd,             // what a read of DPC_END gives: the last value written (a pair waiting has its END here)
    output logic [31:0] status,
    input  wire         rdp_busy,           // the RDP has commands to do
    // the N64's memory: 32-bit words, in order
    output logic        m_valid,
    output logic [20:0] m_addr,             // word address in the 8 MiB
    input  wire         m_ready,
    input  wire         m_rvalid,
    input  wire  [31:0] m_rdata,
    // the RSP's DMEM (XBUS): a word, the cycle after its address
    output logic [9:0]  dx_addr,
    input  wire  [31:0] dx_data,
    // command words
    output logic        c_valid,
    output logic [31:0] c_data,
    input  wire         c_ready
);
logic        start_valid, end_valid, xbus, freeze, flush;
logic [23:0] end_pend;
logic [21:0] fp;                            // the next word to ask for (word address; one more bit: END's)
logic [2:0]  out;                           // reads asked for, not yet back
// words back, waiting to be taken: four
logic [31:0] q [4];
logic [2:0]  qw, qr;
wire  [2:0]  qn = qw - qr;
// busy: commands between CURRENT and END -- END behind CURRENT is nothing to do, not a run all the way
// round the address space. (F3DZEX writes DPC_END = 0 at the start of every task, with CURRENT where the
// last list ended: with "!=" that was an RDP chewing through the whole RDRAM and beyond, a SYNC FULL
// out of every few thousand garbage words -- OoT's scheduler took the DP interrupts for its own tasks'.)
wire         busy = current < end_;
wire         want = busy && !freeze && fp != end_[23:2] && (qn + out) < 3'd4;
wire         x_go = want && xbus;
logic        x_rv;
wire         rv = xbus ? x_rv : m_rvalid;
wire  [31:0] rd = xbus ? dx_data : m_rdata;

assign end_rd  = end_valid ? end_pend : end_;
assign c_valid = qn != 0;
assign c_data  = q[qr[1:0]];
assign m_valid = want && !xbus;
assign m_addr  = fp[20:0];
assign dx_addr = fp[9:0];
assign status  = {21'd0, start_valid, end_valid, 1'b0, 1'b1, {3{busy | rdp_busy}}, busy | rdp_busy, flush, freeze, xbus};

always_ff @(posedge clk) begin
    if (rst) begin
        start <= '0; end_ <= '0; current <= '0; end_pend <= '0; fp <= '0;
        start_valid <= 0; end_valid <= 0; xbus <= 0; freeze <= 0; flush <= 0;
        out <= '0; qw <= '0; qr <= '0; x_rv <= 0;
    end else begin
        // the fetch
        x_rv <= x_go;
        if ((m_valid && m_ready) || x_go) fp <= fp + 22'd1;
        out <= out + 3'((m_valid && m_ready) || x_go) - 3'(rv);
        if (rv) begin
            q[qw[1:0]] <= rd; qw <= qw + 3'd1;
            if (qw[0]) current <= current + 24'd8;      // (a command's second word)
        end
        if (c_valid && c_ready) qr <= qr + 3'd1;

        // the registers
        if (start_we) begin start <= {wdata[23:3], 3'd0}; start_valid <= 1; end
        if (end_we) begin
            if (!start_valid) end_ <= {wdata[23:3], 3'd0};
            else if (busy) begin end_pend <= {wdata[23:3], 3'd0}; end_valid <= 1; end
            else begin current <= start; fp <= start[23:2]; end_ <= {wdata[23:3], 3'd0}; start_valid <= 0; end
        end else if (end_valid && !busy && out == 0) begin
            current <= start; fp <= start[23:2]; end_ <= end_pend; start_valid <= 0; end_valid <= 0;
        end
        if (status_we) begin
            if (wdata[0]) xbus <= 0;
            if (wdata[1]) xbus <= 1;
            if (wdata[2]) freeze <= 0;
            if (wdata[3]) freeze <= 1;
            if (wdata[4]) flush <= 0;
            if (wdata[5]) flush <= 1;
        end
    end
end
endmodule
`default_nettype wire
