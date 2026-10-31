// Mirlo-N64's FPU, double precision (the CPU's FPD64 level): add.d, sub.d, mul.d, abs.d, neg.d and c.cond.d on the 64 bits
// of a register pair, exactly (sim/n64/fpd64.h is the model of it, and the host's own doubles check the model).
//
// The rules are the single-precision path's (fpu_n64.sv): an input below the normals is a zero of its sign (FCR31.FS), arithmetic
// rounds to nearest even whatever FCR31's RM is, with an unbounded exponent, and then a result below the normals is a zero of
// its sign and one above them infinity; a NaN result is 0x7FF7FFFF_FFFFFFFF; no exceptions, no cause or flag bits.
//
// An iterative datapath, a few small pieces used over and over rather than an array:
//  - one 57-bit adder (`sum`): x + y + cin, x the accumulator `acc` (inverted for a subtraction, or zero for a load), y one of the
//    operands' significands, the DSP's product or the rounding increment. The significands are 56 bits, {1, f, 000}: three
//    bits below the double's 53 for the guard, round and sticky bits of a round to nearest even.
//  - one shifter on `acc`: right by 16, 4 or 1 with the shifted-out bits jammed into bit 0 (the alignment of the smaller
//    operand, the carry of a sum) and left by 16, 4 or 1 (the normalisation after a cancellation). A cycle per step.
//  - one 26 x 26 multiplier (a 27 x 27 DSP block, with registers at its inputs and output): the 52-bit fractions' product in
//    four pieces of 26 bits, the pieces accumulated in `acc` (the hidden bits' terms are two more additions):
//    A * B = (2^52 + fa)(2^52 + fb) = 2^104 + 2^52 (fa + fb) + fa * fb.
//
// add / sub: the smaller operand (by exponent) is loaded into acc and shifted right by the exponent difference (at most 60: all
// of it is sticky beyond 56), the larger added to it or taken from it (both zero or equal exponents: the sum may be negative,
// then it is negated and the sign changes), normalised (one right shift on a carry, or left until bit 55 is set), rounded.
// A zero or a denormal operand is a significand of 0 with exponent 0: the same path gives the other operand, exactly.
// mul: nine states for the product, then the same normalise and round. Cycles from the instruction in M (the core waits on
// `busy`): add / sub 9 + the alignment and normalisation steps (at least 1 each), mul 14, abs / neg 4, compare 4 or 6.
//
// acc, 57 bits: [56] carry, [55] the hidden bit of a normal number, [54:3] the fraction, [2] guard, [1] round, [0] sticky.
`default_nettype none

module fpu_d64 (
    input  wire         clk,
    input  wire         rst,
    input  wire         go,                // M's instruction is one of ours (add.d, sub.d, mul.d, abs.d, neg.d, c.cond.d)
    input  wire         live,              // it is live in M (a flushed one is dropped)
    input  wire         take,              // the core takes the result this cycle
    input  wire  [5:0]  fn,
    input  wire  [63:0] a,                 // fs's pair, {odd, even}
    input  wire  [63:0] b,                 // ft's pair
    output logic        busy,
    output logic [63:0] res,
    output logic        c_un, c_eq, c_lt   // a compare's result
);

typedef enum logic [4:0] { S_IDLE, S_CLS, S_DEC, S_ALN, S_SUM, S_NEG, S_NRM, S_RND, S_PACK, S_DONE,
                           S_M2, S_M3, S_M4, S_M5, S_M6, S_M7, S_M8, S_CMP1, S_CMP2, S_CMPF } st_t;
st_t st;

wire f_mul = fn == 6'h02, f_sub = fn == 6'h01, f_ab = fn == 6'h05 || fn == 6'h07, f_cmp = fn[5:4] == 2'b11;

logic        sa, sb;
logic [10:0] ea, eb;
logic [55:0] ma, mb;                                // {hidden, fraction, 000}; 0 for a zero or a denormal
logic        nx, ny, ix, iy, zx, zy;
logic [12:0] E;                                     // the result's biased exponent (before the carry), signed
logic [5:0]  D;                                     // add: the shift still to do
logic        sw;                                    // ea < eb
logic        ee;                                    // ea == eb
logic        rs, seff;                              // the result's sign; the operands' signs differ (after sub's)
logic [56:0] acc;
logic        g, stk;                                // mul: the guard bit and the sticky below acc's bit 3
logic [25:0] mua, mub;
logic [51:0] mp;
logic [63:0] r;

wire [25:0] al = ma[28:3], ah = ma[54:29], bl = mb[28:3], bh = mb[54:29];
always_ff @(posedge clk) begin
    case (st)
    S_CLS:   begin mua <= al; mub <= bl; end
    S_DEC:   begin mua <= ah; mub <= bl; end
    S_M2:    begin mua <= al; mub <= bh; end
    S_M3:    begin mua <= ah; mub <= bh; end
    default: ;
    endcase
    mp <= 52'(mua * mub);                           // (a 26 x 26 unsigned product: one DSP block, its registers the ones around it)
end

// ---------------------------------------------------------------- the adder
typedef enum logic [2:0] { Y_0, Y_MA, Y_MB, Y_PR, Y_PRS, Y_K8 } y_t;
y_t          ysel;
logic        xz, xinv, cin, inc, mbm;
logic [56:0] ys;
always_comb begin
    case (ysel)
    Y_MA:    ys = {1'b0, ma};
    Y_MB:    ys = {1'b0, mb[55] & !mbm, mb[54:3], g, 1'b0, stk};
    Y_PR:    ys = {2'b00, mp, 3'b000};
    Y_PRS:   ys = {28'd0, mp[51:26], 3'b000};
    Y_K8:    ys = {53'd0, inc, 3'b000};
    default: ys = 57'd0;
    endcase
end
wire [56:0] xs = xz ? 57'd0 : acc ^ {57{xinv}};
wire [56:0] sum = xs + ys + {56'd0, cin};

// the adder's operands by state
always_comb begin
    ysel = Y_0; xz = 0; xinv = 0; cin = 0; inc = 0; mbm = 0;
    case (st)
    S_DEC:  begin xz = 1; ysel = sw ? Y_MA : Y_MB; end             // acc: the smaller operand
    S_SUM:  begin ysel = sw ? Y_MB : Y_MA; xinv = seff; cin = seff; end
    S_NEG:  begin xinv = 1; cin = 1; end
    S_RND:  begin ysel = Y_K8; inc = acc[2] & (acc[1] | acc[0] | acc[3]); end
    S_M2:   begin xz = 1; ysel = Y_PRS; end
    S_M3, S_M4, S_M6: ysel = Y_PR;
    S_M7:   ysel = Y_MA;
    S_M8:   begin ysel = Y_MB; mbm = 1; end
    S_CMP1: begin xz = 1; ysel = Y_MB; end
    S_CMP2: begin ysel = Y_MA; xinv = 1; cin = 1; end
    default: ;
    endcase
end

// the shifter: right by n with the shifted-out bits jammed into bit 0
function automatic logic [56:0] rj(input logic [56:0] v, input int n);
    logic [56:0] o;
    o = v >> n;
    o[0] = o[0] | (|(v & ~(57'h1FFFFFFFFFFFFFF << n)));
    return o;
endfunction
wire a_z16 = acc[55:40] == 16'd0, a_z4 = acc[55:52] == 4'd0;
wire acc_z = acc == 57'd0;

// the special cases and the constants
localparam logic [63:0] QNAN = 64'h7FF7FFFF_FFFFFFFF;
wire sp_nan   = nx || ny || (f_mul ? ((ix && zy) || (zx && iy)) : (ix && iy && seff));
wire sp_inf   = ix || iy;
wire sp_zero  = f_mul && (zx || zy);
wire sp_inf_s = f_mul ? sa ^ sb : ix ? sa : sb ^ f_sub;
wire [12:0] Ef = E + {12'd0, acc[56]};                // (the carry of the rounding)

always_ff @(posedge clk) begin
    if (rst || !live) st <= S_IDLE;
    else case (st)
    S_IDLE: if (go) begin
        sa <= a[63]; ea <= a[62:52]; ma <= {a[62:52] != 11'd0, a[62:52] != 11'd0 ? a[51:0] : 52'd0, 3'b000};
        sb <= b[63]; eb <= b[62:52]; mb <= {b[62:52] != 11'd0, b[62:52] != 11'd0 ? b[51:0] : 52'd0, 3'b000};
        st <= S_CLS;
    end
    S_CLS: begin
        logic [11:0] dd, dm;
        nx <= ea == 11'h7FF && ma[54:3] != 52'd0; ix <= ea == 11'h7FF && ma[54:3] == 52'd0; zx <= ea == 11'd0;
        ny <= eb == 11'h7FF && mb[54:3] != 52'd0; iy <= eb == 11'h7FF && mb[54:3] == 52'd0; zy <= eb == 11'd0;
        dd = {1'b0, ea} - {1'b0, eb};
        sw <= dd[11]; ee <= dd == 12'd0;
        dm = dd[11] ? -dd : dd;
        D <= dm >= 12'd60 ? 6'd60 : dm[5:0];
        E <= f_mul ? 13'({2'b00, ea} + {2'b00, eb}) - 13'd1023 : dd[11] ? {2'b00, eb} : {2'b00, ea};
        seff <= !f_mul && (sa ^ sb ^ f_sub);
        rs <= f_mul ? sa ^ sb : dd[11] ? sb ^ f_sub : sa;
        g <= 0; stk <= 0;
        st <= S_DEC;
    end
    S_DEC: begin
        // the special cases are the result at once; else the smaller operand is in acc (add), or on to the product
        if (f_ab) begin
            r <= nx ? QNAN : {fn[1] ? !sa : 1'b0, ea, ma[54:3]};
            st <= S_DONE;
        end else if (f_cmp) begin                      // (a zero or a denormal compares as 0, a NaN is unordered)
            c_un <= nx || ny; c_eq <= 0; c_lt <= 0;
            st <= S_DONE;
            if (nx || ny) ;
            else if (zx && zy) c_eq <= 1;
            else if (sa != sb) c_lt <= sa;
            else if (!ee) c_lt <= sa ? !sw : sw;
            else st <= S_CMP1;
        end
        else if (sp_nan) begin r <= QNAN; st <= S_DONE; end
        else if (sp_inf) begin r <= {sp_inf_s, 63'h7FF0_0000_0000_0000}; st <= S_DONE; end
        else if (sp_zero) begin r <= {sa ^ sb, 63'd0}; st <= S_DONE; end
        else st <= f_mul ? S_M2 : S_ALN;
        acc <= sum;                                   // (add: the smaller operand; else not read before it is loaded again)
    end
    // alignment: the smaller operand, in acc, shifted right by the exponent difference
    S_ALN: begin
        if (D >= 6'd16) begin D <= D - 6'd16; acc <= rj(acc, 16); end
        else if (D >= 6'd4) begin D <= D - 6'd4; acc <= rj(acc, 4); end
        else if (D != 6'd0) begin D <= D - 6'd1; acc <= rj(acc, 1); end
        else st <= S_SUM;
    end
    S_SUM: begin acc <= sum; st <= S_NRM; end
    S_NEG: begin acc <= sum; rs <= !rs; st <= S_NRM; end
    S_NRM:
        if (seff && acc[56]) st <= S_NEG;                          // a negative difference (the operands' exponents were equal)
        else if (acc[56]) begin acc <= rj(acc, 1); E <= E + 13'd1; st <= S_RND; end
        else if (acc_z) begin r <= {seff ? 1'b0 : rs, 63'd0}; st <= S_DONE; end
        else if (!acc[55]) begin
            if (a_z16) begin acc <= acc << 16; E <= E - 13'd16; end
            else if (a_z4) begin acc <= acc << 4; E <= E - 13'd4; end
            else begin acc <= acc << 1; E <= E - 13'd1; end
        end else st <= S_RND;
    S_RND: begin acc <= sum; st <= S_PACK; end
    S_PACK: begin
        r <= Ef >= 13'd2047 && !Ef[12] ? {rs, 63'h7FF0_0000_0000_0000} : (Ef[12] || Ef == 13'd0) ? {rs, 63'd0} : {rs, Ef[10:0], acc[54:3]};
        st <= S_DONE;
    end
    // the product
    S_M2: begin acc <= sum; stk <= |mp[25:0]; st <= S_M3; end
    S_M3: begin acc <= sum; st <= S_M4; end
    S_M4: begin acc <= sum; st <= S_M5; end
    S_M5: begin acc <= {26'd0, acc[56:29], 3'b000}; g <= acc[28]; stk <= stk | (|acc[27:3]); st <= S_M6; end
    S_M6: begin acc <= sum; st <= S_M7; end
    S_M7: begin acc <= sum; st <= S_M8; end
    S_M8: begin acc <= sum; st <= S_NRM; end
    // an exact compare of the significands when the exponents are equal: ma - mb
    S_CMP1: begin acc <= sum; st <= S_CMP2; end
    S_CMP2: begin acc <= sum; st <= S_CMPF; end
    S_CMPF: begin c_eq <= acc_z; c_lt <= sa ? (!acc[56] && !acc_z) : acc[56]; st <= S_DONE; end
    S_DONE: if (take) st <= S_IDLE;
    default: st <= S_IDLE;
    endcase
end

assign busy = go && st != S_DONE;
assign res = r;

endmodule
`default_nettype wire
