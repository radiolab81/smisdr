/*
 * PROJEKT: Digital Upconverter (DUC) für smiSDR, parlioSDR, ...
 * ----------------------------------------------------
 * Nimmt I/Q Baseband-Samples über ein 16-Bit Interface entgegen, siehe smi_rx_16bit.v,
 * führt ein Upsampling durch und mischt das Signal ins HF-Band.
 */
// =========================================================================
// SYSTEM-ARCHITEKTUR & TAKTDOMÄNEN (Info für Nachbauer):
// =========================================================================
// Host-Schnittstelle: 'data_in' kommt asynchron oder mit eigenem Takt. 
//    Der FIFO puffert diese Bursts (z.B. 250k, 500k oder 1.25M Samples/s).
// DSP-Kern: Läuft komplett und synchron mit dem 'clk' (z.B. 50 MHz).
// Upsampling-Pfad:
//    -> FIFO (Basisband)
//    -> multirate_upsampler (bringt alles auf konstante 5 MSps)
//    -> baseband_sharpener (Filtert bei 5 MSps)
//    -> cic_interpolator (Pusht die 5 MSps hoch auf den 50 MHz Systemtakt)
//    -> duc_mixer (Mischt das 50 MSps Signal auf die Trägerfrequenz)
// =========================================================================
module duc_top #(
    parameter DAC_BITS = 14  // Konfigurierbar für 8 bis 14 Bit DACs
)(
    input  wire clk,         // Schneller Systemtakt (z.B. 50 MHz oder via PLL)
    input  wire rst,         // Asynchroner Reset (Taster)
    
    // 16-Bit SMI, parlio oder anderes paralelle Interface vom Raspberry Pi, ESP32P4, ...
    input  wire [15:0] data_in,
    input  wire data_en,     // Strobe (Write-Enable) vom SMI
    
    // Ausgang zum DAC
    output wire [DAC_BITS-1:0] dac_out
);

    // --- 0. Entprellung des Resets ---
    wire sys_rst; // Dieser Draht wird nun vom Debouncer getrieben

`ifdef VERILATOR
    // Im Simulator umgehen wir den Debouncer und nutzen direkt 
    // das logische Verhalten der Testbench (Active-High Reset)
    assign sys_rst = rst;
`else
    // Auf der echten Hardware nutzen wir den Debouncer 
    // und korrigieren den physikalisch invertierten Taster
    // Instanziierung des Entprell-Moduls
    debouncer #(
        .WAIT_CYCLES(5000)
    ) rst_filter (
        .clk(clk),
        .signal_in(!rst),      // (!)für den Fall, dass RST auf dem Board invertiert ist 
        .signal_out(sys_rst)   // Stabiles Ausgangssignal
    );
`endif

    // --- 1. SMI Empfänger & Protokoll-Parser ---
    wire [31:0] nco_freq;
    wire [31:0] rate_inc;
    wire [31:0] fifo_w_data; // {I[15:0], Q[15:0]}
    wire fifo_w_en;

    smi_rx_16bit rx_inst (
        .clk(clk), .rst(sys_rst), .data_in(data_in), .data_en(data_en),
        .fifo_w_data(fifo_w_data), .fifo_w_en(fifo_w_en),
        .nco_freq_out(nco_freq), .rate_inc_out(rate_inc)
    );

    // --- 2. Synchroner FIFO als Burst-Puffer ---
    wire [31:0] fifo_r_data;
    wire fifo_rd_en;
    wire fifo_empty;

    /* verilator lint_off UNUSEDSIGNAL */
    wire fifo_full;
    /* verilator lint_off UNUSEDSIGNAL */

    sync_fifo #(.DEPTH_BITS(10)) fifo_inst ( // 1024 I/Q Samples Puffer
        .clk(clk), .rst(sys_rst),
        .w_en(fifo_w_en), .w_data(fifo_w_data),
        .r_en(fifo_rd_en), .r_data(fifo_r_data),
        .empty(fifo_empty), .full(fifo_full)
    );

    // --- 3. Rational Resampler ---
    // --- Erste Stufe: Multirate Upsampling auf 5M ---
    wire signed [15:0] i_sample;
    wire signed [15:0] q_sample;
    wire               valid_stage1;
   
    multirate_upsampler filter_inst (
        .clk(clk), 
        .rst(sys_rst),
        .rate_config(rate_inc),     // Erkennt selbst, ob es 250k, 500k oder 1.25M ist! Wert kommt über In-band signaling
        .fifo_empty(fifo_empty), 
        .fifo_data(fifo_r_data),
        .fifo_rd_en(fifo_rd_en), 
        .i_out(i_sample), 
        .q_out(q_sample),
        .valid_out(valid_stage1) 
    );


    // --- Zweite Stufe: Clean-Up LPF , arbeitet in der 5MS Domaine ---
    wire signed [15:0] i_sample_2;
    wire signed [15:0] q_sample_2;
    wire      valid_sharpener_out;    


    baseband_sharpener baseband_sharpener_inst (
        .clk(clk), .rst(sys_rst),
        .rate_config(rate_inc),
        .valid_in(valid_stage1),
        .i_in(i_sample), .q_in(q_sample),
        .i_out(i_sample_2), .q_out(q_sample_2),
        .valid_out(valid_sharpener_out)
    );


    // --- Dritte Stufe: Upsampling auf Systemtakt (def. 50 MS)
    wire signed [15:0] i_sample_cic;
    wire signed [15:0] q_sample_cic;

    cic_interpolator cic_inst (
        .clk(clk),
        .rst(sys_rst),
        .valid_in(valid_sharpener_out), // 5 MHz Strobe aus dem Filter
        .i_in(i_sample_2),
        .q_in(q_sample_2),
        .i_out(i_sample_cic),
        .q_out(q_sample_cic)
    );

    // --- 4. Dual-Phase NCO , stellt das LO-Signal für den I/Q Mischer ---
    // Einstellung der NCO Frequenz über das In-band signaling, siehe smi_rx_16bit.v
    wire signed [15:0] cos_val;
    wire signed [15:0] sin_val;

    nco_dual nco_inst (
        .clk(clk), .rst(sys_rst),
        .phase_inc(nco_freq),
        .cos_out(cos_val), .sin_out(sin_val)
    );

    // --- 5. I/Q Mischer & Clipping-Schutz ---
    duc_mixer #(.DAC_BITS(DAC_BITS)) mixer_inst (
        .clk(clk), .rst(sys_rst),
        .i_in(i_sample_cic), .q_in(q_sample_cic),
        .cos_in(cos_val), .sin_in(sin_val),
        .dac_out(dac_out)
    );

endmodule
