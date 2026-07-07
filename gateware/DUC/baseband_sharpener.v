/*
 * MODUL: baseband_sharpener (M9K VERSION)
 * ---------------------------------------------------
 * - Teilt die Speicher strikt in 6 diskrete Arrays auf.
 * - Sichert M9K-Inference durch Trennung von RAM-Ausgangsregistern und ALU.
 * - Behebt 'constant value overflow' und 'truncated value' Warnungen.
 * - Verilator (WIDTHEXPAND) kompatibel durch striktes 7-Bit Typecasting.
 */

module baseband_sharpener (
    input  wire clk,                  
    input  wire rst,                  
    input  wire [31:0] rate_config,   
    input  wire valid_in,             
    input  wire signed [15:0] i_in,   
    input  wire signed [15:0] q_in,
    output reg  signed [15:0] i_out,  
    output reg  signed [15:0] q_out,
    output reg                valid_out 
);

    reg [1:0] rate_sel;
    always @(*) begin
        if (rate_config > 32'h0500_0000)      rate_sel = 2'b10;
        else if (rate_config > 32'h0200_0000) rate_sel = 2'b01;
        else                                  rate_sel = 2'b00;
    end

    // ROM Adresse synchronisieren (verhindert asynchrone ROM Reads)
    reg [1:0] rate_sel_reg;
    always @(posedge clk) begin
        if (rst) rate_sel_reg <= 2'b00;
        else     rate_sel_reg <= rate_sel;
    end

    // =========================================================================
    // 1. DISKRETE RINGPUFFER (M9K Inference)
    // =========================================================================
    // WARUM SO VIELE ARRAYS? 
    // Ein FIR-Filter benötigt gleichzeitigen Zugriff auf viele vergangene Samples.
    // Ein einzelnes Block-RAM (BRAM) hat aber maximal 2 Lese-Ports (Dual-Port).
    // Anstatt teure Logikzellen (LUTs) für Schieberegister zu verschwenden, 
    // speichern wir die eingehenden Samples parallel in 6 unabhängige BRAM-Blöcke.
    // So können wir in einem einzigen Taktzyklus 12 Werte (2 pro Block) auslesen 
    // und multiplizieren! Das spart massiv DSPs und Logik.
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_0 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_0 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_1 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_1 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_2 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_2 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_3 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_3 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_4 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_4 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_i_a_5 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_i_b_5 [0:127];

    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_0 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_0 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_1 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_1 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_2 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_2 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_3 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_3 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_4 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_4 [0:127];
    (* ramstyle = "M9K" *) reg signed [15:0] ram_q_a_5 [0:127]; (* ramstyle = "M9K" *) reg signed [15:0] ram_q_b_5 [0:127];

    // =========================================================================
    // 1a. STEUERLOGIK (Mit Reset)
    // =========================================================================
    reg [6:0] wr_ptr;
    reg [3:0] tdm_count;

    always @(posedge clk) begin
        if (rst) begin
            wr_ptr    <= 7'd0;
            tdm_count <= 4'd0;
        end else begin
            if (valid_in) begin
                wr_ptr    <= (wr_ptr == 7'd119) ? 7'd0 : wr_ptr + 7'd1;
                tdm_count <= 4'd0;
            end else if (tdm_count == 4'd9) begin
                tdm_count <= 4'd0;
            end else begin
                tdm_count <= tdm_count + 4'd1;
            end
        end
    end

    // =========================================================================
    // 1b. REINER RAM-SCHREIBZUGRIFF (Absolut KEIN Reset im Pfad!)
    // =========================================================================
    // Da valid_in als Write-Enable fungiert, wird bei Reset ohnehin nicht geschrieben.
    // Falls doch, stört es nicht, da wr_ptr zurückgesetzt wird. Das garantiert M9K!
    always @(posedge clk) begin
        if (valid_in) begin
            ram_i_a_0[wr_ptr] <= i_in; ram_i_b_0[wr_ptr] <= i_in; ram_q_a_0[wr_ptr] <= q_in; ram_q_b_0[wr_ptr] <= q_in;
            ram_i_a_1[wr_ptr] <= i_in; ram_i_b_1[wr_ptr] <= i_in; ram_q_a_1[wr_ptr] <= q_in; ram_q_b_1[wr_ptr] <= q_in;
            ram_i_a_2[wr_ptr] <= i_in; ram_i_b_2[wr_ptr] <= i_in; ram_q_a_2[wr_ptr] <= q_in; ram_q_b_2[wr_ptr] <= q_in;
            ram_i_a_3[wr_ptr] <= i_in; ram_i_b_3[wr_ptr] <= i_in; ram_q_a_3[wr_ptr] <= q_in; ram_q_b_3[wr_ptr] <= q_in;
            ram_i_a_4[wr_ptr] <= i_in; ram_i_b_4[wr_ptr] <= i_in; ram_q_a_4[wr_ptr] <= q_in; ram_q_b_4[wr_ptr] <= q_in;
            ram_i_a_5[wr_ptr] <= i_in; ram_i_b_5[wr_ptr] <= i_in; ram_q_a_5[wr_ptr] <= q_in; ram_q_b_5[wr_ptr] <= q_in;
        end
    end

    // Adressberechnung mit striktem Type-Casting (Befriedigt Quartus & Verilator)
    reg [6:0] addr1_0, addr1_1, addr1_2, addr1_3, addr1_4, addr1_5;
    reg [6:0] addr2_0, addr2_1, addr2_2, addr2_3, addr2_4, addr2_5;
    
    wire [6:0] tdm_ext = {3'b000, tdm_count};
    
    wire [6:0] off_0 = tdm_ext;           wire [6:0] roff_0 = 7'd119 - off_0;
    wire [6:0] off_1 = 7'd10 + tdm_ext;   wire [6:0] roff_1 = 7'd119 - off_1;
    wire [6:0] off_2 = 7'd20 + tdm_ext;   wire [6:0] roff_2 = 7'd119 - off_2;
    wire [6:0] off_3 = 7'd30 + tdm_ext;   wire [6:0] roff_3 = 7'd119 - off_3;
    wire [6:0] off_4 = 7'd40 + tdm_ext;   wire [6:0] roff_4 = 7'd119 - off_4;
    wire [6:0] off_5 = 7'd50 + tdm_ext;   wire [6:0] roff_5 = 7'd119 - off_5;

    always @(*) begin
        addr1_0 = (wr_ptr > off_0) ? (wr_ptr - 7'd1 - off_0) : (wr_ptr + 7'd119 - off_0);
        addr2_0 = (wr_ptr > roff_0) ? (wr_ptr - 7'd1 - roff_0) : (wr_ptr + 7'd119 - roff_0);
        
        addr1_1 = (wr_ptr > off_1) ? (wr_ptr - 7'd1 - off_1) : (wr_ptr + 7'd119 - off_1);
        addr2_1 = (wr_ptr > roff_1) ? (wr_ptr - 7'd1 - roff_1) : (wr_ptr + 7'd119 - roff_1);
        
        addr1_2 = (wr_ptr > off_2) ? (wr_ptr - 7'd1 - off_2) : (wr_ptr + 7'd119 - off_2);
        addr2_2 = (wr_ptr > roff_2) ? (wr_ptr - 7'd1 - roff_2) : (wr_ptr + 7'd119 - roff_2);
        
        addr1_3 = (wr_ptr > off_3) ? (wr_ptr - 7'd1 - off_3) : (wr_ptr + 7'd119 - off_3);
        addr2_3 = (wr_ptr > roff_3) ? (wr_ptr - 7'd1 - roff_3) : (wr_ptr + 7'd119 - roff_3);
        
        addr1_4 = (wr_ptr > off_4) ? (wr_ptr - 7'd1 - off_4) : (wr_ptr + 7'd119 - off_4);
        addr2_4 = (wr_ptr > roff_4) ? (wr_ptr - 7'd1 - roff_4) : (wr_ptr + 7'd119 - roff_4);
        
        addr1_5 = (wr_ptr > off_5) ? (wr_ptr - 7'd1 - off_5) : (wr_ptr + 7'd119 - off_5);
        addr2_5 = (wr_ptr > roff_5) ? (wr_ptr - 7'd1 - roff_5) : (wr_ptr + 7'd119 - roff_5);
    end

    // =========================================================================
    // 2. DISKRETE KOEFFIZIENTEN ROMs (Explizites Signed Type-Casting)
    // =========================================================================
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_0 [0:63];
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_1 [0:63];
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_2 [0:63];
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_3 [0:63];
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_4 [0:63];
    (* ramstyle = "M9K" *) reg signed [15:0] coeff_rom_5 [0:63];
    
    wire [5:0] rom_addr = {rate_sel_reg, tdm_count};

    initial begin
        // Init Rate 0 (Index 0-9)
        coeff_rom_0[0]=16'sd0;     coeff_rom_0[1]=16'sd1;     coeff_rom_0[2]=16'sd1;     coeff_rom_0[3]=16'sd1;     coeff_rom_0[4]=16'sd2;     coeff_rom_0[5]=16'sd2;     coeff_rom_0[6]=16'sd3;     coeff_rom_0[7]=16'sd3;     coeff_rom_0[8]=16'sd3;     coeff_rom_0[9]=16'sd4;
        coeff_rom_1[0]=16'sd4;     coeff_rom_1[1]=16'sd3;     coeff_rom_1[2]=16'sd2;     coeff_rom_1[3]=16'sd1;     coeff_rom_1[4]=-16'sd1;    coeff_rom_1[5]=-16'sd4;    coeff_rom_1[6]=-16'sd7;    coeff_rom_1[7]=-16'sd12;   coeff_rom_1[8]=-16'sd17;   coeff_rom_1[9]=-16'sd23;
        coeff_rom_2[0]=-16'sd30;   coeff_rom_2[1]=-16'sd38;   coeff_rom_2[2]=-16'sd47;   coeff_rom_2[3]=-16'sd56;   coeff_rom_2[4]=-16'sd66;   coeff_rom_2[5]=-16'sd75;   coeff_rom_2[6]=-16'sd83;   coeff_rom_2[7]=-16'sd91;   coeff_rom_2[8]=-16'sd96;   coeff_rom_2[9]=-16'sd100;
        coeff_rom_3[0]=-16'sd101;  coeff_rom_3[1]=-16'sd98;   coeff_rom_3[2]=-16'sd91;   coeff_rom_3[3]=-16'sd79;   coeff_rom_3[4]=-16'sd62;   coeff_rom_3[5]=-16'sd39;   coeff_rom_3[6]=-16'sd9;    coeff_rom_3[7]=16'sd27;    coeff_rom_3[8]=16'sd71;    coeff_rom_3[9]=16'sd121;
        coeff_rom_4[0]=16'sd179;   coeff_rom_4[1]=16'sd243;   coeff_rom_4[2]=16'sd314;   coeff_rom_4[3]=16'sd390;   coeff_rom_4[4]=16'sd472;   coeff_rom_4[5]=16'sd557;   coeff_rom_4[6]=16'sd646;   coeff_rom_4[7]=16'sd736;   coeff_rom_4[8]=16'sd826;   coeff_rom_4[9]=16'sd916;
        coeff_rom_5[0]=16'sd1002;  coeff_rom_5[1]=16'sd1085;  coeff_rom_5[2]=16'sd1162;  coeff_rom_5[3]=16'sd1232;  coeff_rom_5[4]=16'sd1293;  coeff_rom_5[5]=16'sd1346;  coeff_rom_5[6]=16'sd1387;  coeff_rom_5[7]=16'sd1418;  coeff_rom_5[8]=16'sd1436;  coeff_rom_5[9]=16'sd1442;
        
        // Init Rate 1 (Index 16-25)
        coeff_rom_0[16]=16'sd0;    coeff_rom_0[17]=-16'sd1;   coeff_rom_0[18]=-16'sd1;   coeff_rom_0[19]=-16'sd1;   coeff_rom_0[20]=-16'sd2;   coeff_rom_0[21]=-16'sd2;   coeff_rom_0[22]=-16'sd2;   coeff_rom_0[23]=-16'sd1;   coeff_rom_0[24]=16'sd0;    coeff_rom_0[25]=16'sd2;
        coeff_rom_1[16]=16'sd4;    coeff_rom_1[17]=16'sd7;    coeff_rom_1[18]=16'sd10;   coeff_rom_1[19]=16'sd13;   coeff_rom_1[20]=16'sd15;   coeff_rom_1[21]=16'sd15;   coeff_rom_1[22]=16'sd14;   coeff_rom_1[23]=16'sd9;    coeff_rom_1[24]=16'sd2;    coeff_rom_1[25]=-16'sd8;
        coeff_rom_2[16]=-16'sd21;  coeff_rom_2[17]=-16'sd34;  coeff_rom_2[18]=-16'sd47;  coeff_rom_2[19]=-16'sd58;  coeff_rom_2[20]=-16'sd64;  coeff_rom_2[21]=-16'sd64;  coeff_rom_2[22]=-16'sd56;  coeff_rom_2[23]=-16'sd39;  coeff_rom_2[24]=-16'sd13;  coeff_rom_2[25]=16'sd22;
        coeff_rom_3[16]=16'sd63;   coeff_rom_3[17]=16'sd106;  coeff_rom_3[18]=16'sd146;  coeff_rom_3[19]=16'sd179;  coeff_rom_3[20]=16'sd198;  coeff_rom_3[21]=16'sd198;  coeff_rom_3[22]=16'sd175;  coeff_rom_3[23]=16'sd127;  coeff_rom_3[24]=16'sd54;   coeff_rom_3[25]=-16'sd41;
        coeff_rom_4[16]=-16'sd152; coeff_rom_4[17]=-16'sd270; coeff_rom_4[18]=-16'sd382; coeff_rom_4[19]=-16'sd476; coeff_rom_4[20]=-16'sd536; coeff_rom_4[21]=-16'sd549; coeff_rom_4[22]=-16'sd502; coeff_rom_4[23]=-16'sd388; coeff_rom_4[24]=-16'sd201; coeff_rom_4[25]=16'sd58;
        coeff_rom_5[16]=16'sd383;  coeff_rom_5[17]=16'sd761;  coeff_rom_5[18]=16'sd1176; coeff_rom_5[19]=16'sd1606; coeff_rom_5[20]=16'sd2027; coeff_rom_5[21]=16'sd2415; coeff_rom_5[22]=16'sd2746; coeff_rom_5[23]=16'sd2998; coeff_rom_5[24]=16'sd3157; coeff_rom_5[25]=16'sd3211;
        
        // Init Rate 2 (Index 32-41)
        coeff_rom_0[32]=16'sd0;    coeff_rom_0[33]=16'sd0;    coeff_rom_0[34]=16'sd0;    coeff_rom_0[35]=16'sd1;    coeff_rom_0[36]=16'sd1;    coeff_rom_0[37]=16'sd0;    coeff_rom_0[38]=-16'sd2;   coeff_rom_0[39]=-16'sd4;   coeff_rom_0[40]=-16'sd4;   coeff_rom_0[41]=16'sd0;
        coeff_rom_1[32]=16'sd5;    coeff_rom_1[33]=16'sd9;    coeff_rom_1[34]=16'sd7;    coeff_rom_1[35]=-16'sd2;   coeff_rom_1[36]=-16'sd13;  coeff_rom_1[37]=-16'sd18;  coeff_rom_1[38]=-16'sd12;  coeff_rom_1[39]=16'sd6;    coeff_rom_1[40]=16'sd26;   coeff_rom_1[41]=16'sd32;
        coeff_rom_2[32]=16'sd17;   coeff_rom_2[33]=-16'sd17;  coeff_rom_2[34]=-16'sd48;  coeff_rom_2[35]=-16'sd53;  coeff_rom_2[36]=-16'sd20;  coeff_rom_2[37]=16'sd36;   coeff_rom_2[38]=16'sd81;   coeff_rom_2[39]=16'sd80;   coeff_rom_2[40]=16'sd20;   coeff_rom_2[41]=-16'sd70;
        coeff_rom_3[32]=-16'sd131; coeff_rom_3[33]=-16'sd113; coeff_rom_3[34]=-16'sd10;  coeff_rom_3[35]=16'sd124;  coeff_rom_3[36]=16'sd201;  coeff_rom_3[37]=16'sd152;  coeff_rom_3[38]=-16'sd15;  coeff_rom_3[39]=-16'sd208; coeff_rom_3[40]=-16'sd296; coeff_rom_3[41]=-16'sd193;
        coeff_rom_4[32]=16'sd68;   coeff_rom_4[33]=16'sd337;  coeff_rom_4[34]=16'sd427;  coeff_rom_4[35]=16'sd234;  coeff_rom_4[36]=-16'sd166; coeff_rom_4[37]=-16'sd539; coeff_rom_4[38]=-16'sd616; coeff_rom_4[39]=-16'sd272; coeff_rom_4[40]=16'sd352;  coeff_rom_4[41]=16'sd886;
        coeff_rom_5[32]=16'sd927;  coeff_rom_5[33]=16'sd302;  coeff_rom_5[34]=-16'sd756; coeff_rom_5[35]=-16'sd1640;coeff_rom_5[36]=-16'sd1641;coeff_rom_5[37]=-16'sd321; coeff_rom_5[38]=16'sd2194; coeff_rom_5[39]=16'sd5181; coeff_rom_5[40]=16'sd7595; coeff_rom_5[41]=16'sd8519;
    end

    // =========================================================================
    // 3. PIPELINE STUFE 1 (Synchroner Read OHNE kombinatorische Addierer)
    // =========================================================================
    reg signed [15:0] ram_i_a_0_out, ram_i_a_1_out, ram_i_a_2_out, ram_i_a_3_out, ram_i_a_4_out, ram_i_a_5_out;
    reg signed [15:0] ram_i_b_0_out, ram_i_b_1_out, ram_i_b_2_out, ram_i_b_3_out, ram_i_b_4_out, ram_i_b_5_out;
    reg signed [15:0] ram_q_a_0_out, ram_q_a_1_out, ram_q_a_2_out, ram_q_a_3_out, ram_q_a_4_out, ram_q_a_5_out;
    reg signed [15:0] ram_q_b_0_out, ram_q_b_1_out, ram_q_b_2_out, ram_q_b_3_out, ram_q_b_4_out, ram_q_b_5_out;
    reg signed [15:0] coeff_0_out,   coeff_1_out,   coeff_2_out,   coeff_3_out,   coeff_4_out,   coeff_5_out;
    
    reg [3:0] pipe_count_q1;

    always @(posedge clk) begin
        if (rst) pipe_count_q1 <= 4'd0;
        else     pipe_count_q1 <= tdm_count;
        
        // Direkte Zuweisung (Garantiert M9K Output Register Mapping)
        ram_i_a_0_out <= ram_i_a_0[addr1_0]; ram_i_b_0_out <= ram_i_b_0[addr2_0];
        ram_i_a_1_out <= ram_i_a_1[addr1_1]; ram_i_b_1_out <= ram_i_b_1[addr2_1];
        ram_i_a_2_out <= ram_i_a_2[addr1_2]; ram_i_b_2_out <= ram_i_b_2[addr2_2];
        ram_i_a_3_out <= ram_i_a_3[addr1_3]; ram_i_b_3_out <= ram_i_b_3[addr2_3];
        ram_i_a_4_out <= ram_i_a_4[addr1_4]; ram_i_b_4_out <= ram_i_b_4[addr2_4];
        ram_i_a_5_out <= ram_i_a_5[addr1_5]; ram_i_b_5_out <= ram_i_b_5[addr2_5];

        ram_q_a_0_out <= ram_q_a_0[addr1_0]; ram_q_b_0_out <= ram_q_b_0[addr2_0];
        ram_q_a_1_out <= ram_q_a_1[addr1_1]; ram_q_b_1_out <= ram_q_b_1[addr2_1];
        ram_q_a_2_out <= ram_q_a_2[addr1_2]; ram_q_b_2_out <= ram_q_b_2[addr2_2];
        ram_q_a_3_out <= ram_q_a_3[addr1_3]; ram_q_b_3_out <= ram_q_b_3[addr2_3];
        ram_q_a_4_out <= ram_q_a_4[addr1_4]; ram_q_b_4_out <= ram_q_b_4[addr2_4];
        ram_q_a_5_out <= ram_q_a_5[addr1_5]; ram_q_b_5_out <= ram_q_b_5[addr2_5];

        coeff_0_out <= coeff_rom_0[rom_addr]; coeff_1_out <= coeff_rom_1[rom_addr];
        coeff_2_out <= coeff_rom_2[rom_addr]; coeff_3_out <= coeff_rom_3[rom_addr];
        coeff_4_out <= coeff_rom_4[rom_addr]; coeff_5_out <= coeff_rom_5[rom_addr];
    end

    // =========================================================================
    // 4. PIPELINE STUFE 2 (MAC Multiplikation)
    // =========================================================================
    // Kombinatorische Addition und Multiplikation vor dem Akkumulator
    /* verilator lint_off WIDTHEXPAND */
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_0 = (ram_i_a_0_out + ram_i_b_0_out) * coeff_0_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_0 = (ram_q_a_0_out + ram_q_b_0_out) * coeff_0_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_1 = (ram_i_a_1_out + ram_i_b_1_out) * coeff_1_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_1 = (ram_q_a_1_out + ram_q_b_1_out) * coeff_1_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_2 = (ram_i_a_2_out + ram_i_b_2_out) * coeff_2_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_2 = (ram_q_a_2_out + ram_q_b_2_out) * coeff_2_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_3 = (ram_i_a_3_out + ram_i_b_3_out) * coeff_3_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_3 = (ram_q_a_3_out + ram_q_b_3_out) * coeff_3_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_4 = (ram_i_a_4_out + ram_i_b_4_out) * coeff_4_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_4 = (ram_q_a_4_out + ram_q_b_4_out) * coeff_4_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_i_5 = (ram_i_a_5_out + ram_i_b_5_out) * coeff_5_out;
    (* multstyle = "dsp" *) wire signed [32:0] prod_q_5 = (ram_q_a_5_out + ram_q_b_5_out) * coeff_5_out;
    /* verilator lint_on WIDTHEXPAND */

    reg signed [39:0] acc_i_0, acc_i_1, acc_i_2, acc_i_3, acc_i_4, acc_i_5;
    reg signed [39:0] acc_q_0, acc_q_1, acc_q_2, acc_q_3, acc_q_4, acc_q_5;
    reg [3:0] pipe_count_q2;

    always @(posedge clk) begin
        if (rst) begin
            pipe_count_q2 <= 4'd0;
            acc_i_0 <= 40'sd0; acc_q_0 <= 40'sd0;
            acc_i_1 <= 40'sd0; acc_q_1 <= 40'sd0;
            acc_i_2 <= 40'sd0; acc_q_2 <= 40'sd0;
            acc_i_3 <= 40'sd0; acc_q_3 <= 40'sd0;
            acc_i_4 <= 40'sd0; acc_q_4 <= 40'sd0;
            acc_i_5 <= 40'sd0; acc_q_5 <= 40'sd0;
        end else begin
            pipe_count_q2 <= pipe_count_q1;
            if (pipe_count_q1 == 4'd0) begin
                acc_i_0 <= $signed({{7{prod_i_0[32]}}, prod_i_0}); acc_q_0 <= $signed({{7{prod_q_0[32]}}, prod_q_0});
                acc_i_1 <= $signed({{7{prod_i_1[32]}}, prod_i_1}); acc_q_1 <= $signed({{7{prod_q_1[32]}}, prod_q_1});
                acc_i_2 <= $signed({{7{prod_i_2[32]}}, prod_i_2}); acc_q_2 <= $signed({{7{prod_q_2[32]}}, prod_q_2});
                acc_i_3 <= $signed({{7{prod_i_3[32]}}, prod_i_3}); acc_q_3 <= $signed({{7{prod_q_3[32]}}, prod_q_3});
                acc_i_4 <= $signed({{7{prod_i_4[32]}}, prod_i_4}); acc_q_4 <= $signed({{7{prod_q_4[32]}}, prod_q_4});
                acc_i_5 <= $signed({{7{prod_i_5[32]}}, prod_i_5}); acc_q_5 <= $signed({{7{prod_q_5[32]}}, prod_q_5});
            end else begin
                acc_i_0 <= acc_i_0 + $signed({{7{prod_i_0[32]}}, prod_i_0}); acc_q_0 <= acc_q_0 + $signed({{7{prod_q_0[32]}}, prod_q_0});
                acc_i_1 <= acc_i_1 + $signed({{7{prod_i_1[32]}}, prod_i_1}); acc_q_1 <= acc_q_1 + $signed({{7{prod_q_1[32]}}, prod_q_1});
                acc_i_2 <= acc_i_2 + $signed({{7{prod_i_2[32]}}, prod_i_2}); acc_q_2 <= acc_q_2 + $signed({{7{prod_q_2[32]}}, prod_q_2});
                acc_i_3 <= acc_i_3 + $signed({{7{prod_i_3[32]}}, prod_i_3}); acc_q_3 <= acc_q_3 + $signed({{7{prod_q_3[32]}}, prod_q_3});
                acc_i_4 <= acc_i_4 + $signed({{7{prod_i_4[32]}}, prod_i_4}); acc_q_4 <= acc_q_4 + $signed({{7{prod_q_4[32]}}, prod_q_4});
                acc_i_5 <= acc_i_5 + $signed({{7{prod_i_5[32]}}, prod_i_5}); acc_q_5 <= acc_q_5 + $signed({{7{prod_q_5[32]}}, prod_q_5});
            end
        end
    end

    // =========================================================================
    // 5. PIPELINE STUFE 3 (Endsumme)
    // =========================================================================
    reg signed [42:0] final_sum_i, final_sum_q;
    reg               out_valid_reg;
    reg signed [42:0] sum_i_comb, sum_q_comb;

    always @(*) begin
        sum_i_comb = $signed({{3{acc_i_0[39]}}, acc_i_0}) + $signed({{3{acc_i_1[39]}}, acc_i_1}) +
                     $signed({{3{acc_i_2[39]}}, acc_i_2}) + $signed({{3{acc_i_3[39]}}, acc_i_3}) +
                     $signed({{3{acc_i_4[39]}}, acc_i_4}) + $signed({{3{acc_i_5[39]}}, acc_i_5});
                     
        sum_q_comb = $signed({{3{acc_q_0[39]}}, acc_q_0}) + $signed({{3{acc_q_1[39]}}, acc_q_1}) +
                     $signed({{3{acc_q_2[39]}}, acc_q_2}) + $signed({{3{acc_q_3[39]}}, acc_q_3}) +
                     $signed({{3{acc_q_4[39]}}, acc_q_4}) + $signed({{3{acc_q_5[39]}}, acc_q_5});
    end

    always @(posedge clk) begin
        if (rst) begin
            final_sum_i <= 43'sd0; final_sum_q <= 43'sd0; out_valid_reg <= 1'b0;
        end else begin
            if (pipe_count_q2 == 4'd9) begin
                final_sum_i <= sum_i_comb; final_sum_q <= sum_q_comb; out_valid_reg <= 1'b1;
            end else begin
                out_valid_reg <= 1'b0;
            end
        end
    end

    // =========================================================================
    // 6. PIPELINE STUFE 4 (Rundung & Sättigung)
    // =========================================================================
    wire signed [42:0] rounded_sum_i = final_sum_i + 43'sd16384; 
    wire signed [42:0] rounded_sum_q = final_sum_q + 43'sd16384;

    wire signed [42:0] shifted_i = rounded_sum_i >>> 15;
    wire signed [42:0] shifted_q = rounded_sum_q >>> 15;

    always @(posedge clk) begin
        if (rst) begin
            i_out <= 16'sd0; q_out <= 16'sd0; valid_out <= 1'b0;
        end else begin
            valid_out <= out_valid_reg;
            if (out_valid_reg) begin
                if (shifted_i > 43'sd32767)       i_out <= 16'sd32767;
                else if (shifted_i < -43'sd32768) i_out <= -16'sd32768;
                else                              i_out <= shifted_i[15:0];

                if (shifted_q > 43'sd32767)       q_out <= 16'sd32767;
                else if (shifted_q < -43'sd32768) q_out <= -16'sd32768;
                else                              q_out <= shifted_q[15:0];
            end
        end
    end

endmodule
