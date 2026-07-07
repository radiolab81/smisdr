module nco_dual (
    input  wire clk, rst,
    input  wire [31:0] phase_inc,
    output wire signed [15:0] cos_out,
    output wire signed [15:0] sin_out
);

    reg [31:0] phase_acc;
    always @(posedge clk) begin
        if (rst) phase_acc <= 0;
        else     phase_acc <= phase_acc + phase_inc;
    end
    
    // Sinus: Normale Phase
    wire [11:0] addr_sin = phase_acc[31:20];
    
    // Cosinus: +90 Grad. Bei 4096 (12-Bit) Vollwelle sind 90 Grad exakt 1024.
    wire [11:0] addr_cos = phase_acc[31:20] + 12'd1024; 
    
    shared_sine_rom rom_inst (
        .clk(clk),
        .addr_a(addr_sin), .q_a(sin_out),
        .addr_b(addr_cos), .q_b(cos_out)
    );

endmodule
