// The RSP's clock crossing (rtl/soc/rsp_clk.sv, rsp_gate.sv, rsp_xing.sv) against stand-ins: a random "RSP" on the gated
// clock -- host registers, DMA requests, DPC writes --, and a random SoC on clk_sys -- the CPU's register accesses, an
// arbiter port with back-pressure, SDRAM data coming back late, the DPC's registers. clk (62.5 MHz) and clk_c (93.75 MHz)
// are inputs of the C++ driver, phase-locked 3:2 as the PLL's outputs are. It checks:
//   - the DMA's requests reach the arbiter in order, once each, intact; a read's words come back to the RSP in order,
//     once each, with their own data
//   - a host access is done once (one h_we or h_rd), its read data is the register's, a write is seen by the next read
//   - the RSP's DPC writes arrive once each, in order, after every DMA request before them has been taken by the arbiter,
//     and the RSP's next read of the DPC's registers finds them (the RSP waits for that)
//   - no FIFO overflows
// Plusargs: +dn=<duty_n> +dd=<duty_d> +pr=<p_ready %> +preq=<RSP request %> +pdpc=<DPC write per mille> +ph=<host access %> +lat=<read latency>
//           +cycles=<sys cycles> +reset=<mid-run resets>
`default_nettype none

module tb_xing (
    input wire clk, clk_c, rst_in, rstc_in,
    output logic [31:0] errors,
    output logic [31:0] st_dma_w, st_dma_r, st_host, st_dpc, st_maxq, st_maxb, st_edges, st_dpc_stall, st_lat
);
integer dn = 1, dd = 1, pr = 100, preq = 100, pdpc = 5, ph = 5, lat = 8, rd_pct = 50;
initial begin
    void'($value$plusargs("dn=%d", dn)); void'($value$plusargs("dd=%d", dd)); void'($value$plusargs("pr=%d", pr));
    void'($value$plusargs("preq=%d", preq)); void'($value$plusargs("pdpc=%d", pdpc)); void'($value$plusargs("ph=%d", ph));
    void'($value$plusargs("lat=%d", lat)); void'($value$plusargs("rd=%d", rd_pct));
    errors = 0;
end

logic rst = 1, rst_c = 1;
always @(posedge clk) rst <= rst_in;
always @(posedge clk_c) rst_c <= rstc_in;

// ---------------------------------------------------------------- the DUT
wire gclk;
wire h_we, h_rd; wire [3:0] h_addr; wire [31:0] h_wdata; logic [31:0] h_rdata; logic h_ready;
logic r_valid, r_we; logic [23:2] r_addr; logic [31:0] r_wdata; wire r_ready, r_rvalid; wire [31:0] r_rdata;
logic dpc_start_we, dpc_end_we, dpc_status_we; logic [31:0] dpc_wdata;
wire [31:0] dpc_start_r, dpc_end_r, dpc_current_r, dpc_status_r;
wire dq_t, dpa_t;
logic s_h_req, s_h_we; logic [3:0] s_h_addr; logic [31:0] s_h_wdata; wire s_h_done; wire [31:0] s_h_rdata;
wire p_valid, p_we; wire [22:2] p_addr; wire [31:0] p_wdata; logic p_ready, p_rvalid; logic [31:0] p_rdata;
wire s_dp_go; wire [2:0] s_dp_k; wire [31:0] s_dp_wdata;
logic [23:0] s_dpc_start, s_dpc_end, s_dpc_current; logic [31:0] s_dpc_status;
logic [3:0] duty_n, duty_d;
always @(posedge clk_c) begin duty_n <= 4'(dn); duty_d <= 4'(dd); end
rsp_clk clkgen (.clk_c, .rst_c, .duty_n, .duty_d, .strobe(dpc_start_we || dpc_end_we || dpc_status_we), .dpa_t, .gclk);
rsp_xing xing (.clk_r(gclk), .rst_r(rst_c), .clk_c, .h_we, .h_rd, .h_addr, .h_wdata, .h_rdata, .h_ready,
    .r_valid, .r_we, .r_addr, .r_wdata, .r_ready, .r_rvalid, .r_rdata,
    .dpc_start_we, .dpc_end_we, .dpc_status_we, .dpc_wdata, .dpc_start_r, .dpc_end_r, .dpc_current_r, .dpc_status_r, .dq_t,
    .clk, .rst, .s_h_req, .s_h_we, .s_h_addr, .s_h_wdata, .s_h_done, .s_h_rdata,
    .p_valid, .p_we, .p_addr, .p_wdata, .p_ready, .p_rvalid, .p_rdata,
    .s_dp_go, .s_dp_k, .s_dp_wdata, .dpa_t, .s_dpc_start, .s_dpc_end, .s_dpc_current, .s_dpc_status);

function automatic bit chance(input integer pct);
    return ($urandom % 100) < pct;
endfunction

// ---------------------------------------------------------------- the RSP's side
// host registers: 0..6 plain, 7 the semaphore (a read sets it, a write clears it), 8 the PC
logic [31:0] hregs [16];
logic        sem;
integer      n_hw, n_hr;                    // the writes and reads the RSP's port took
assign h_rdata = (h_addr == 4'd7) ? {31'd0, sem} : hregs[h_addr];
always @(posedge gclk) begin
    if (rst_c) begin sem <= 0; n_hw <= 0; n_hr <= 0; for (int i = 0; i < 16; i++) hregs[i] <= 0; end
    else begin
        h_ready <= chance(70);
        if (h_we) begin
            n_hw <= n_hw + 1;
            if (h_addr == 4'd7) sem <= 0; else hregs[h_addr] <= h_wdata;
        end
        if (h_rd) begin n_hr <= n_hr + 1; if (h_addr == 4'd7) sem <= 1; end
    end
end

// DMA requests: held until taken. A read's word is {address, ordinal}
typedef struct packed { logic we; logic [22:2] a; logic [31:0] d; } req_t;
req_t   rq_exp [$];                         // pushed when the RSP's request is taken
logic [31:0] rd_exp [$];
integer rd_ord;
integer taken, taken_at_dpc;
logic   dpc_pend_dma;                       // (DPC ordering: the number of requests taken by the strobe)
integer dma_rd_words;
always @(posedge gclk) begin
    if (rst_c) begin r_valid <= 0; rd_ord <= 0; taken <= 0; end
    else begin
        if (r_valid && r_ready) begin
            req_t q; q.we = r_we; q.a = r_addr[22:2]; q.d = r_wdata;
            rq_exp.push_back(q);
            taken <= taken + 1;
            if (!r_we) begin rd_exp.push_back({r_addr[22:2], 11'(rd_ord)}); rd_ord <= rd_ord + 1; end
        end
        if (!r_valid || r_ready) begin
            if (go_r && chance(preq)) begin
                r_valid <= 1; r_we <= chance(100 - rd_pct); r_addr <= {1'b0, 21'($urandom)}; r_wdata <= $urandom;
            end else r_valid <= 0;
        end
    end
end
// read words back
always @(posedge gclk) if (!rst_c && r_rvalid) begin
    if (rd_exp.size() == 0) begin errors <= errors + 1; $display("%0t ERR: a word back with none expected", $time); end
    else begin
        logic [31:0] e; e = rd_exp.pop_front();
        if (e !== r_rdata) begin errors <= errors + 1; $display("%0t ERR: word back %h, expected %h", $time, r_rdata, e); end
        st_dma_r <= st_dma_r + 1;
    end
end

// DPC writes: the RSP makes one now and then; its next edge after that finds them in the shadows
typedef struct packed { logic [2:0] k; logic [31:0] d; } dw_t;
dw_t    dw_exp [$];                         // the writes in the order made
integer dw_taken_dma [$];                   // ... and the DMA requests taken before each
logic [31:0] rsp_start, rsp_end, rsp_status;// what the RSP last wrote
logic        chk_next;                      // the first edge after a write
integer      stall_cnt;
always @(posedge gclk) begin
    if (rst_c) begin
        dpc_start_we <= 0; dpc_end_we <= 0; dpc_status_we <= 0; chk_next <= 0;
        rsp_start <= 0; rsp_end <= 0; rsp_status <= 0;
    end else begin
        // (the three strobes and the data are state: held until this edge takes them)
        if (dpc_start_we || dpc_end_we || dpc_status_we) begin
            dw_t w; w.k = {dpc_status_we, dpc_end_we, dpc_start_we}; w.d = dpc_wdata;
            dw_exp.push_back(w); dw_taken_dma.push_back(taken + ((r_valid && r_ready) ? 1 : 0));
            if (dpc_start_we) rsp_start <= {8'd0, dpc_wdata[23:0]};
            if (dpc_end_we) rsp_end <= {8'd0, dpc_wdata[23:0]};
            if (dpc_status_we) rsp_status <= dpc_wdata;
            chk_next <= 1;
        end else chk_next <= 0;
        if (chk_next) begin
            // the RSP "reads" the DPC's registers at its first edge after the write
            if (dpc_start_r !== rsp_start || dpc_end_r !== rsp_end || dpc_status_r !== rsp_status) begin
                errors <= errors + 1;
                $display("%0t ERR: DPC shadows %h %h %h, the RSP wrote %h %h %h", $time, dpc_start_r, dpc_end_r, dpc_status_r, rsp_start, rsp_end, rsp_status);
            end
        end
        dpc_start_we <= 0; dpc_end_we <= 0; dpc_status_we <= 0;
        if (go_r && ($urandom % 1000) < pdpc && !(dpc_start_we || dpc_end_we || dpc_status_we)) begin
            case ($urandom % 3) 0: dpc_start_we <= 1; 1: dpc_end_we <= 1; default: dpc_status_we <= 1; endcase
            dpc_wdata <= $urandom;
        end
    end
end
always @(posedge gclk) st_edges <= st_edges + 1;

// ---------------------------------------------------------------- the SoC's side
// the arbiter's port: ready with back-pressure; a read's word comes back after a latency, in order
integer acc_taken;                          // requests the arbiter has taken
logic [4:0] go_s_n, go_r_n;                 // cycles since each side's reset ended: the traffic starts after 8
wire go_s = go_s_n == 5'd8, go_r = go_r_n == 5'd8;
always @(posedge clk) go_s_n <= rst ? 5'd0 : go_s ? go_s_n : go_s_n + 5'd1;
always @(posedge gclk) go_r_n <= rst_c ? 5'd0 : go_r ? go_r_n : go_r_n + 5'd1;
integer stall_left;
typedef struct packed { logic [31:0] d; integer due; } rsp_t;
rsp_t   rs_q [$];
integer cyc, rd_issued;
integer st_inflight_max;
always @(posedge clk) begin
    if (rst) begin cyc <= 0; rd_issued <= 0; p_rvalid <= 0; stall_left <= 0; p_ready <= 0; acc_taken <= 0; end
    else begin
        cyc <= cyc + 1;
        // back-pressure: random, with long stalls now and then
        if (stall_left > 0) begin stall_left <= stall_left - 1; p_ready <= 0; end
        else if (pr < 100 && ($urandom % 400) == 0) begin stall_left <= $urandom % 60; p_ready <= 0; end
        else p_ready <= chance(pr);
        // an accepted request: checked against the RSP's
        if (p_valid && p_ready) begin
            if (rq_exp.size() == 0) begin errors <= errors + 1; $display("%0t ERR: a request at the arbiter with none made", $time); end
            else begin
                req_t e; e = rq_exp.pop_front();
                if (e.we !== p_we || e.a !== p_addr || (e.we && e.d !== p_wdata)) begin
                    errors <= errors + 1; $display("%0t ERR: request %b %h %h, expected %b %h %h", $time, p_we, p_addr, p_wdata, e.we, e.a, e.d);
                end
                if (p_we) st_dma_w <= st_dma_w + 1;
                else begin
                    rsp_t r; r.d = {p_addr, 11'(rd_issued)}; r.due = cyc + 1 + ($urandom % (lat + 1)); rs_q.push_back(r);
                    rd_issued <= rd_issued + 1;
                end
                acc_taken <= acc_taken + 1;
            end
        end
        // data back
        p_rvalid <= 0;
        if (rs_q.size() != 0 && rs_q[0].due <= cyc && chance(90)) begin
            p_rvalid <= 1; p_rdata <= rs_q[0].d; void'(rs_q.pop_front());
        end
        if (rs_q.size() > st_inflight_max) st_inflight_max <= rs_q.size();
    end
end

// the DPC's registers: the RSP's writes arrive in order; the CPU's own writes (to DPC_CURRENT here) wait for a free cycle
integer dw_n, dw_head;
always @(posedge clk) begin
    if (rst) begin s_dpc_start <= 0; s_dpc_end <= 0; s_dpc_current <= 0; s_dpc_status <= 0; end
    else begin
        if (s_dp_go) begin
            if (dw_exp.size() == 0) begin errors <= errors + 1; $display("%0t ERR: a DPC write with none made", $time); end
            else begin
                dw_t e; integer need; e = dw_exp.pop_front(); need = dw_taken_dma.pop_front();
                if (e.k !== s_dp_k || e.d !== s_dp_wdata) begin errors <= errors + 1; $display("%0t ERR: DPC write %b %h, expected %b %h", $time, s_dp_k, s_dp_wdata, e.k, e.d); end
                if (acc_taken < need) begin errors <= errors + 1; $display("%0t ERR: a DPC write before the DMA requests before it (%0d of %0d taken)", $time, acc_taken, need); end
                if (s_dp_k[0]) s_dpc_start <= s_dp_wdata[23:0];
                if (s_dp_k[1]) s_dpc_end <= s_dp_wdata[23:0];
                if (s_dp_k[2]) s_dpc_status <= s_dp_wdata;
                st_dpc <= st_dpc + 1;
            end
        end else if (chance(10)) s_dpc_current <= $urandom;     // the CPU's write
    end
end

// the CPU's accesses to the SP registers
logic [31:0] cref [16];
logic        csem;
integer      c_done_cnt, h_wait_cyc, h_lat_sum;
always @(posedge clk) begin
    if (rst) begin s_h_req <= 0; csem <= 0; c_done_cnt <= 0; for (int i = 0; i < 16; i++) cref[i] <= 0; end
    else begin
        if (s_h_req) begin
            h_wait_cyc <= h_wait_cyc + 1;
            if (s_h_done) begin
                s_h_req <= 0; c_done_cnt <= c_done_cnt + 1; st_host <= st_host + 1; h_lat_sum <= h_lat_sum + h_wait_cyc + 1; st_lat <= h_lat_sum + h_wait_cyc + 1;
                if (!s_h_we) begin
                    logic [31:0] e; e = (s_h_addr == 4'd7) ? {31'd0, csem} : cref[s_h_addr];
                    if (s_h_rdata !== e) begin errors <= errors + 1; $display("%0t ERR: host read %0d gave %h, expected %h", $time, s_h_addr, s_h_rdata, e); end
                    if (s_h_addr == 4'd7) csem <= 1;
                end else begin
                    if (s_h_addr == 4'd7) csem <= 0; else cref[s_h_addr] <= s_h_wdata;
                end
            end
        end else if (go_s && chance(ph)) begin
            s_h_req <= 1; s_h_we <= chance(50); s_h_addr <= 4'(($urandom % 10) < 9 ? $urandom % 9 : 7); s_h_wdata <= $urandom; h_wait_cyc <= 0;
        end
    end
end
// every access done exactly once at the RSP's port
always @(posedge clk) if (!rst && s_h_done) begin
    if (n_hw + n_hr != c_done_cnt + 1) begin errors <= errors + 1; $display("%0t ERR: %0d port operations for %0d accesses", $time, n_hw + n_hr, c_done_cnt + 1); end
end

// ---------------------------------------------------------------- sizes
always @(posedge clk) if (!rst) begin
    automatic integer qo = $unsigned(xing.qw - xing.rp) & 'hF; automatic integer bo = $unsigned(xing.rw - xing.rr_r) & 'h1F;
    if (qo > st_maxq) st_maxq <= qo;
    if (bo > st_maxb) st_maxb <= bo;
    if (qo > 8) begin errors <= errors + 1; $display("%0t ERR: the requests' FIFO holds %0d", $time, qo); end
    if (bo > 16) begin errors <= errors + 1; $display("%0t ERR: the words' FIFO holds %0d", $time, bo); end
end
// a reset: the models start again
always @(posedge clk) if (rst) begin
    rq_exp.delete(); rs_q.delete(); dw_exp.delete(); dw_taken_dma.delete();
end
always @(posedge gclk) if (rst_c) rd_exp.delete();
initial begin
    st_dma_w = 0; st_dma_r = 0; st_host = 0; st_dpc = 0; st_maxq = 0; st_maxb = 0; st_edges = 0; st_dpc_stall = 0; st_inflight_max = 0; h_lat_sum = 0;
    for (int i = 0; i < 16; i++) hregs[i] = 0;
end
endmodule
`default_nettype wire
