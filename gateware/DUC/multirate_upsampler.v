/*
 * MODUL: multirate_upsampler
 * --------------------------
 * Dual-Path Upsampling Architektur:
 * 1. Pfad (1.25M):     TDM-basierte MAC-Engine mit Zero-Stuffing (L=4).
 * 2. Pfad (250k/500k): Effizientere 3-stufige Filter-Kaskade (Half-Band + Polyphase) 
 *                      zur Erhöhung der Filtersteilheit.
 */
module multirate_upsampler (
    input  wire clk,         // 50 MHz Systemtakt
    input  wire rst,         // Synchroner Reset
    
    // In-Band Konfiguration (übernimmt rate_inc_out vom smi_rx_16bit)
    input  wire [31:0] rate_config, 
    
    // FIFO Interface (0-Cycle Latency Read)
    input  wire [31:0] fifo_data,  // {I[31:16], Q[15:0]}
    input  wire        fifo_empty,
    output reg         fifo_rd_en,
    
    // Ausgang in Richtung CIC/DUC-Mixer (5 MSps)
    output reg signed [15:0] i_out,
    output reg signed [15:0] q_out,
    output reg               valid_out
);

    // =========================================================================
    // 0. GENERELLE STEUERUNG & TAKT-BASIS (50 MHz)
    // =========================================================================
    reg [1:0] rate_sel;
    always @(*) begin
        if (rate_config > 32'h0500_0000)      rate_sel = 2'b10; // 1.25 MSps
        else if (rate_config > 32'h0200_0000) rate_sel = 2'b01; // 500 ksps
        else                                  rate_sel = 2'b00; // 250 ksps
    end

    // Master-Zähler für alle synchronen Strobe-Signale
    reg [7:0] clk_div;
    always @(posedge clk) begin
        if (rst) clk_div <= 8'd0;
        else clk_div <= (clk_div == 8'd199) ? 8'd0 : clk_div + 8'd1;
    end

    // Synchrone Strobe-Pulse für die Verarbeitungsraten
    wire tick_5m   = (clk_div % 10 == 9);   // 5 MHz
    wire tick_1m   = (clk_div % 50 == 49);  // 1 MHz
    wire tick_500k = (clk_div % 100 == 99); // 500 kHz
    wire tick_250k = (clk_div == 199);      // 250 kHz

    // =========================================================================
    // 1. PFAD A: ORIGINALER 1.25 MSps ZWEIG
    // =========================================================================
    // M9K ROM für 1.25M Rate (L=4)
    reg signed [15:0] coeffs_1250 [0:39];
    initial begin
        coeffs_1250[0]  = 16'sd0;    coeffs_1250[1]  = 16'sd0;    coeffs_1250[2]  = 16'sd0;    coeffs_1250[3]  = 16'sd0;
        coeffs_1250[4]  = 16'sd0;    coeffs_1250[5]  = 16'sd0;    coeffs_1250[6]  = 16'sd0;    coeffs_1250[7]  = 16'sd1;
        coeffs_1250[8]  = -16'sd1;   coeffs_1250[9]  = -16'sd3;   coeffs_1250[10] = -16'sd3;   coeffs_1250[11] = -16'sd1;
        coeffs_1250[12] = 16'sd3;    coeffs_1250[13] = 16'sd7;    coeffs_1250[14] = 16'sd11;   coeffs_1250[15] = 16'sd7;
        coeffs_1250[16] = -16'sd8;   coeffs_1250[17] = -16'sd26;  coeffs_1250[18] = -16'sd36;  coeffs_1250[19] = -16'sd21;
        coeffs_1250[20] = 16'sd28;   coeffs_1250[21] = 16'sd93;   coeffs_1250[22] = 16'sd126;  coeffs_1250[23] = 16'sd66;
        coeffs_1250[24] = -16'sd84;  coeffs_1250[25] = -16'sd257; coeffs_1250[26] = -16'sd328; coeffs_1250[27] = -16'sd171;
        coeffs_1250[28] = 16'sd201;  coeffs_1250[29] = 16'sd586;  coeffs_1250[30] = 16'sd710;  coeffs_1250[31] = 16'sd355;
        coeffs_1250[32] = -16'sd430; coeffs_1250[33] = -16'sd1273;coeffs_1250[34] = -16'sd1577;coeffs_1250[35] = -16'sd825;
        coeffs_1250[36] = 16'sd1104; coeffs_1250[37] = 16'sd3790; coeffs_1250[38] = 16'sd6387; coeffs_1250[39] = 16'sd8017;
    end

    reg [1:0] phase_cnt_1250;
    wire      is_data_phase_1250 = (phase_cnt_1250 == 0);
    wire      insert_zero_1250   = is_data_phase_1250 ? fifo_empty : 1'b1;

    always @(posedge clk) begin
        if (rst) phase_cnt_1250 <= 0;
        else if (tick_5m) phase_cnt_1250 <= (phase_cnt_1250 == 3) ? 0 : phase_cnt_1250 + 1;
    end

    reg signed [15:0] sr_i_1250 [0:79];
    reg signed [15:0] sr_q_1250 [0:79];

    always @(posedge clk) begin
        if (rst) begin
            for (int i = 0; i < 80; i++) begin
                sr_i_1250[i] <= 0;
                sr_q_1250[i] <= 0;
            end
        end else if (tick_5m) begin
            sr_i_1250[0] <= insert_zero_1250 ? 16'd0 : $signed(fifo_data[31:16]);
            sr_q_1250[0] <= insert_zero_1250 ? 16'd0 : $signed(fifo_data[15:0]);
            for (int i = 1; i < 80; i++) begin
                sr_i_1250[i] <= sr_i_1250[i-1];
                sr_q_1250[i] <= sr_q_1250[i-1];
            end
        end
    end
    
    /* verilator lint_off WIDTHEXPAND */
    /* verilator lint_off WIDTHTRUNC */
    wire [3:0]        tdm_cycle = clk_div % 4'd10;
    reg [3:0]         stg1_cnt_1250, stg2_cnt_1250;
    /* verilator lint_on WIDTHEXPAND */
    /* verilator lint_on WIDTHTRUNC */
    reg signed [16:0] stg1_sum_i_1250 [0:3], stg1_sum_q_1250 [0:3];
    reg signed [15:0] stg1_coeff_1250 [0:3];
    //reg signed [32:0] stg2_mult_i_1250 [0:3], stg2_mult_q_1250 [0:3];
    // Zwingt Quartus bei der Synthese, die DSPs freizugeben
    (* multstyle = "logic" *) reg signed [32:0] stg2_mult_i_1250 [0:3];
    (* multstyle = "logic" *) reg signed [32:0] stg2_mult_q_1250 [0:3];

    reg signed [35:0] stg3_acc_i_1250, stg3_acc_q_1250;
    reg               stg3_valid_1250;

    always @(posedge clk) begin
        if (rst) begin
            stg1_cnt_1250 <= 0; stg2_cnt_1250 <= 0; stg3_valid_1250 <= 0;
            stg3_acc_i_1250 <= 0; stg3_acc_q_1250 <= 0;
        end else begin
            stg1_cnt_1250 <= tdm_cycle;
            for (int k = 0; k < 4; k++) begin
                stg1_sum_i_1250[k] <= sr_i_1250[tdm_cycle*4 + k] + sr_i_1250[79 - (tdm_cycle*4 + k)];
                stg1_sum_q_1250[k] <= sr_q_1250[tdm_cycle*4 + k] + sr_q_1250[79 - (tdm_cycle*4 + k)];
                stg1_coeff_1250[k] <= coeffs_1250[tdm_cycle*4 + k];
            end

            stg2_cnt_1250 <= stg1_cnt_1250;
            for (int k = 0; k < 4; k++) begin
                stg2_mult_i_1250[k] <= stg1_sum_i_1250[k] * stg1_coeff_1250[k];
                stg2_mult_q_1250[k] <= stg1_sum_q_1250[k] * stg1_coeff_1250[k];
            end

            // Use explicit sign-extension to 36 bits before addition to satisfy Verilator
            if (stg2_cnt_1250 == 0) begin
                stg3_acc_i_1250 <= {{3{stg2_mult_i_1250[0][32]}}, stg2_mult_i_1250[0]} + 
                                   {{3{stg2_mult_i_1250[1][32]}}, stg2_mult_i_1250[1]} + 
                                   {{3{stg2_mult_i_1250[2][32]}}, stg2_mult_i_1250[2]} + 
                                   {{3{stg2_mult_i_1250[3][32]}}, stg2_mult_i_1250[3]};

                stg3_acc_q_1250 <= {{3{stg2_mult_q_1250[0][32]}}, stg2_mult_q_1250[0]} + 
                                   {{3{stg2_mult_q_1250[1][32]}}, stg2_mult_q_1250[1]} + 
                                   {{3{stg2_mult_q_1250[2][32]}}, stg2_mult_q_1250[2]} + 
                                   {{3{stg2_mult_q_1250[3][32]}}, stg2_mult_q_1250[3]};
            end else begin
                stg3_acc_i_1250 <= stg3_acc_i_1250 + 
                                   {{3{stg2_mult_i_1250[0][32]}}, stg2_mult_i_1250[0]} + 
                                   {{3{stg2_mult_i_1250[1][32]}}, stg2_mult_i_1250[1]} + 
                                   {{3{stg2_mult_i_1250[2][32]}}, stg2_mult_i_1250[2]} + 
                                   {{3{stg2_mult_i_1250[3][32]}}, stg2_mult_i_1250[3]};

                stg3_acc_q_1250 <= stg3_acc_q_1250 + 
                                   {{3{stg2_mult_q_1250[0][32]}}, stg2_mult_q_1250[0]} + 
                                   {{3{stg2_mult_q_1250[1][32]}}, stg2_mult_q_1250[1]} + 
                                   {{3{stg2_mult_q_1250[2][32]}}, stg2_mult_q_1250[2]} + 
                                   {{3{stg2_mult_q_1250[3][32]}}, stg2_mult_q_1250[3]};
            end
            stg3_valid_1250 <= (stg2_cnt_1250 == 9);
        end
    end

    // Gain L=4 für 1.25M
    reg signed [39:0] stg4_scaled_i_1250, stg4_scaled_q_1250;
    reg               stg4_valid_1250;
    always @(posedge clk) begin
        stg4_valid_1250 <= stg3_valid_1250;
        if (stg3_valid_1250) begin
            // KORREKTUR: Vorzeichenerhaltende Erweiterung auf 40 Bit vor dem Shift
            stg4_scaled_i_1250 <= {{4{stg3_acc_i_1250[35]}}, stg3_acc_i_1250} <<< 2;
            stg4_scaled_q_1250 <= {{4{stg3_acc_q_1250[35]}}, stg3_acc_q_1250} <<< 2;
        end
    end

    // =========================================================================
    // 2. PFAD B: KASKADIERTE ARCHITEKTUR FÜR 250k / 500k
    // =========================================================================
    
    // --- STUFE 1: 250k -> 500k (L=2 Half-Band) ---
    // 11-Tap Half-Band Filter (Unity Gain 2048)
    reg signed [15:0] s1_sr_i [0:5], s1_sr_q [0:5];
    reg signed [15:0] s1_out_i, s1_out_q;

    wire signed [16:0] s1_i_sum0 = $signed({s1_sr_i[0][15], s1_sr_i[0]}) + $signed({s1_sr_i[5][15], s1_sr_i[5]});
    wire signed [16:0] s1_i_sum1 = $signed({s1_sr_i[1][15], s1_sr_i[1]}) + $signed({s1_sr_i[4][15], s1_sr_i[4]});
    wire signed [16:0] s1_i_sum2 = $signed({s1_sr_i[2][15], s1_sr_i[2]}) + $signed({s1_sr_i[3][15], s1_sr_i[3]});
    
    wire signed [16:0] s1_q_sum0 = $signed({s1_sr_q[0][15], s1_sr_q[0]}) + $signed({s1_sr_q[5][15], s1_sr_q[5]});
    wire signed [16:0] s1_q_sum1 = $signed({s1_sr_q[1][15], s1_sr_q[1]}) + $signed({s1_sr_q[4][15], s1_sr_q[4]});
    wire signed [16:0] s1_q_sum2 = $signed({s1_sr_q[2][15], s1_sr_q[2]}) + $signed({s1_sr_q[3][15], s1_sr_q[3]});

    always @(posedge clk) begin
        if (tick_250k && rate_sel == 2'b00) begin
            s1_sr_i[0] <= $signed(fifo_data[31:16]);
            s1_sr_q[0] <= $signed(fifo_data[15:0]);
            for(int i=1; i<6; i++) begin
                s1_sr_i[i] <= s1_sr_i[i-1];
                s1_sr_q[i] <= s1_sr_q[i-1];
            end
        end
        if (tick_500k) begin
            if (clk_div == 99) begin // Phase 1 (Odd-Tap Convolution)
                // KORREKTUR: Explizites Cast auf 16-Bit zur Unterdrückung der WIDTHTRUNC Warnung
                s1_out_i <= 16'((s1_i_sum0 * 20 - s1_i_sum1 * 116 + s1_i_sum2 * 608) >>> 10);
                s1_out_q <= 16'((s1_q_sum0 * 20 - s1_q_sum1 * 116 + s1_q_sum2 * 608) >>> 10);
            end else begin // Phase 0 (Center Tap, clk_div == 199)
                s1_out_i <= s1_sr_i[2];
                s1_out_q <= s1_sr_q[2];
            end
        end
    end

    // --- STUFE 2: 500k -> 1M (L=2 Half-Band) ---
    // 15-Tap Half-Band Filter (Unity Gain 2048)
    reg signed [15:0] s2_sr_i [0:7], s2_sr_q [0:7];
    reg signed [15:0] s2_out_i, s2_out_q;

    wire signed [16:0] s2_i_sum0 = $signed({s2_sr_i[0][15], s2_sr_i[0]}) + $signed({s2_sr_i[7][15], s2_sr_i[7]});
    wire signed [16:0] s2_i_sum1 = $signed({s2_sr_i[1][15], s2_sr_i[1]}) + $signed({s2_sr_i[6][15], s2_sr_i[6]});
    wire signed [16:0] s2_i_sum2 = $signed({s2_sr_i[2][15], s2_sr_i[2]}) + $signed({s2_sr_i[5][15], s2_sr_i[5]});
    wire signed [16:0] s2_i_sum3 = $signed({s2_sr_i[3][15], s2_sr_i[3]}) + $signed({s2_sr_i[4][15], s2_sr_i[4]});
    
    wire signed [16:0] s2_q_sum0 = $signed({s2_sr_q[0][15], s2_sr_q[0]}) + $signed({s2_sr_q[7][15], s2_sr_q[7]});
    wire signed [16:0] s2_q_sum1 = $signed({s2_sr_q[1][15], s2_sr_q[1]}) + $signed({s2_sr_q[6][15], s2_sr_q[6]});
    wire signed [16:0] s2_q_sum2 = $signed({s2_sr_q[2][15], s2_sr_q[2]}) + $signed({s2_sr_q[5][15], s2_sr_q[5]});
    wire signed [16:0] s2_q_sum3 = $signed({s2_sr_q[3][15], s2_sr_q[3]}) + $signed({s2_sr_q[4][15], s2_sr_q[4]});

    always @(posedge clk) begin
        if (tick_500k) begin
            // MUX: Entweder Kaskade von 250k übernehmen oder nativ 500k von FIFO lesen
            s2_sr_i[0] <= (rate_sel == 2'b00) ? s1_out_i : $signed(fifo_data[31:16]);
            s2_sr_q[0] <= (rate_sel == 2'b00) ? s1_out_q : $signed(fifo_data[15:0]);
            for(int i=1; i<8; i++) begin
                s2_sr_i[i] <= s2_sr_i[i-1];
                s2_sr_q[i] <= s2_sr_q[i-1];
            end
        end
        if (tick_1m) begin
            if (clk_div % 100 == 49) begin // Phase 1
                // KORREKTUR: Explizites Cast auf 16-Bit
                s2_out_i <= 16'((s2_i_sum0 * (-10) + s2_i_sum1 * 45 - s2_i_sum2 * 160 + s2_i_sum3 * 637) >>> 10);
                s2_out_q <= 16'((s2_q_sum0 * (-10) + s2_q_sum1 * 45 - s2_q_sum2 * 160 + s2_q_sum3 * 637) >>> 10);
            end else begin // Phase 0
                s2_out_i <= s2_sr_i[3];
                s2_out_q <= s2_sr_q[3];
            end
        end
    end

    // --- STUFE 3: 1M -> 5M (L=5 Polyphase Interpolator) ---
    // Mathematisch optimierter 25-Tap FIR (Gain 5120 -> Shift 10)
    reg signed [15:0] s3_sr_i [0:4], s3_sr_q [0:4];
    reg signed [15:0] s3_out_i, s3_out_q;

    always @(posedge clk) begin
        if (tick_1m) begin
            s3_sr_i[0] <= s2_out_i;
            s3_sr_q[0] <= s2_out_q;
            for(int i=1; i<5; i++) begin
                s3_sr_i[i] <= s3_sr_i[i-1];
                s3_sr_q[i] <= s3_sr_q[i-1];
            end
        end
        
        if (tick_5m) begin
            case (clk_div % 50)
                8'd49: begin // Phase 0
                    s3_out_i <= s3_sr_i[2];
                    s3_out_q <= s3_sr_q[2];
                end
                8'd9: begin // Phase 1
                    s3_out_i <= ($signed(s3_sr_i[0])*(-10) + $signed(s3_sr_i[1])*180 + $signed(s3_sr_i[2])*880 - $signed(s3_sr_i[3])*40 + $signed(s3_sr_i[4])*14) >>> 10;
                    s3_out_q <= ($signed(s3_sr_q[0])*(-10) + $signed(s3_sr_q[1])*180 + $signed(s3_sr_q[2])*880 - $signed(s3_sr_q[3])*40 + $signed(s3_sr_q[4])*14) >>> 10;
                end
                8'd19: begin // Phase 2
                    s3_out_i <= ($signed(s3_sr_i[0])*(-30) + $signed(s3_sr_i[1])*500 + $signed(s3_sr_i[2])*580 - $signed(s3_sr_i[3])*40 + $signed(s3_sr_i[4])*14) >>> 10;
                    s3_out_q <= ($signed(s3_sr_q[0])*(-30) + $signed(s3_sr_q[1])*500 + $signed(s3_sr_q[2])*580 - $signed(s3_sr_q[3])*40 + $signed(s3_sr_q[4])*14) >>> 10;
                end
                8'd29: begin // Phase 3
                    s3_out_i <= ($signed(s3_sr_i[0])*14 - $signed(s3_sr_i[1])*40 + $signed(s3_sr_i[2])*580 + $signed(s3_sr_i[3])*500 - $signed(s3_sr_i[4])*30) >>> 10;
                    s3_out_q <= ($signed(s3_sr_q[0])*14 - $signed(s3_sr_q[1])*40 + $signed(s3_sr_q[2])*580 + $signed(s3_sr_q[3])*500 - $signed(s3_sr_q[4])*30) >>> 10;
                end
                8'd39: begin // Phase 4
                    s3_out_i <= ($signed(s3_sr_i[0])*14 - $signed(s3_sr_i[1])*40 + $signed(s3_sr_i[2])*880 + $signed(s3_sr_i[3])*180 - $signed(s3_sr_i[4])*10) >>> 10;
                    s3_out_q <= ($signed(s3_sr_q[0])*14 - $signed(s3_sr_q[1])*40 + $signed(s3_sr_q[2])*880 + $signed(s3_sr_q[3])*180 - $signed(s3_sr_q[4])*10) >>> 10;
                end
                default: begin
                    s3_out_i <= s3_out_i;
                    s3_out_q <= s3_out_q;
                end
            endcase
        end
    end

    // =========================================================================
    // 3. FIFO READ & FINAL OUTPUT MUX
    // =========================================================================
    always @(posedge clk) begin
        if (rst) fifo_rd_en <= 1'b0;
        else begin
            if (rate_sel == 2'b10)      fifo_rd_en <= tick_5m & is_data_phase_1250 & !fifo_empty;
            else if (rate_sel == 2'b01) fifo_rd_en <= tick_500k & !fifo_empty;
            else                        fifo_rd_en <= tick_250k & !fifo_empty;
        end
    end

    // Mathematisch korrekte Rundung auf Q15 (nur für Zweig A nötig)
    localparam SHIFT_VAL = 15;
    localparam signed [39:0] ROUND_OFFSET = 40'sd1 <<< (SHIFT_VAL - 1);
    
    wire signed [39:0] rounded_i_1250 = stg4_scaled_i_1250 + ROUND_OFFSET;
    wire signed [39:0] rounded_q_1250 = stg4_scaled_q_1250 + ROUND_OFFSET;

    function signed [15:0] saturate;
        input signed [39:0] val;
        begin
            if (val > 40'sd32767)       saturate = 16'sd32767;
            else if (val < -40'sd32768) saturate = -16'sd32768;
            else                        saturate = $signed(val[15:0]);
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            valid_out <= 1'b0;
            i_out     <= 16'd0;
            q_out     <= 16'd0;
        end else begin
            if (rate_sel == 2'b10) begin
                valid_out <= stg4_valid_1250;
                if (stg4_valid_1250) begin
                    i_out <= saturate(rounded_i_1250 >>> SHIFT_VAL);
                    q_out <= saturate(rounded_q_1250 >>> SHIFT_VAL);
                end
            end else begin
                // Synchronisierter Ausgang für die 250k/500k Kaskade
                valid_out <= tick_5m;
                if (tick_5m) begin
                    i_out <= s3_out_i;
                    q_out <= s3_out_q;
                end
            end
        end
    end

endmodule
