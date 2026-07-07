module shared_sine_rom (
    input wire clk,
    input wire [11:0] addr_a,
    input wire [11:0] addr_b,
    output reg signed [15:0] q_a,
    output reg signed [15:0] q_b
);
    // Das physische ROM: Nur 1024 Einträge
    (* ramstyle = "M4K" *) reg signed [15:0] sine_quarter [0:1023];
    initial $readmemh("sin_quarter.hex", sine_quarter);

    // Hilfsregister für die Pipeline
    reg signed [15:0] raw_q_a, raw_q_b;
    reg invert_q_a, invert_q_b;

    always @(posedge clk) begin
        // --- TAKT 1: RAM-Zugriff & Adress-Spiegelung ---
        // Bit 10 steuert die Spiegelung innerhalb der Halbwelle
        raw_q_a <= sine_quarter[addr_a[10] ? ~addr_a[9:0] : addr_a[9:0]];
        raw_q_b <= sine_quarter[addr_b[10] ? ~addr_b[9:0] : addr_b[9:0]];
        
        // Bit 11 (MSB) steuert das Vorzeichen (Quadrant 3 & 4)
        invert_q_a <= addr_a[11];
        invert_q_b <= addr_b[11];

        // --- TAKT 2: Symmetrie-Anpassung (Invertierung) ---
        // Hier werden die finalen Ausgänge gesetzt
        q_a <= invert_q_a ? -raw_q_a : raw_q_a;
        q_b <= invert_q_b ? -raw_q_b : raw_q_b;
    end

endmodule
