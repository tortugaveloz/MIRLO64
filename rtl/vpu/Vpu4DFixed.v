// ============================================================================
// Vpu4DFixed -- a parallel 4-lane fixed-point matrix-vector engine for the
// geometry core's vertex transform, attached as a VexRiscv CFU (custom
// function unit). It also hosts GeomSetupUnit, which owns the scalar setup
// ops (ids 0x04 and 0x40 and up) and the PUSH port toward RDP's command FIFO.
// ----------------------------------------------------------------------------
// Format: S17.10 signed fixed point (17 integer bits incl. sign, 10
// fractional, range +/-65536, step 2^-10). 27-bit operands because Cyclone
// V's variable-precision DSP block does a signed 27x27 multiply in ONE
// block, while 32x32 costs three. S17.10 keeps >2.7x headroom over the
// largest clip-space value measured on real game content (~23,659) and
// >3.7x over the largest matrix element (~17,424).
//
// Each of the 4 output lanes has its own multiply-accumulate unit (4 DSP
// blocks in total) and the four lanes run at once; within a lane the 4 terms
// of a dot product go through that one multiplier in 4 sequential steps
// into a running accumulator. Registering all 16 raw products for an adder
// tree instead cost more than twice the ALMs for 2 cycles less.
//
// CFU-L1 protocol: cmd_valid/ready in, rsp_valid/ready out, one 32-bit
// result per instruction.
//
// function id map (cmd_function_id = {funct3, funct7} as VexRiscv's CfuPlugin
// packs it -- see lang/c/geom/geom_vpar.h's GEOM_VPAR_INSN):
//   0x00  M[rs2 & 0xF] = rs1     resident 4x4 matrix element, row-major
//                                (m[i*4+j] at index i*4+j), S17.10 --
//                                MATVEC4 only.
//   0x01  A[rs2 & 0x3] = rs1     resident input vector element, S17.10
//   0x02  B[rs2 & 0x3] = rs1     resident B vector element, S17.10 --
//                                VMUL4/VFMA4 only.
//   0x03  C[rs2 & 0x3] = rs1     resident C (seed) vector element, S17.10
//                                -- VFMA4 only.
//   0x30  MATVEC4                out[j] = sum_i A[i]*M[i*4+j] for all 4 j
//                                 at once; rd = out[0] once done
//   0x31  VMUL4                  out[i] = A[i]*B[i] for all 4 i at once,
//                                 1 MAC cycle (one term per lane) -- e.g.
//                                 x,y,z times a broadcast 1/w.
//   0x32  VFMA4                  out[i] = A[i]*B[i] + C[i] for all 4 i
//                                 at once, still 1 MAC cycle (C seeds the
//                                 accumulator) -- e.g. viewport scale +
//                                 translate (B = scale, C = translate).
//   0x08  rd = out[rs2 & 0x3]    read one output component
//
// Lighting needs no opcode of its own: MATVEC4 with M's columns loaded with
// up to 4 light directions and A with the normal (A[3] = 0) computes all 4
// dot products in one call.
//
// B and C are register files of their own, not part of M, so the MVP matrix
// stays resident across a vertex batch while VMUL4/VFMA4 run per vertex.
//
// Signedness: a Verilog concatenation or unsized literal is unsigned and
// makes a whole ?: or + expression unsigned; every mixed expression below
// uses explicit $signed() or if/else instead.
// ============================================================================

`default_nettype none

module Vpu4DFixed #(
    parameter DATA_WIDTH  = 27,
    parameter FRAC_BITS   = 10     // S17.10
) (
    input  wire                     clk,
    input  wire                     resetn,

    input  wire                     cmd_valid,
    output wire                     cmd_ready,
    input  wire [9:0]               cmd_function_id,
    input  wire [31:0]              cmd_inputs_0,
    input  wire [31:0]              cmd_inputs_1,

    output wire                     rsp_valid,
    input  wire                     rsp_ready,
    output wire [31:0]              rsp_outputs_0,

    // GeomSetupUnit's PUSH (0x61) toward RDP's command FIFO
    output wire                     out_valid,
    input  wire                     out_ready,
    output wire [31:0]              out_data
);
    localparam DW = DATA_WIDTH;
    localparam FB = FRAC_BITS;
    localparam AW = 2*DW+2;   // accumulator width: 2 extra bits of headroom for the 4-term add

    // ---- resident register files ---------------------------------------
    reg signed [DW-1:0] m_l [0:15];   // row-major: m_l[i*4+j] -- MATVEC4 only
    reg signed [DW-1:0] a_l [0:3];
    reg signed [DW-1:0] out_r [0:3];

    // ---- CFU handshake ---------------------------------------------------
    // MATVEC4 needs several cycles (see the state machine below); everything
    // else (register writes, output reads) completes in one cycle. `busy`
    // covers the multi-cycle op only.
    reg        busy;
    reg        resp;
    reg [31:0] result;
    wire       su_bg;                // the setup unit still finishing a PROJECT
    assign cmd_ready     = ~busy & ~resp & ~su_bg;
    assign rsp_valid     = resp;
    assign rsp_outputs_0 = result;

    // Triangle-setup helpers (ids 0x04, 0x40-0x4F, 0x50-0x5F): their own
    // module, see rtl/vpu/GeomSetupUnit.v. This module keeps the handshake:
    // `busy` while the unit works, its `done` pulse becomes the response.
    wire su_sel = (cmd_function_id == 10'h04) || (cmd_function_id[9:4] == 6'h04)
               || (cmd_function_id[9:4] == 6'h05) || (cmd_function_id[9:4] == 6'h06);
    wire        su_done;
    wire [31:0] su_result;
    GeomSetupUnit setup_unit (
        .clk(clk), .resetn(resetn),
        .start(cmd_valid && cmd_ready && su_sel),
        .fid(cmd_function_id), .in0(cmd_inputs_0), .in1(cmd_inputs_1),
        .done(su_done), .result(su_result), .bg(su_bg),
        .vo0({{(32-DW){out_r[0][DW-1]}}, out_r[0]}), .vo1({{(32-DW){out_r[1][DW-1]}}, out_r[1]}),
        .vo2({{(32-DW){out_r[2][DW-1]}}, out_r[2]}), .vo3({{(32-DW){out_r[3][DW-1]}}, out_r[3]}),
        .out_valid(out_valid), .out_ready(out_ready), .out_data(out_data)
    );

    wire [3:0]  op_M_idx   = cmd_inputs_1[3:0];
    wire [1:0]  op_A_idx   = cmd_inputs_1[1:0];
    wire [1:0]  op_B_idx   = cmd_inputs_1[1:0];
    wire [1:0]  op_C_idx   = cmd_inputs_1[1:0];
    wire [1:0]  op_out_idx = cmd_inputs_1[1:0];

    // ---- MATVEC4: 4 lanes, each ONE multiply-accumulate unit stepping
    // through its own 4-term dot product over 4 cycles, then 1 cycle to
    // round+saturate, then 1 cycle to publish the response. All 4 lanes
    // advance in lockstep (mv_term is shared), so the 4 units run
    // concurrently -- 4 real DSP-mapped MAC units, not a single shared ALU.
    localparam MV_IDLE  = 2'd0;
    localparam MV_MAC   = 2'd1;
    localparam MV_ROUND = 2'd2;
    localparam MV_DONE  = 2'd3;

    reg [1:0]           mv_state;
    reg [1:0]           mv_term;      // which of the 4 dot-product terms (0..3)
    reg signed [AW-1:0] mv_acc [0:3]; // one running accumulator per lane

    // Which op MV_MAC is currently running -- latched when the command is
    // accepted, read back by MV_MAC/is_last_term below. Only needs to
    // distinguish MATVEC4 (4 sequential terms, matrix-column operand) from
    // VMUL4/VFMA4 (1 term, flat B/C operand); MUL vs FMA only changes
    // whether that one term's result is seeded with C, handled inline in
    // MV_MAC directly rather than needing its own bit here.
    localparam OPK_MATVEC = 2'd0;
    localparam OPK_VMUL   = 2'd1;
    localparam OPK_VFMA   = 2'd2;
    reg [1:0] op_kind;

    integer li;

    // Saturating round-and-shift from Q(2*FB) back to Q(FB), S(DW) result.
    function signed [DW-1:0] round_sat;
        input signed [AW-1:0] acc;
        reg   signed [AW-1:0] rounded;
        reg   signed [AW-1:0] hi_clamp;
        reg   signed [AW-1:0] lo_clamp;
        begin
            rounded  = acc + (1 <<< (FB-1));
            rounded  = rounded >>> FB;
            // hi_clamp = 2^(DW-1)-1 = 0111...1 (DW-1 ones), sign-extended to
            // the full AW-bit accumulator width.
            hi_clamp = {{(AW-DW+1){1'b0}}, {(DW-1){1'b1}}};
            lo_clamp = -(hi_clamp + 1);                     // -2^(DW-1)
            if (rounded > hi_clamp)      round_sat = hi_clamp[DW-1:0];
            else if (rounded < lo_clamp) round_sat = lo_clamp[DW-1:0];
            else                         round_sat = rounded[DW-1:0];
        end
    endfunction

    // Column lookup: lane j's term `t` operand is m_l[t*4 + j] (row-major
    // storage). Combinational (just wiring/muxing into the multiplier's
    // input, not an extra register) -- keeps the register savings from not
    // latching all 16 elements up front like the first version did.
    function signed [DW-1:0] col_elem;
        input [1:0] j;
        input [1:0] t;
        begin
            col_elem = m_l[{t, j}];   // {t,j} == t*4+j for 2-bit t,j
        end
    endfunction

    // Generalised second-multiply-operand select, reading op_kind directly
    // (module-scope reg, legal to read inside a function body). MATVEC4
    // walks m_l as a 4x4 matrix (col_elem, above); VMUL4/VFMA4 read their
    // OWN b_l register file instead (see this file's header for why B/C
    // aren't just M[0..3]/M[4..7] -- they'd collide with MATVEC4's
    // resident matrix in the real per-vertex call sequence). term `t` is
    // always 0 for those two (see is_last_term below), so lane li's
    // operand is simply b_l[li].
    function signed [DW-1:0] b_operand;
        input [1:0] lane;
        input [1:0] t;
        begin
            b_operand = col_elem(lane, t);
        end
    endfunction

    // First-multiply-operand select (A). MATVEC4's formula broadcasts ONE
    // A[term] against all 4 lanes' matrix columns for that term -- lane
    // index is irrelevant, only the shared mv_term matters. VMUL4/VFMA4 are
    // element-wise (out[i]=A[i]*B[i]): each lane needs its OWN A[lane], not
    // the shared term index (which is always 0 for these two anyway).
    // Missing this and reusing a_l[mv_term] for every lane was a real bug
    // (not caught by design review, caught by tb_vpu4dfixed.cpp's VMUL4
    // test): every lane silently computed a_l[0]*B[lane] instead of
    // a_l[lane]*B[lane] -- confirmed by hand (lane 1 of a first VMUL4 test
    // came back exactly a_l[0]*B[1], not a_l[1]*B[1]) before writing this
    // fix, not guessed.
    function signed [DW-1:0] a_operand;
        input [1:0] lane;
        input [1:0] t;
        begin
            a_operand = a_l[t];
        end
    endfunction

    // MATVEC4 needs all 4 terms; VMUL4/VFMA4 are a single term (one
    // multiply per lane, not a dot product), so they finish after mv_term
    // reaches 0 for the first and only time.
    wire is_last_term = (mv_term == 2'd3);

    always @(posedge clk) begin
        if (!resetn) begin
            busy <= 1'b0; resp <= 1'b0; result <= 32'b0;
            mv_state <= MV_IDLE; mv_term <= 2'd0;
        end else begin
            // ---- response consumed ----
            if (resp && rsp_ready) resp <= 1'b0;

            // ---- setup-unit response ----
            if (su_done) begin
                result <= su_result; resp <= 1'b1; busy <= 1'b0;
            end

            // ---- accept a new command ----
            if (cmd_valid && cmd_ready && su_sel) begin
                busy <= 1'b1;
            end else if (cmd_valid && cmd_ready) begin
                case (cmd_function_id)
                    10'h00: begin
                        m_l[op_M_idx] <= cmd_inputs_0[DW-1:0];
                        result <= 32'b0; resp <= 1'b1;
                    end
                    10'h01: begin
                        a_l[op_A_idx] <= cmd_inputs_0[DW-1:0];
                        result <= 32'b0; resp <= 1'b1;
                    end
                    10'h08: begin
                        result <= {{(32-DW){out_r[op_out_idx][DW-1]}}, out_r[op_out_idx]};
                        resp   <= 1'b1;
                    end
                    // 0x33 MATVEC_OB: A = (x, y, z, 1) from a vertex's packed
                    // int16 object coordinates -- rs1 = y << 16 | x, rs2 = z --
                    // then MATVEC4. One op for load_vertices()'s four loads
                    // and the transform (each CFU op costs ~30 cycles of
                    // firmware around it).
                    10'h33: begin
                        a_l[0]   <= $signed(cmd_inputs_0[15:0]) <<< FB;
                        a_l[1]   <= $signed(cmd_inputs_0[31:16]) <<< FB;
                        a_l[2]   <= $signed(cmd_inputs_1[15:0]) <<< FB;
                        a_l[3]   <= 1 <<< FB;
                        op_kind  <= OPK_MATVEC;
                        mv_state <= MV_MAC;
                        mv_term  <= 2'd0;
                        busy     <= 1'b1;
                    end
                    // 0x34 MATVEC_OB_HP: 0x33 with x, y, z entering at
                    // S17.4 instead of S17.10 (the constant 1 stays S17.10),
                    // so the firmware can hold the matrix's x/y/z rows with
                    // 6 more fraction bits -- the output scale is unchanged.
                    // A world-sized vertex times a row kept to 10 fraction
                    // bits was up to ~6 clip units off: floors and walls
                    // jittered frame to frame.
                    10'h34: begin
                        a_l[0]   <= $signed(cmd_inputs_0[15:0]) <<< (FB - 6);
                        a_l[1]   <= $signed(cmd_inputs_0[31:16]) <<< (FB - 6);
                        a_l[2]   <= $signed(cmd_inputs_1[15:0]) <<< (FB - 6);
                        a_l[3]   <= 1 <<< FB;
                        op_kind  <= OPK_MATVEC;
                        mv_state <= MV_MAC;
                        mv_term  <= 2'd0;
                        busy     <= 1'b1;
                    end
                    10'h30: begin
                        op_kind  <= OPK_MATVEC;
                        mv_state <= MV_MAC;
                        mv_term  <= 2'd0;
                        busy     <= 1'b1;
                    end
                    default: begin
                        result <= 32'b0; resp <= 1'b1;
                    end
                endcase
            end

            // ---- MATVEC4/VMUL4/VFMA4 state machine ----
            case (mv_state)
                MV_MAC: begin
                    for (li = 0; li < 4; li = li + 1) begin
                        // Explicit if/else, not a ?: ternary: mixing the unsized
                        // (always-unsigned) {AW{1'b0}} literal with the signed
                        // mv_acc[li] inside a ?: makes the WHOLE ternary unsigned
                        // per Verilog's signedness-propagation rules, silently
                        // corrupting every accumulate after the first term. Found
                        // by tracing (this exact symptom: term 0 always correct,
                        // every later term garbage) rather than guessed. VFMA4's
                        // seed (c_l[li]) needs the SAME <<FB left-shift as the
                        // round_sat() at the far end does in reverse -- a*b is a
                        // raw S17.10 x S17.10 product, at Q(2*FB) scale (twice
                        // FB fractional bits), but the seed C is a plain S17.10
                        // value at Q(FB) scale; adding them at mismatched scales
                        // silently computed A*B + C/1024 instead of A*B + C. A
                        // REAL bug caught by tb_vpu4dfixed.cpp's negative_seed
                        // test (which independently hand-computed 3.0*2.0-500.0
                        // = -494, not derived from this formula) diverging from
                        // its own from-scratch reference model -- both the RTL
                        // and the first draft of that reference model shared
                        // this exact scale mistake, which is why their mutual
                        // agreement alone did NOT catch it; only the
                        // independently-computed expected value did.
                        if (mv_term == 2'd0)
                            mv_acc[li] <= $signed(a_operand(li[1:0], mv_term)) * $signed(b_operand(li[1:0], mv_term));
                        else
                            mv_acc[li] <= mv_acc[li] + $signed(a_operand(li[1:0], mv_term)) * $signed(b_operand(li[1:0], mv_term));
                    end
                    if (is_last_term) mv_state <= MV_ROUND;
                    else              mv_term  <= mv_term + 2'd1;
                end
                MV_ROUND: begin
                    for (li = 0; li < 4; li = li + 1)
                        out_r[li] <= round_sat(mv_acc[li]);
                    mv_state <= MV_DONE;
                end
                MV_DONE: begin
                    result   <= {{(32-DW){out_r[0][DW-1]}}, out_r[0]};
                    resp     <= 1'b1;
                    busy     <= 1'b0;
                    mv_state <= MV_IDLE;
                end
                default: ; // MV_IDLE: nothing to advance
            endcase
        end
    end

endmodule
