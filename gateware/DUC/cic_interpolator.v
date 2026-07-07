module cic_interpolator #(
    parameter W_IN  = 16,
    parameter W_OUT = 16,
    parameter W_INT = 32 // Interne Bitbreite zum Schutz vor Überlauf (Gain = 100)
)(
    input  wire clk,
    input  wire rst,
    input  wire valid_in, // 5 MHz Strobe vom baseband_sharpener
    input  wire signed [W_IN-1:0] i_in,
    input  wire signed [W_IN-1:0] q_in,
    output wire signed [W_OUT-1:0] i_out,
    output wire signed [W_OUT-1:0] q_out
);

    // =====================================================================
    // 1. COMB-SEKTION (Läuft mit der niedrigen 5 MHz Rate)
    // =====================================================================
    // Vorzeichenerweiterung für die interne Berechnung
    wire signed [W_INT-1:0] i_ext = {{W_INT-W_IN{i_in[W_IN-1]}}, i_in};
    wire signed [W_INT-1:0] q_ext = {{W_INT-W_IN{q_in[W_IN-1]}}, q_in};

    reg signed [W_INT-1:0] i_c1_d, i_c2_d, i_c3_d;
    reg signed [W_INT-1:0] q_c1_d, q_c2_d, q_c3_d;

    wire signed [W_INT-1:0] i_c1 = i_ext - i_c1_d;
    wire signed [W_INT-1:0] i_c2 = i_c1   - i_c2_d;
    wire signed [W_INT-1:0] i_c3 = i_c2   - i_c3_d;

    wire signed [W_INT-1:0] q_c1 = q_ext - q_c1_d;
    wire signed [W_INT-1:0] q_c2 = q_c1   - q_c2_d;
    wire signed [W_INT-1:0] q_c3 = q_c2   - q_c3_d;

    always @(posedge clk) begin
        if (rst) begin
            i_c1_d <= 0; i_c2_d <= 0; i_c3_d <= 0;
            q_c1_d <= 0; q_c2_d <= 0; q_c3_d <= 0;
        end else if (valid_in) begin
            // Update NUR wenn ein neues 5 MSps Sample kommt!
            i_c1_d <= i_ext; i_c2_d <= i_c1; i_c3_d <= i_c2;
            q_c1_d <= q_ext; q_c2_d <= q_c1; q_c3_d <= q_c2;
        end
    end

    // =====================================================================
    // 2. ZERO-STUFFING (Upsampling von 5 MSps auf 50 MSps)
    // =====================================================================
    // Wir leiten den Comb-Wert nur weiter, wenn 'valid_in' hoch ist.
    // In den restlichen 9 Taktzyklen füttern wir Nullen in den Integrator.
    wire signed [W_INT-1:0] i_int_in = valid_in ? i_c3 : {W_INT{1'b0}};
    wire signed [W_INT-1:0] q_int_in = valid_in ? q_c3 : {W_INT{1'b0}};

    // =====================================================================
    // 3. INTEGRATOR-SEKTION (Läuft durchgehend mit 50 MHz)
    // =====================================================================
    reg signed [W_INT-1:0] i_i1, i_i2, i_i3;
    reg signed [W_INT-1:0] q_i1, q_i2, q_i3;

    always @(posedge clk) begin
        if (rst) begin
            i_i1 <= 0; i_i2 <= 0; i_i3 <= 0;
            q_i1 <= 0; q_i2 <= 0; q_i3 <= 0;
        end else begin
            // Update JEDEN Takt!
            i_i1 <= i_i1 + i_int_in;
            i_i2 <= i_i2 + i_i1;
            i_i3 <= i_i3 + i_i2;

            q_i1 <= q_i1 + q_int_in;
            q_i2 <= q_i2 + q_i1;
            q_i3 <= q_i3 + q_i2;
        end
    end

    // =====================================================================
    // 4. SCALING UND AUSGABE
    // =====================================================================
    // Ein CIC-Interpolator mit R=10 und N=3 hat einen DC-Gain von R^(N-1) = 100.
    // Das heißt, das Signal wächst massiv an! 
    // Wir verschieben es um 5 Bits nach rechts (Teilung durch 32).
    // Der effektive Gain ist somit 100 / 32 = 3.125.
    // Das ersetzt deinen vorherigen " <<< 2 " Shift (was Faktor 4 war) fast perfekt!
    assign i_out = i_i3[W_OUT+4 : 5]; 
    assign q_out = q_i3[W_OUT+4 : 5]; 

endmodule
