// Native-pixel framing with APF enlargement; bilinear fallback for sizes that
// cannot be represented by the eight static presets. Original bypasses RAM.
// 0=Original, 1=Stretch, 2=Fit, 3=Overscale, 4=Hybrid; existing menu values are unchanged.
`default_nettype none
module pocket_video (
    input wire clk_vid, rst_vid, clk_sys, rst_sys,
    input wire [2:0] screen_mode,
    input wire docked,
    // Cropped, clipped picture bounds, synchronous to clk_vid, from scanout.
    input wire [9:0] source_x, source_y, source_w, source_h,
    input wire [23:0] rgb,
    input wire de, hs, vs, skip,
    output wire [23:0] video_rgb,
    output logic video_de = 0, video_hs = 0, video_vs = 0, video_skip = 0,
    output wire m_valid, m_we, m_last,
    output wire [21:0] m_addr,
    output wire [31:0] m_wdata,
    input wire m_ready, m_rvalid, m_idle, memory_ready,
    input wire [31:0] m_rdata
);
    (* async_reg = "true" *) logic [3:0] settings_meta, settings_sync;
    logic [3:0] announced, active;
    wire [2:0] mode = active[2:0];
    // Slot dimensions must match video.json. Fit retains every native pixel;
    // cover chooses a centred, exact 10:9 rectangle without pixel repetition.
    function automatic [2:0] choose_slot(input [3:0] setting, input [9:0] w, h);
        begin
            choose_slot = 7;
            if (setting[2:0] == 0) choose_slot = 0;
            else if (setting[2:0] == 2 && (w >= 638 && w <= 640) && h <= 240 && h != 0) choose_slot = 0;
            else if (setting[2:0] == 1 && (w >= 638 && w <= 640)) begin
                case (h)
                    240: choose_slot = 1;
                    224: choose_slot = 2;
                    216: choose_slot = 3;
                    default: choose_slot = 7;
                endcase
            end else if (setting[2:0] == 3) begin
                if (setting[3]) begin
                    if (w == 640 && h == 240) choose_slot = 0;
                end else begin
                    if (h == 240 && w >= 520) choose_slot = 4;
                    else if ((h == 224 || h == 216) && w >= 480) choose_slot = 5;
                    else if (h == 213 && w >= 460) choose_slot = 6;
                end
            end
        end
    endfunction
    logic [2:0] announced_slot, active_slot;
    wire filtered = active_slot == 7;
    // Precompute preset dimensions and their counter/multiplier offsets.
    // A registered ROM settles during vertical blanking after a slot change.
    (* ramstyle = "M10K" *) logic [37:0] geometry_rom [0:7];
    initial begin
        geometry_rom[0] = {10'd740, 9'd244, 10'd639, 9'd239};
        geometry_rom[1] = {10'd740, 9'd244, 10'd639, 9'd239};
        geometry_rom[2] = {10'd740, 9'd228, 10'd639, 9'd223};
        geometry_rom[3] = {10'd740, 9'd220, 10'd639, 9'd215};
        geometry_rom[4] = {10'd620, 9'd238, 10'd519, 9'd233};
        geometry_rom[5] = {10'd580, 9'd220, 10'd479, 9'd215};
        geometry_rom[6] = {10'd560, 9'd211, 10'd459, 9'd206};
        geometry_rom[7] = {10'd500, 9'd364, 10'd399, 9'd359};
    end
    logic [37:0] geometry;
    always_ff @(posedge clk_vid) geometry <= geometry_rom[active_slot];
    wire [9:0] pixel_end = geometry[37:28];
    wire [8:0] line_end = geometry[27:19];
    wire [9:0] span_columns = geometry[18:9];
    wire [8:0] span_rows = geometry[8:0];
    logic vs_d, seen_pixel;
    wire frame_start = vs && !vs_d;
    wire capture_bank, capture_good;
    wire [1:0] completed;
    // Completed bounds remain here throughout the coordinate-table build.
    // Upstream VS precedes the next capture by over 18,000 video clocks;
    // both tables finish within 1,100. No second bounds register is needed.
    logic [39:0] bounds;
    logic bounds_good;
    wire [39:0] source_bounds = {source_x, source_y, source_w, source_h};
    logic frame_valid, frame_bank;
    wire [9:0] left = bounds[39:30], top = bounds[29:20];
    wire [9:0] width = bounds[19:10], height = bounds[9:0];
    // Signed Q12 source-index coordinates, mapping destination pixel centres.
    // Uniform modes share one step on both axes, including fractional crops.
    // Ratios never exceed 511/300; 14 signed bits hold their Q12 values.
    // Coordinates use signed 11.12 to retain the full 640x240 VI field.
    logic signed [13:0] step_x, step_y;
    // Fixed denominators need only small ROMs. This spends five M10K
    // blocks instead of a divider and its blanking-time control/datapath.
    (* ramstyle = "M10K" *) logic [25:0] horizontal_ratios [0:1023];
    (* ramstyle = "M10K" *) logic [25:0] vertical_ratios [0:511];
    initial begin
        for (int n = 0; n < 1024; n++)
            horizontal_ratios[n] = {13'(n * 4096 / 400), 13'(n * 4096 / 960)};
        for (int n = 0; n < 512; n++) begin
            vertical_ratios[n] = {13'(n * 4096 / 300), 13'(n * 4096 / 360)};
        end
    end
    logic [25:0] horizontal_ratio, vertical_ratio;
    always_ff @(posedge clk_vid) begin
        horizontal_ratio <= horizontal_ratios[width];
        vertical_ratio <= vertical_ratios[height[8:0]];
    end
    wire signed [13:0] ratio_x = {1'b0, horizontal_ratio[25:13]};
    // A VI sample is half as wide as it is tall. Compare steps in VI-X
    // units; convert back to Y units after choosing contain/cover.
    wire signed [13:0] ratio_y = {active[3] ? vertical_ratio[25:13] : vertical_ratio[12:0], 1'b0};
    wire uniform_from_x = mode == 2 ? ratio_x > ratio_y : ratio_x < ratio_y;
    wire signed [13:0] uniform_step = uniform_from_x ? ratio_x : ratio_y;
    wire signed [13:0] stretch_y = {1'b0, vertical_ratio[12:0]};
    wire signed [13:0] uniform_y = uniform_from_x ?
        (active[3] ? $signed({1'b0, horizontal_ratio[12:0]}) : ratio_x >>> 1) : stretch_y;
    // Halfway between Stretch and aspect-correct cover in source coordinates.
    wire signed [14:0] hybrid_sum_x = {1'b0, ratio_x} + {1'b0, uniform_step};
    wire signed [14:0] hybrid_sum_y = {1'b0, stretch_y} + {1'b0, uniform_y};
    logic [9:0] column;
    logic [8:0] row;
    logic running;
    wire picture_line = row >= 4 && row < line_end;
    wire picture_pixel = running && picture_line && column >= 100 && column < pixel_end;
    logic request, request_frame, request_cache;
    logic [7:0] request_y;
    wire acknowledge;
    logic expected, requested_line, line_ready;
    logic [23:0] rgb_delay;
    logic out_de_d;
    wire [23:0] pixel00, pixel10, pixel01, pixel11;
    // Compute both axes during vertical blanking with one shared datapath.
    // Each entry is {inside, fractional weight, clamped source index}.
    (* ramstyle = "M10K, no_rw_check" *) logic [18:0] x_lookup [0:1023];
    (* ramstyle = "M10K, no_rw_check" *) logic [18:0] y_lookup [0:511];
    logic build_active, build_axis;
    logic [9:0] build_index;
    wire [9:0] build_origin = build_axis ? top : left;
    wire [9:0] build_size = build_axis ? height : width;
    wire signed [13:0] build_step = build_axis ? step_y : step_x;
    wire [9:0] build_span = build_axis ? {1'b0, span_rows} : span_columns;
    // Compute each pixel centre directly in one DSP. This replaces a wide
    // start-value mux and accumulator with a small integer-centre adder.
    // Ceil(product/2) matches centre - floor(step*span/2) + step*index.
    wire [9:0] build_last = build_size - 10'd1;
    wire signed [11:0] offset = $signed({1'b0, build_index, 1'b0}) - $signed({2'b0, build_span});
    wire signed [25:0] product = build_step * offset;
    wire signed [22:0] centred = (23'(build_last) << 11) + 23'((product + 26'sd1) >>> 1);
    wire signed [22:0] build_sample = !filtered && !build_axis && mode == 1 ?
        {1'b0, build_index, 12'd0} : filtered ? centred : {centred[22:12], 12'd0};
    wire negative = build_sample[22];
    wire [9:0] sample_index = build_sample[21:12];
    wire before_end = !negative && (sample_index < build_last ||
        (sample_index == build_last && !build_sample[11]));
    wire before_start = negative && !(build_sample[22:12] == 11'h7ff && build_sample[11]);
    wire build_inside = !before_start && (negative || before_end ||
        (!build_axis && !filtered && mode == 1));
    wire clamped = negative || sample_index >= build_last;
    wire [9:0] tap_index = negative ? 10'd0 : clamped ? build_last : sample_index;
    wire [9:0] absolute_index = build_origin + tap_index;
    wire [18:0] build_entry = {build_inside, filtered && !clamped ? build_sample[11:4] : 8'd0, absolute_index};
    logic [18:0] x_map, y_map, next_y_map;
    wire [9:0] x_read_address = 10'(column - 10'd99);
    wire [8:0] y_address_a = build_active ? build_index[8:0] : row - 9'd4;
    always_ff @(posedge clk_vid) begin
        if (build_active && !build_axis) x_lookup[build_index] <= build_entry;
        x_map <= x_lookup[x_read_address];
        // Port A is shared between blanking-time writes and current-row reads.
        if (build_active && build_axis) y_lookup[y_address_a] <= build_entry;
        y_map <= y_lookup[y_address_a];
        next_y_map <= y_lookup[row - 9'd3];
    end
    always_ff @(posedge clk_vid) begin
        if (rst_vid || frame_start) begin
            build_active <= 0; build_axis <= 0; build_index <= 0;
        end else if (running && row == 0 && column == 83) begin
            build_active <= 1; build_axis <= 0; build_index <= 0;
        end else if (build_active) begin
            build_index <= build_index + 1'b1;
            if (build_index == (build_axis ? {1'b0, span_rows} : span_columns)) begin
                build_index <= 0; build_axis <= 1;
                if (build_axis) build_active <= 0;
            end
        end
    end
    wire [10:0] pixel_address = {row[0], x_map[9:0]};
    logic request_pair;
    pocket_video_memory store (
        .clk_sys, .rst_sys, .clk_vid, .rst_vid, .vs, .de, .skip, .rgb,
        .capture_bank, .capture_good, .completed,
        .request, .request_frame, .request_cache, .request_pair, .request_y, .acknowledge,
        .pixel_address, .pixel00, .pixel10, .pixel01, .pixel11,
        .m_valid, .m_we, .m_last, .m_addr, .m_wdata,
        .m_ready, .m_rvalid, .m_idle, .memory_ready, .m_rdata
    );
    // Cache read, horizontal interpolation, vertical interpolation: three
    // video clocks. Native pixels use zero fractions and remain bit-exact.
    function automatic [7:0] blend(input [7:0] a, b, input [8:0] f);
        logic [8:0] inverse_fraction;
        logic [16:0] weighted_a, weighted_b;
        logic [17:0] total;
        begin
            inverse_fraction = 9'd256 - f;
            weighted_a = a * inverse_fraction;
            weighted_b = b * f;
            total = {1'b0,weighted_a} + {1'b0,weighted_b} + 18'd128;
            blend = total[15:8];
        end
    endfunction
    logic [8:0] fraction_x;
    logic [7:0] fraction_y, fraction_y2;
    logic [23:0] horizontal_upper, horizontal_lower, filtered_pixel;
    logic de1, de2, hs1, hs2, valid1, valid2, valid3;
    wire [23:0] output_pixel = mode == 0 ? rgb_delay : filtered_pixel;
    assign video_rgb = video_de && (mode == 0 || valid3) ? output_pixel :
        (!video_de && out_de_d && !video_vs) ? {8'd0, announced_slot, 13'd0} : 24'd0;

    // Colour pipeline needs no reset: valid1/2/3 suppress its warm-up
    // clocks. Leaving data unreset also lets Quartus use DSP registers.
    always_ff @(posedge clk_vid) begin
        rgb_delay <= rgb;
        fraction_x <= x_map[0] ? 9'd256 - {1'b0,x_map[17:10]} : {1'b0,x_map[17:10]};
        fraction_y <= y_map[17:10];
        fraction_y2 <= fraction_y;
        for (int channel = 0; channel < 3; channel++) begin
            horizontal_upper[channel*8 +: 8] <= blend(pixel00[channel*8 +: 8], pixel10[channel*8 +: 8], fraction_x);
            horizontal_lower[channel*8 +: 8] <= blend(pixel01[channel*8 +: 8], pixel11[channel*8 +: 8], fraction_x);
            filtered_pixel[channel*8 +: 8] <= blend(horizontal_upper[channel*8 +: 8], horizontal_lower[channel*8 +: 8], {1'b0,fraction_y2});
        end
    end

    always_ff @(posedge clk_vid) begin
        if (rst_vid) begin
            settings_meta <= 0; settings_sync <= 0; announced <= 0; active <= 0; announced_slot <= 0; active_slot <= 0;
            vs_d <= 0; seen_pixel <= 0; bounds_good <= 0;
            bounds <= 0;
            frame_valid <= 0; frame_bank <= 0;
            step_x <= 0; step_y <= 0;
            row <= 0; column <= 0; running <= 0;
            request <= 0; request_frame <= 0; request_cache <= 0; request_y <= 0;
            expected <= 0; requested_line <= 0; line_ready <= 0;
            de1 <= 0; de2 <= 0; hs1 <= 0; hs2 <= 0; valid1 <= 0; valid2 <= 0; valid3 <= 0;
            request_pair <= 0;
            video_de <= 0; video_hs <= 0; video_vs <= 0; video_skip <= 0; out_de_d <= 0;
        end else begin
            settings_meta <= {docked, screen_mode}; settings_sync <= settings_meta;
            vs_d <= vs; out_de_d <= video_de;
            de1 <= picture_pixel; de2 <= de1;
            hs1 <= running && column == 0; hs2 <= hs1;
            video_de <= mode == 0 ? de : de2;
            video_hs <= mode == 0 ? hs : hs2;
            video_vs <= mode == 0 ? vs : frame_start;
            video_skip <= mode == 0 && de && skip;
            valid1 <= frame_valid && line_ready && x_map[18] && y_map[18];
            valid2 <= valid1; valid3 <= valid2;
            if (de && !skip) begin
                if (!seen_pixel) begin
                    bounds <= source_bounds;
                    bounds_good <= source_w != 0 && source_h != 0;
                    seen_pixel <= 1;
                end else if (bounds != source_bounds)
                    bounds_good <= 0; // no mixed-window frames
            end
            if (frame_start) begin
                // EOL requests apply to the NEXT frame. Mode, Dock aspect and
                // dimensions all follow the previously announced selection.
                active <= announced; announced <= settings_sync;
                active_slot <= announced_slot;
                announced_slot <= choose_slot(settings_sync, bounds[19:10], bounds[9:0]);
                seen_pixel <= 0; bounds_good <= 0;
                frame_bank <= capture_bank;
                frame_valid <= capture_good && completed[capture_bank] && bounds_good &&
                    (announced_slot == 7 || announced_slot == choose_slot(announced, bounds[19:10], bounds[9:0]));
                row <= 0; column <= 1; running <= 1;
                line_ready <= 0; requested_line <= 0;
            end else if (running) begin
                if (column == (filtered ? 10'd571 : 10'd789)) begin
                    column <= 0; row <= row + 1'b1;
                    if (row + 9'd1 == line_end) running <= 0;
                end else column <= column + 1'b1;
                // Small setup pipeline in vertical blanking, after the new
                // mode and the matching frame's bounds have been latched.
                if (row == 0 && column == 54) begin
                    step_x <= !filtered ? 14'sd4096 : mode == 1 ? ratio_x : mode == 4 ? hybrid_sum_x[14:1] : uniform_step;
                    // Convert the selected horizontal step to vertical units;
                    // Dock also accounts for slot 7's 6:5 output pixel aspect.
                    step_y <= !filtered ? 14'sd4096 : mode == 4 ? hybrid_sum_y[14:1] : mode == 1 ? stretch_y : uniform_y;
                end
                if (column == 0) begin
                    line_ready <= requested_line && acknowledge == expected;
                end
                // Prefetch the next line into the other cache pair while
                // the current line is being displayed.
                if (column == 100 && row >= 3 && row + 9'd1 < line_end && mode != 0) begin
                    requested_line <= 0;
                    if (frame_valid && next_y_map[18] && request == acknowledge) begin
                        request <= !request; expected <= !request;
                        request_frame <= frame_bank; request_cache <= !row[0];
                        request_y <= next_y_map[7:0]; request_pair <= filtered; requested_line <= 1;
                    end
                end
                // Check again immediately before DE. A row-pair request has
                // nearly a whole line (572 filtered / 790 native video clocks).
                if (picture_line && column == 99)
                    line_ready <= line_ready || (request == expected && acknowledge == expected && requested_line);
            end
        end
    end
endmodule
`default_nettype wire
