/*
 * MODUL: debouncer
 * ----------------
 * Unterdrückt mechanisches Prellen.
 * Signal muss für 'WAIT_CYCLES' stabil sein, bevor der Ausgang umschaltet.
 */
module debouncer #(
    parameter WAIT_CYCLES = 1_000_000 // 20ms bei 50MHz
)(
    input  wire clk,
    input  wire signal_in,
    output reg  signal_out
);

    // Die Bitbreite des Zählers (22 Bit) ist exakt dimensioniert.
    // Bei 50 MHz Systemtakt benötigt ein Zyklus 20ns.
    // 1_000_000 Zyklen * 20ns = 20 Millisekunden (typische Entprellzeit für Taster).
    // Da 2^21 = 2.097.152 und 2^20 = 1.048.576, reichen exakt 22 Bit aus,
    // um den Wert 1_000_000 ohne vorzeitigen Überlauf zu fassen.
    reg [21:0] count;
    reg last_state;

    always @(posedge clk) begin
        if (signal_in != last_state) begin
            // Signal hat gewackelt -> Zähler zurücksetzen
            count <= 0;
            last_state <= signal_in;
        end else if (count < WAIT_CYCLES) begin
            // Signal ist stabil -> Zähler läuft
            count <= count + 1;
        end else begin
            // Zähler erreicht -> Signal übernehmen
            signal_out <= last_state;
        end
    end
endmodule
