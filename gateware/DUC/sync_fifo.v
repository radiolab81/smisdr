/*
 * MODUL: sync_fifo
 * ----------------
 * Synchroner Ringpuffer (FIFO) für 32-Bit Wörter {I, Q}.
 * Nutzt "Inferred Dual-Port RAM" für optimale BRAM-Ausnutzung.
 */
module sync_fifo #(
    parameter DEPTH_BITS = 10,  // 2^10 = 1024 Einträge
    parameter DATA_WIDTH = 32   // 16 Bit I + 16 Bit Q
)(
    input  wire clk,
    input  wire rst,
    
    // Schreib-Port
    input  wire w_en,
    input  wire [DATA_WIDTH-1:0] w_data,
    
    // Lese-Port
    input  wire r_en,
    output wire [DATA_WIDTH-1:0] r_data,
    
    // Status-Flags
    output wire empty,
    output wire full
);

    localparam FIFO_DEPTH = (1 << DEPTH_BITS);

    // Der eigentliche Speicher (wird vom Tool als Block-RAM inferiert)
    (* ramstyle = "no_rw_check" *) reg [DATA_WIDTH-1:0] ram [0:FIFO_DEPTH-1];
    
    reg [DEPTH_BITS-1:0] r_ptr;
    reg [DEPTH_BITS-1:0] w_ptr;
    reg [DEPTH_BITS:0]   count; // Ein Bit mehr für den Voll/Leer-Zustand

    assign empty  = (count == 0);
    assign full   = (count == (1 << DEPTH_BITS));
    
    // Asynchrones Lesen für 0-Takt Latenz beim ZOH/CIC
    assign r_data = ram[r_ptr];

    always @(posedge clk) begin
        if (rst) begin
            r_ptr <= 0;
            w_ptr <= 0;
            count <= 0;
        end else begin
            // Schreiben
            if (w_en && !full) begin
                ram[w_ptr] <= w_data;
                w_ptr <= w_ptr + 1'b1;
            end
            
            // Lesen
            if (r_en && !empty) begin
                r_ptr <= r_ptr + 1'b1;
            end
            
            // Count-Update
            case ({w_en && !full, r_en && !empty})
                2'b10: count <= count + 1'b1; // Nur schreiben
                2'b01: count <= count - 1'b1; // Nur lesen
                // Bei 2'b11 (beides) oder 2'b00 bleibt count gleich
                default: ; 
            endcase
        end
    end
endmodule
