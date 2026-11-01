// Software License Agreement

// The software supplied herewith by Analogue Enterprises Limited (the "Company”),
// the Analogue Pocket Framework (“APF”), is provided and licensed to you, the
// Company's customer, solely for use in designing, testing and creating
// applications for use with Company's Products or Services.  The software is
// owned by the Company and/or its licensors, and is protected under applicable
// laws, including, but not limited to, U.S. copyright law. All rights are
// reserved. By using the APF code you are agreeing to the terms of the End User
// License Agreement (“EULA”) located at [https://www.analogue.link/pocket-eula]
// and incorporated herein by reference. To the extent any use of the APF requires
// application of the MIT License or the GNU General Public License and terms of
// this APF Software License Agreement and EULA are inconsistent with such license,
// the applicable terms of the MIT License or the GNU General Public License, as
// applicable, will prevail.

// THE SOFTWARE IS PROVIDED "AS-IS" AND WE EXPRESSLY DISCLAIM ANY IMPLIED
// WARRANTIES TO THE FULLEST EXTENT PROVIDED BY LAW, INCLUDING BUT NOT LIMITED TO,
// ANY WARRANTY OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE OR
// NON-INFRINGEMENT. TO THE EXTENT APPLICABLE LAWS PROHIBIT TERMS OF USE FROM
// DISCLAIMING ANY IMPLIED WARRANTY, SUCH IMPLIED WARRANTY SHALL BE LIMITED TO THE
// MINIMUM WARRANTY PERIOD REQUIRED BY LAW, AND IF NO SUCH PERIOD IS REQUIRED,
// THEN THIRTY (30) DAYS FROM FIRST USE OF THE SOFTWARE. WE CANNOT GUARANTEE AND
// DO NOT PROMISE ANY SPECIFIC RESULTS FROM USE OF THE SOFTWARE. WITHOUT LIMITING
// THE FOREGOING, WE DO NOT WARRANT THAT THE SOFTWARE WILL BE UNINTERRUPTED OR
// ERROR-FREE.  IN NO EVENT WILL WE BE LIABLE TO YOU OR ANY OTHER PERSON FOR ANY
// INDIRECT, CONSEQUENTIAL, EXEMPLARY, INCIDENTAL, SPECIAL OR PUNITIVE DAMAGES,
// INCLUDING BUT NOT LIMITED TO, LOST PROFITS ARISING OUT OF YOUR USE, OR
// INABILITY TO USE, THE SOFTWARE, EVEN IF WE HAVE BEEN ADVISED OF THE POSSIBILITY
// OF SUCH DAMAGES. UNDER NO CIRCUMSTANCES SHALL OUR LIABILITY TO YOU FOR ANY
// CLAIM OR CAUSE OF ACTION WHATSOEVER, AND REGARDLESS OF THE FORM OF THE ACTION,
// WHETHER ARISING IN CONTRACT, TORT OR OTHERWISE, EXCEED THE AMOUNT PAID BY YOU
// TO US, IF ANY, DURING THE 90 DAY PERIOD IMMEDIATELY PRECEDING THE DATE ON WHICH
// YOU FIRST ASSERT ANY SUCH CLAIM. THE FOREGOING LIMITATIONS SHALL APPLY TO THE
// FULLEST EXTENT PERMITTED BY APPLICABLE LAW.
//
// pad controller
// 2020-08-17 Analogue
//

module io_pad_controller (

input   wire            clk,
input   wire            reset_n,

inout   reg             pad_1wire,

// the controller words, as the core reads them: written one word at a time into a RAM of 16 words (the core's, in the
// core's own clock; mirlo_regs.sv), at the address of the register's place in the map: 0..3 CONT1..4_KEY, 4..7 CONT1..4_JOY,
// 8..11 CONT1..4_TRIG (the trigger words' upper half is 0). pad_we is a pulse of this clock
output  reg             pad_we,
output  reg     [3:0]   pad_wa,
output  reg     [31:0]  pad_wd,

output  reg             rx_timed_out
);

    wire        reset_n_s;
synch_3 s00(reset_n, reset_n_s, clk);

    wire        pad_1wire_s, pad_1wire_r, pad_1wire_f;
synch_3 s01(pad_1wire, pad_1wire_s, clk, pad_1wire_r, pad_1wire_f);


//
// protocol fsm
//

    reg [20:0]  rx_timeout; // ~28ms

    reg         auto_poll_queue;

    reg [18:0]  heartbeat_cnt; // 7ms; its low 16 bits are the poll timer, 882us (both counted from the start, each clock)
    reg         heartbeat_queue;


    localparam  ST_RESET        = 'd0;
    localparam  ST_IDLE         = 'd1;
    localparam  ST_RX_BUTTON_1  = 'd2;
    localparam  ST_RX_BUTTON_2  = 'd3;
    localparam  ST_TX_SCALER    = 'd4;
    localparam  ST_END_TX       = 'd5;

    reg [3:0]   state;
    reg [3:0]   cnt;

always @(posedge clk) begin
    tx_word_start <= 0;
    pad_we <= 0;

    heartbeat_cnt <= heartbeat_cnt + 1'b1;

    // increment rx timeout, override and reset when idle below
    rx_timeout <= rx_timeout + 1'b1;

    case(state)
    ST_RESET: begin
        reset_tr_n <= 0;
        rx_timed_out <= 0;

        if(&rx_timeout[19:0]) begin
            state <= ST_IDLE;
        end
    end
    ST_IDLE: begin
        // idle state
        reset_tr_n <= 1;
        rx_timeout <= 0;
        cnt <= 0;
        if(auto_poll_queue) begin
            auto_poll_queue <= 0;

            tx_word_start <= 1;
            tx_hb <= 0;

            state <= ST_RX_BUTTON_1;
        end else if(heartbeat_queue) begin
            heartbeat_queue <= 0;

            tx_word_start <= 1;
            tx_hb <= 1;

            state <= ST_END_TX;
        end
    end
    // receive button words
    ST_RX_BUTTON_1: begin
        if(tx_word_done) begin
            state <= ST_RX_BUTTON_2;
        end
    end
    ST_RX_BUTTON_2: begin
        if(rx_word_done) begin
            cnt <= cnt + 1'b1;
            // (rx_word_shift is the word: it keeps it until the next one's first bit, some 25 clocks on)
            pad_we <= cnt < 4'd12;
            pad_wd <= (cnt == 4'd2 || cnt == 4'd5 || cnt == 4'd8 || cnt == 4'd11) ? {16'd0, rx_word_shift[15:0]} : rx_word_shift;
            case(cnt)
            0:  pad_wa <= 4'd0;     // cont1_key
            1:  pad_wa <= 4'd4;     // cont1_joy
            2:  pad_wa <= 4'd8;     // cont1_trig

            3:  pad_wa <= 4'd1;
            4:  pad_wa <= 4'd5;
            5:  pad_wa <= 4'd9;

            6:  pad_wa <= 4'd2;
            7:  pad_wa <= 4'd6;
            8:  pad_wa <= 4'd10;

            9:  pad_wa <= 4'd3;
            10: pad_wa <= 4'd7;
            11: begin
                pad_wa <= 4'd11;    // cont4_trig
                state <= ST_IDLE;
            end
            endcase
        end
    end
    // do nothing
    ST_END_TX: begin
        // done sending, idle again
        if(tx_word_done) begin
            state <= ST_IDLE;
        end
    end
    endcase


    if(&heartbeat_cnt[15:0]) begin
        auto_poll_queue <= 1;
    end
    if(&heartbeat_cnt) begin
        heartbeat_queue <= 1;
    end

    if(&rx_timeout) begin
        // reset protocol FSM which will also reset t/r engine
        rx_timed_out <= 1;
        rx_timeout <= 0;
        state <= ST_RESET;
    end

    if(~reset_n_s) begin
        state <= ST_RESET;
    end
end





//
// word receive/transmit engine
//
    reg         reset_tr_n;
    localparam  BITLEN = 60;

    reg         rx_word_done;
    reg [31:0]  rx_word_shift;

    reg         tx_word_start, tx_word_start_1;
    reg         tx_word_done;
    reg         tx_hb;      // the word sent: the heartbeat's, else the poll's
    wire [31:0] tx_word = tx_hb ? 32'h4AFE0000 : 32'h4A10000C;

    reg [7:0]   tr_cnt;
    reg [5:0]   tr_bit;

    localparam  TR_IDLE         = 'd1;
    localparam  TR_TX_START     = 'd2;
    localparam  TR_TX_CONTINUE  = 'd3;
    localparam  TR_TX_DONE      = 'd4;
    localparam  TR_RX_START     = 'd5;
    localparam  TR_RX_WAITEDGE  = 'd6;
    localparam  TR_RX_DONE      = 'd7;

    reg [3:0]   tr_state;

always @(posedge clk) begin

    rx_word_done <= 0;
    tx_word_done <= 0;

    tx_word_start_1 <= tx_word_start;

    case(tr_state)
    TR_IDLE: begin
        tr_bit <= 0;
        tr_cnt <= 0;

        pad_1wire <= 1'bZ;

        if(tx_word_start & ~tx_word_start_1) begin
            // transmit word
            tr_state <= TR_TX_START;
        end

        if(pad_1wire_f) begin
            // receive word
            tr_state <= TR_RX_START;
        end
    end

    // transmit 32bit
    TR_TX_START: begin
        // insert delay
        tr_cnt <= tr_cnt + 1'b1;
        if(&tr_cnt) begin
            // drive from tristate(high) to explicitly high to prevent glitching
            pad_1wire <= 1'b1;
            tr_state <= TR_TX_CONTINUE;
        end
    end
    TR_TX_CONTINUE: begin
        tr_cnt <= tr_cnt + 1'b1;
        case(tr_cnt)
        0: begin
            pad_1wire <= 1'b0;
        end
        (BITLEN/3): begin
            pad_1wire <= tx_word[5'd31 - tr_bit[4:0]];
        end
        (BITLEN*2/3): begin
            pad_1wire <= 1'b1;
        end
        (BITLEN-1): begin
            tr_cnt <= 0;
            tr_bit <= tr_bit + 1'b1;
            if(tr_bit == 31) begin
                tr_state <= TR_TX_DONE;
            end
        end
        endcase
    end
    TR_TX_DONE: begin
        tx_word_done <= 1;
        tr_state <= TR_IDLE;
    end

    // receive 32bit
    TR_RX_START: begin
        tr_cnt <= tr_cnt + 1'b1;
        case(tr_cnt)
        (BITLEN/2-4): begin
            rx_word_shift <= {rx_word_shift[30:0], pad_1wire_s};
        end
        (BITLEN*5/6): begin
            tr_cnt <= 0;

            // wait for next falling edge
            tr_state <= TR_RX_WAITEDGE;
            tr_bit <= tr_bit + 1'b1;
            if(tr_bit == 31) begin
                // if this is bit32, don't wait and finish
                tr_state <= TR_RX_DONE;
            end
        end
        endcase
    end
    TR_RX_WAITEDGE: begin
        if(pad_1wire_f) begin
            tr_state <= TR_RX_START;
        end
    end
    TR_RX_DONE: begin
        rx_word_done <= 1;
        tr_state <= TR_IDLE;
    end

    default: begin
        tr_state <= TR_IDLE;
    end
    endcase

    if(~reset_n_s | ~reset_tr_n) tr_state <= TR_IDLE;
end

endmodule
