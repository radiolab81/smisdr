/*
 * MODUL: smi_rx_16bit (In-Band Signaling Version)
 * -----------------------------------------------
 * Parst den 16-Bit smiBus / PARLIO Datenstrom.
 * - Nutzt Bit [15:14] zur Unterscheidung zwischen I/Q und Commands.
 * - Erweitert 14-Bit I/Q Samples auf 16-Bit Signed für den DSP.
 * - Fügt I und Q zusammen und pusht sie als 32-Bit Wort in den FIFO.
 * - Baut 32-Bit Befehle (Rate/Shift) indexiert zusammen.
 */
module smi_rx_16bit #(
    parameter TIMEOUT_CYCLES = 32'd50_000_000, // Watchdog Timeout
    parameter FILTER_TAPS    = 4               // Glitch-Filter für Strobe
)(
    input  wire clk,
    input  wire rst,
    input  wire [15:0] data_in,
    input  wire data_en,
    
    output reg [31:0] fifo_w_data,   // {I_16bit, Q_16bit}
    output reg        fifo_w_en,     // FIFO Write Strobe
    output reg [31:0] nco_freq_out,  // Verschiebefrequenz (Shift)
    output reg [31:0] rate_inc_out   // Samplerate (Rate)
);

    // =========================================================================
    // 1. EMV HÄRTUNG & SYNCHRONISIERUNG (Metastabilität & Glitch Filter)
    // =========================================================================
    reg [15:0] data_meta, data_sync;
    reg [2:0]  en_meta;
    reg [FILTER_TAPS-1:0] en_filter;
    reg en_clean, en_clean_prev;
    
    wire de_pulse = (en_clean && !en_clean_prev);

    always @(posedge clk) begin
        if (rst) begin
            data_meta <= 0; data_sync <= 0; en_meta <= 0;
            en_filter <= 0; en_clean <= 0; en_clean_prev <= 0;
        end else begin
            data_meta <= data_in; data_sync <= data_meta;
            en_meta <= {en_meta[1:0], data_en};
            en_filter <= {en_filter[FILTER_TAPS-2:0], en_meta[2]};
            
            if (&en_filter) en_clean <= 1'b1;
            else if (~|en_filter) en_clean <= 1'b0;
            
            en_clean_prev <= en_clean;
        end
    end

    // =========================================================================
    // 2. PROTOKOLL PARSER (In-Band Signaling)
    // =========================================================================
    
    // Protokoll-Felder aufschlüsseln
    wire [1:0] ctrl_bits = data_sync[15:14];
    wire [5:0] idx_bits  = data_sync[13:8];
    wire [7:0] pay_byte  = data_sync[7:0];

    // Status-Automaten Definition für Befehle
    localparam CMD_IDLE = 1'b0;
    localparam CMD_RECV = 1'b1;

    reg        cmd_state;
    reg [7:0]  active_cmd;       // Speichert 'R' oder 'S'
    reg [31:0] param_buf;        // Baut den 32-Bit Parameter zusammen
    reg signed [15:0] i_latch;   // Speichert I-Sample, bis Q ankommt
    
    reg [31:0] watchdog;

    always @(posedge clk) begin
        if (rst) begin
            fifo_w_data  <= 0;
            fifo_w_en    <= 0;
            cmd_state    <= CMD_IDLE;
            active_cmd   <= 0;
            param_buf    <= 0;
            i_latch      <= 0;
            watchdog     <= 0;
            
            // Standardwerte (Safe-Defaults)
            nco_freq_out <= 32'h0F70_0020; 
            rate_inc_out <= 32'h00A3_D70A;
        end else begin
            fifo_w_en <= 1'b0; // Default: Nichts in den FIFO schreiben
            

            // --- Watchdog Logic (Schützt den Command-State vor Hängern) ---
            // Wenn das feeding Python/C-Skript oder der Bus mitten in einer Befehls-
            // übertragung (z.B. nach dem Senden von 2 statt 4 Bytes) abstürzt oder beendet
            // wird, würde die State Machine für immer im Zustand CMD_RECV blockieren.
            // Der Watchdog zählt hier hoch: Wird 1 Sekunde lang (50.000.000 Takte) kein 
            // valides Byte empfangen, wird die State Machine radikal auf IDLE zurückgesetzt.
            if (cmd_state != CMD_IDLE) begin
                if (de_pulse) watchdog <= 0;
                else begin
                    watchdog <= watchdog + 1;
                    if (watchdog >= TIMEOUT_CYCLES) begin
                        cmd_state <= CMD_IDLE; 
                        watchdog <= 0;
                    end
                end
            end else begin
                watchdog <= 0;
            end

            // --- Daten Verarbeitung ---
            // Protokoll-Aufbau für den sendenden Host (z.B. Raspberry Pi):
            // Bit [15:14] | Funktion
            // --------------------------------------------------------
            //   00        | I-Sample (untere 14 Bit sind Daten)
            //   01        | Q-Sample (untere 14 Bit sind Daten)
            //   10        | Kommando-Parameter (Bit 13:8 Index, Bit 7:0 Payload)
            //   11        | Kommando-Ende/Execute (Payload = "E" für End/Execute)
            // 
            // Beispiel: Setze NCO ('S') -> Sende INIT (Idx 63, Val 'S'), 
            // sende 4 Bytes (Idx 0-3), sende EXEC (11).
            if (de_pulse) begin
                case (ctrl_bits)
            if (de_pulse) begin
                case (ctrl_bits)
                    
                    // ----------------------------------------------------
                    // 00: I-Sample
                    // ----------------------------------------------------
                    2'b00: begin
                        // 14-Bit auf 16-Bit Sign-Extension:
                        // Wir kopieren das Vorzeichenbit (Bit 13) zweimal nach oben.
                        i_latch <= $signed({ {2{data_sync[13]}}, data_sync[13:0] });
                    end
                    
                    // ----------------------------------------------------
                    // 01: Q-Sample
                    // ----------------------------------------------------
                    2'b01: begin
                        // Vorzeichenerweiterung für Q und Kombination mit I
                        /* verilator lint_off UNUSEDSIGNAL */ // (Falls Linter meckert)
                        fifo_w_data <= { i_latch, $signed({ {2{data_sync[13]}}, data_sync[13:0] }) };
                        /* verilator lint_on UNUSEDSIGNAL */
                        fifo_w_en   <= 1'b1; // Jetzt parallel in den DSP-Core/FIFO pushen
                    end
                    
                    // ----------------------------------------------------
                    // 10: Command Init oder Param Chunk
                    // ----------------------------------------------------
                    2'b10: begin
                        if (idx_bits == 6'd63) begin
                            // INIT: Neues Kommando startet
                            active_cmd <= pay_byte;
                            param_buf  <= 32'd0;
                            cmd_state  <= CMD_RECV;
                        end else if (cmd_state == CMD_RECV) begin
                            // CHUNK: Parameter-Bytes anhand des Index einsortieren (Little Endian Annahme)
                            case (idx_bits)
                                6'd0: param_buf[7:0]   <= pay_byte;
                                6'd1: param_buf[15:8]  <= pay_byte;
                                6'd2: param_buf[23:16] <= pay_byte;
                                6'd3: param_buf[31:24] <= pay_byte;
                                default: ; // Bei 32-Bit ignorieren wir Indizes > 3
                            endcase
                        end
                    end
                    
                    // ----------------------------------------------------
                    // 11: Command End
                    // ----------------------------------------------------
                    2'b11: begin
                        if (cmd_state == CMD_RECV && pay_byte == "E") begin
                            // Ausführen: Puffer in das echte Register übernehmen
                            if (active_cmd == "R") rate_inc_out <= param_buf;
                            if (active_cmd == "S") nco_freq_out <= param_buf;
                            
                            cmd_state <= CMD_IDLE; // Zurücksetzen
                        end
                    end
                    
                endcase
            end
        end
    end
endmodule
