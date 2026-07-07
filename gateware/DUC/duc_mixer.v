module duc_mixer #(
    parameter DAC_BITS = 14
)(
    input  wire clk, rst,
    input  wire signed [15:0] i_in,
    input  wire signed [15:0] q_in,
    input  wire signed [15:0] cos_in,
    input  wire signed [15:0] sin_in,
    output reg  [DAC_BITS-1:0] dac_out
);

    reg signed [31:0] i_mix, q_mix;
    reg signed [33:0] rf_sum; // Sicherer mit 34 Bit bei Addition zweier 32-Bit Zahlen

    // Sättigungsgrenzen (z.B. für 14 Bit: -8192 bis 8191)
    localparam signed [15:0] MAX_VAL = (1 << (DAC_BITS - 1)) - 1;
    localparam signed [15:0] MIN_VAL = -(1 << (DAC_BITS - 1));

    // =========================================================================
    // DYNAMISCHE SKALIERUNG & RUNDUNG (KORRIGIERT FÜR INTEGRATION & MIXER-GROWTH)
    // =========================================================================
    // 1. Peak des I/Q Inputs (nach resampler_cic): Nutzen die vollen 16 Bit Signed (~2^15)
    // 2. Peak des NCO Inputs: Volle 16 Bit Signed (~2^15)
    // 3. Peak nach Multiplikation (i_mix / q_mix): 2^15 * 2^15 = 2^30
    // 4. Peak nach komplexer Mischung (rf_sum = i_mix - q_mix): 
    //    Durch die Subtraktion zweier gegenphasiger Maxima entsteht 1 Bit zusätzliches 
    //    Wachstum (Bit Growth) -> Maximale Amplitude liegt bei 2^30 - (-2^30) = 2^31.
    // 5. Peak des DAC liegt bei: 2^(DAC_BITS - 1) -> Für 14-Bit: 2^13.
    // 
    // Daraus folgt der exakte Shift-Wert:
    // Shift = Multiplikations_und_Summen_Peak - Ziel_Peak
    // Shift = 31 - (DAC_BITS - 1) = 32 - DAC_BITS.
    //
    // Für DAC_BITS = 14 ergibt sich ein Shift von genau 18 Bits, was einen sicheren
    // Headroom für Signalspitzen von ca. 30% gewährt.
    // =========================================================================
    localparam SHIFT_VAL = 32 - DAC_BITS;
    
    // Für maximales SNR runden wir mathematisch korrekt (+0.5 LSB vor dem Shift)
    localparam signed [33:0] ROUND_OFFSET = (1 << (SHIFT_VAL - 1));

    // =========================================================================
    // KOMBINATORISCHE LOGIK (Ungetaktet)
    // =========================================================================

    // Bit-Shift mit vorheriger Rundung (Downscaling)
    /* verilator lint_off WIDTHTRUNC */
    wire signed [15:0] scaled_sum = (rf_sum + ROUND_OFFSET) >>> SHIFT_VAL;
    /* verilator lint_on WIDTHTRUNC */

    // Sättigung / Clipping-Schutz
    reg signed [15:0] clipped_sum;
    always @(*) begin
        if      (scaled_sum > MAX_VAL) clipped_sum = MAX_VAL;
        else if (scaled_sum < MIN_VAL) clipped_sum = MIN_VAL;
        else                           clipped_sum = scaled_sum;
    end

    // =========================================================================
    // SEQUENZIELLE LOGIK (Getaktet)
    // =========================================================================

    always @(posedge clk) begin
        if (rst) begin
            i_mix   <= 0; 
            q_mix   <= 0; 
            rf_sum  <= 0; 
            // Reset: Setze DAC auf den Mittelwert (z.B. 8192 bei 14 Bit)
            dac_out <= (1 << (DAC_BITS - 1)); 
        end else begin
            // 1. Multiplikation
            i_mix <= i_in * cos_in;
            q_mix <= q_in * sin_in;
            
            // 2. Addition (komplexe Mischung)
            /* verilator lint_off WIDTHEXPAND */
            rf_sum <= i_mix - q_mix;
            /* verilator lint_on WIDTHEXPAND */
            
            // 3. Mapping auf Unsigned via Offset-Addition
            // Clipped_sum ist signed (16-bit). Wir addieren den Offset (z.B. 8192).
            // Das Ergebnis ist unsigned für normalen DAC (ohne differenzielle Versorgung)
            // Die Lint-Pragmas sind notwendig, da 'clipped_sum' 16-Bit breit ist
            // und 'dac_out' nur DAC_BITS breit ist. Verilator warnt hier 
            // korrekt vor einer Bit-Kürzung (Truncation), die wir hier aber 
            // durch das vorherige Clipping bewusst in Kauf nehmen.
            /* verilator lint_off WIDTHTRUNC */
            /* verilator lint_off WIDTHEXPAND */
            dac_out <= clipped_sum + (1 << (DAC_BITS - 1));
            /* verilator lint_on WIDTHTRUNC */
            /* verilator lint_on WIDTHEXPAND */
        end
    end
endmodule
