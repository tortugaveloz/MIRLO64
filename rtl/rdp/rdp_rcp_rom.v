// The table of lang/c/rdp/rdp.c's rcp_init:
//   P[i] = round(2^22 / (64 + i)), D[i] = P[i] - P[i+1]. Do not edit.
module rdp_rcp_rom (
    input  wire [5:0]  idx,
    output reg  [16:0] p,
    output reg  [10:0] d
);
    always @(*) begin
        case (idx)
            6'd0: begin p = 17'd65536; d = 11'd1008; end
            6'd1: begin p = 17'd64528; d = 11'd978; end
            6'd2: begin p = 17'd63550; d = 11'd948; end
            6'd3: begin p = 17'd62602; d = 11'd921; end
            6'd4: begin p = 17'd61681; d = 11'd894; end
            6'd5: begin p = 17'd60787; d = 11'd868; end
            6'd6: begin p = 17'd59919; d = 11'd844; end
            6'd7: begin p = 17'd59075; d = 11'd821; end
            6'd8: begin p = 17'd58254; d = 11'd798; end
            6'd9: begin p = 17'd57456; d = 11'd776; end
            6'd10: begin p = 17'd56680; d = 11'd756; end
            6'd11: begin p = 17'd55924; d = 11'd736; end
            6'd12: begin p = 17'd55188; d = 11'd717; end
            6'd13: begin p = 17'd54471; d = 11'd698; end
            6'd14: begin p = 17'd53773; d = 11'd681; end
            6'd15: begin p = 17'd53092; d = 11'd663; end
            6'd16: begin p = 17'd52429; d = 11'd647; end
            6'd17: begin p = 17'd51782; d = 11'd632; end
            6'd18: begin p = 17'd51150; d = 11'd616; end
            6'd19: begin p = 17'd50534; d = 11'd602; end
            6'd20: begin p = 17'd49932; d = 11'd587; end
            6'd21: begin p = 17'd49345; d = 11'd574; end
            6'd22: begin p = 17'd48771; d = 11'd561; end
            6'd23: begin p = 17'd48210; d = 11'd547; end
            6'd24: begin p = 17'd47663; d = 11'd536; end
            6'd25: begin p = 17'd47127; d = 11'd524; end
            6'd26: begin p = 17'd46603; d = 11'd512; end
            6'd27: begin p = 17'd46091; d = 11'd501; end
            6'd28: begin p = 17'd45590; d = 11'd490; end
            6'd29: begin p = 17'd45100; d = 11'd480; end
            6'd30: begin p = 17'd44620; d = 11'd469; end
            6'd31: begin p = 17'd44151; d = 11'd460; end
            6'd32: begin p = 17'd43691; d = 11'd451; end
            6'd33: begin p = 17'd43240; d = 11'd441; end
            6'd34: begin p = 17'd42799; d = 11'd432; end
            6'd35: begin p = 17'd42367; d = 11'd424; end
            6'd36: begin p = 17'd41943; d = 11'd415; end
            6'd37: begin p = 17'd41528; d = 11'd407; end
            6'd38: begin p = 17'd41121; d = 11'd400; end
            6'd39: begin p = 17'd40721; d = 11'd391; end
            6'd40: begin p = 17'd40330; d = 11'd384; end
            6'd41: begin p = 17'd39946; d = 11'd377; end
            6'd42: begin p = 17'd39569; d = 11'd370; end
            6'd43: begin p = 17'd39199; d = 11'd363; end
            6'd44: begin p = 17'd38836; d = 11'd356; end
            6'd45: begin p = 17'd38480; d = 11'd350; end
            6'd46: begin p = 17'd38130; d = 11'd343; end
            6'd47: begin p = 17'd37787; d = 11'd338; end
            6'd48: begin p = 17'd37449; d = 11'd331; end
            6'd49: begin p = 17'd37118; d = 11'd326; end
            6'd50: begin p = 17'd36792; d = 11'd320; end
            6'd51: begin p = 17'd36472; d = 11'd314; end
            6'd52: begin p = 17'd36158; d = 11'd309; end
            6'd53: begin p = 17'd35849; d = 11'd304; end
            6'd54: begin p = 17'd35545; d = 11'd299; end
            6'd55: begin p = 17'd35246; d = 11'd293; end
            6'd56: begin p = 17'd34953; d = 11'd289; end
            6'd57: begin p = 17'd34664; d = 11'd284; end
            6'd58: begin p = 17'd34380; d = 11'd280; end
            6'd59: begin p = 17'd34100; d = 11'd275; end
            6'd60: begin p = 17'd33825; d = 11'd271; end
            6'd61: begin p = 17'd33554; d = 11'd266; end
            6'd62: begin p = 17'd33288; d = 11'd262; end
            6'd63: begin p = 17'd33026; d = 11'd258; end
            default: begin p = 17'd0; d = 11'd0; end
        endcase
    end
endmodule
