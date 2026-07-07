/*
     * ==============================================================================
     * DUC Testbench - Parameterübersicht (Kommandozeilen-Argumente)
     * ==============================================================================
     *
     * Steuerung der Simulation über CLI-Parameter. Wenn keine Parameter übergeben 
     * werden, startet die Simulation mit den Standardwerten (1250 ksps, 2 MHz Shift).
     * * Aufrufbeispiele: 
     * ./Vduc_top --rate 500 --shift 1125000
     * ./Vduc_top --passthrough --pt-modulo 50
     *
     * Parameter:
     * ------------------------------------------------------------------------------
     * --passthrough     Aktiviert den Passthrough-Modus. 
     * - Es wird keine In-Band-Command-Phase (Shift/Rate) durch die TB gesendet, 
     *   diese muss bereits in der iq Datei einkodiert sein.
     * - Eingangsdatei ist fest: testbench_passthrough_ci16.iq. -> 
     *   Realworldscenario erzeugbar mit cohi_wav_to_smi_iq.py (parsing von SR und SHIFT aus 
     *   aus WAV HEADER, filename...)
     * - Liest die fertigen 16-Bit-Protokollwörter direkt aus der Datei.
     *
     * --rate <ksps>     Setzt die Eingangs-Samplerate in ksps (Standard: 1250).
     * Typische Werte: 1250, 500, 250.
     * - Sucht automatisch die passende Eingangsdatei 
     * (z. B. testsignal_1250ksps_ci16.iq).
     * - Setzt das korrekte Takt-Modulo (20 für 1250, 50 für 500, etc.).
     * - Berechnet das 32-Bit Resampler-Wort für das In-Band-Signaling.
     *
     * --shift <Hz>      Setzt den NCO Frequenz-Shift in Hertz (Standard: 2000000).
     * - Berechnet automatisch das 32-Bit FTW (Frequency Tuning Word) 
     * bezogen auf einen 50 MHz Master-Takt.
     * - Wird während der Initialisierung via In-Band-Command ('S') gesendet.
     *
     * --pt-modulo <N>   Überschreibt das Takt-Modulo (Wartezyklen zwischen Wörtern) 
     * exklusiv für den Passthrough-Modus (Standardwert: 100), genauer Wert
     * wird in cohi_wav_to_smi_iq.py vorgeschlagen
     * ==============================================================================
     */

#include "Vduc_top.h"
#include "verilated.h"
#include "verilated_vcd_c.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <cmath>
#include <iomanip>

// --- Hilfsfunktion zum Generieren der In-Band-Command-Wörter ---
uint16_t make_cmd_word(uint8_t ctrl, uint8_t index, uint8_t payload) {
    // Bit [15:14] = ctrl, Bit [13:8] = index, Bit [7:0] = payload
    return ((uint16_t)(ctrl & 0x03) << 14) | ((uint16_t)(index & 0x3F) << 8) | payload;
}

// --- Hilfsfunktionen für die 32-Bit Registerberechnung ---
uint32_t calculate_tuning_word(double target_freq, double clock_hz = 50.0e6) {
    double tw = (target_freq / clock_hz) * 4294967296.0; // 2^32
    return static_cast<uint32_t>(std::round(tw));
}

// --- zentrale Konfigurations-Struktur ---
struct SimConfig {
    bool passthrough = false;
    std::string iq_filename;
    int clock_modulo = 20; 
    uint32_t target_rate_word = 0;
    uint32_t target_ftw = 0;
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    
    // -------------------------------------------------------------------------
    // 1. CLI PARSING & KONFIGURATION
    // -------------------------------------------------------------------------
    SimConfig config;
    double shift_freq = 2.0e6; // Default: 2 MHz
    int rate_ksps = 1250;      // Default: 1250 ksps
    int pt_modulo =20;         // Default Modulo für Passthrough (z.B. für 1250ksps)

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--passthrough") {
            config.passthrough = true;
        } else if (arg == "--rate" && i + 1 < argc) {
            rate_ksps = std::stoi(argv[++i]);
        } else if (arg == "--shift" && i + 1 < argc) {
            shift_freq = std::stod(argv[++i]);
        } else if (arg == "--pt-modulo" && i + 1 < argc) {
            pt_modulo = std::stoi(argv[++i]); // Manuelles Überschreiben für Passthrough
        }
    }

    if (config.passthrough) {
        config.iq_filename = "./testsignals/testbench_passthrough_ci16.iq"; // zum Beispiel aus cohi_wav_to_smi.iq.py
        config.clock_modulo = pt_modulo; 
    } else {
        config.iq_filename = "./testsignals/testsignal_" + std::to_string(rate_ksps) + "ksps_ci16.iq"; // aus den gen_testsig.....py
        
        switch (rate_ksps) {
            case 1250: config.clock_modulo = 20;  break;
            case 500:  config.clock_modulo = 50;  break;
            case 250:  config.clock_modulo = 100; break;
            default: 
                std::cerr << "Warnung: Unbekannte Samplerate (" << rate_ksps 
                          << " ksps). Setze Fallback-Modulo auf 100." << std::endl;
                config.clock_modulo = 100;
                break;
        }
        
        config.target_rate_word = calculate_tuning_word(rate_ksps * 1000.0);
        config.target_ftw = calculate_tuning_word(shift_freq);
    }

    std::cout << "=== DUC Simulation Start ===" << std::endl;
    std::cout << "Modus:      " << (config.passthrough ? "Passthrough" : "Normal") << std::endl;
    std::cout << "Eingang:    " << config.iq_filename << std::endl;
    std::cout << "Modulo:     " << config.clock_modulo << " Takte" << std::endl;
    if (!config.passthrough) {
        std::cout << "Rate-Word:  0x" << std::hex << std::setw(8) << std::setfill('0') << config.target_rate_word << std::dec << std::endl;
        std::cout << "Shift-FTW:  0x" << std::hex << std::setw(8) << std::setfill('0') << config.target_ftw << std::dec << std::endl;
    }
    std::cout << "============================" << std::endl;

    // -------------------------------------------------------------------------
    // 2. IN-BAND SIGNALING VORBEREITEN
    // -------------------------------------------------------------------------
    std::vector<uint16_t> cmd_sequence;
    if (!config.passthrough) {
        cmd_sequence = {
            // 1. KOMMANDO: NCO Frequenz-Shift ('S')
            make_cmd_word(2, 63, 'S'),
            make_cmd_word(2,  0, (config.target_ftw & 0xFF)),
            make_cmd_word(2,  1, ((config.target_ftw >> 8) & 0xFF)),  
            make_cmd_word(2,  2, ((config.target_ftw >> 16) & 0xFF)), 
            make_cmd_word(2,  3, ((config.target_ftw >> 24) & 0xFF)),
            make_cmd_word(3,  0, 'E'),

            // 2. KOMMANDO: Eingangs-Samplerate ('R')
            make_cmd_word(2, 63, 'R'),
            make_cmd_word(2,  0, (config.target_rate_word & 0xFF)),
            make_cmd_word(2,  1, ((config.target_rate_word >> 8) & 0xFF)), 
            make_cmd_word(2,  2, ((config.target_rate_word >> 16) & 0xFF)), 
            make_cmd_word(2,  3, ((config.target_rate_word >> 24) & 0xFF)),
            make_cmd_word(3,  0, 'E')
        };
    }
    size_t cmd_ptr = 0;
    bool cmd_phase = !config.passthrough; // Im Passthrough-Modus überspringen wir Phase A

    // -------------------------------------------------------------------------
    // 3. DATEIEN & SIMULATION INITIALISIEREN
    // -------------------------------------------------------------------------
    std::ifstream iq_in(config.iq_filename, std::ios::binary);
    std::ofstream rf_out("rf.out", std::ios::binary); 

    if (!iq_in.is_open()) {
        std::cerr << "Kritischer Fehler: Eingangsdatei '" << config.iq_filename << "' nicht gefunden!" << std::endl;
        return -1;
    }
    if (!rf_out.is_open()) {
        std::cerr << "Kritischer Fehler: Ausgangsdatei 'rf.out' konnte nicht erstellt werden!" << std::endl;
        return -1;
    }

    Vduc_top* top = new Vduc_top;
    Verilated::traceEverOn(true);
    VerilatedVcdC* tfp = new VerilatedVcdC;
    top->trace(tfp, 99);
    tfp->open("waveform.vcd");

    vluint64_t main_time = 0;
    int clock_counter = 0;
    bool is_q_sample = false;
    
    // Reset initialisieren (invertiertes Reset)
    top->rst = 1; 
    top->clk = 0;
    top->data_en = 0;
    top->data_in = 0;

    // -------------------------------------------------------------------------
    // 4. HAUPTSCHLEIFE
    // -------------------------------------------------------------------------
    while (!iq_in.eof()) {
        
        // ---------------------------------------------------------------------
        // FALLING EDGE (Takt fällt)
        // ---------------------------------------------------------------------
        top->clk = 0;

        if (main_time > 40) {
            top->rst = 0; // Reset lösen, Dev-Board nutzt ggf. einen inv. Reset, bitte beachten
        }

        // Datenübergabe-Steuerung (Alle 20 Takte bei 50 MHz Master-Clock = 2.5 MSPS Bus-Wortrate, da I/Q interleaved angeliefert wird)
        if (!top->rst && clock_counter == 0) {
            
            if (config.passthrough) {
                // REINER PASSTHROUGH-MODUS (liest Hardware-Wort direkt inkl. Protokoll-Bits)
                uint16_t protocol_word = 0;
                if (iq_in.read(reinterpret_cast<char*>(&protocol_word), sizeof(protocol_word))) {
                    top->data_in = protocol_word; // Unverändert durchreichen! (Bit 15/14 steuern das Protokoll)
                    top->data_en = 1;
                } else {
                    break; // EOF
                }
            } else {
                // NORMALER MODUS (Kommando-Phase oder I/Q-Streaming)
                if (cmd_phase) {
                    // --- PHASE A: Erst die Konfigurations-Kommandos einspeisen ---
                    if (cmd_ptr < cmd_sequence.size()) {
                        top->data_in = cmd_sequence[cmd_ptr];
                        top->data_en = 1;
                        cmd_ptr++;
                    } else {
                        cmd_phase = false; // Kommandos fertig, wechsle zu IQ-Streaming
                    }
                } 
                
                if (!cmd_phase) {
                    // Phase B: I/Q Streaming aus 14-Bit-Nutzdaten
                    int16_t raw_sample = 0;
                    if (iq_in.read(reinterpret_cast<char*>(&raw_sample), sizeof(raw_sample))) {
                        
                        // nur für den Fall das es noch ECHTE 16 Bit Samples sind
                        // 16-Bit Signed des Python-Scripts auf 14-Bit herunterskalieren (Arithmetic Shift)
                        //int16_t sample_14bit = raw_sample >> 2;
                        //uint16_t protocol_word = (uint16_t)sample_14bit & 0x3FFF; // Maske für Bit [13:0]

                        // I/Q Samples kommen nun schon als 14 Bit aus Client an und lassen Platz für das In-Band Signaling
                        uint16_t protocol_word = (uint16_t)raw_sample & 0x3FFF; // Maske für Bit [13:0]

                        if (is_q_sample) {
                            protocol_word |= (1 << 14); // Bit 14 setzen für Q-Sample (01)
                            is_q_sample = false;
                        } else {
                            // Bit [15:14] bleiben 00 für I-Sample
                            is_q_sample = true; 
                        }

                        top->data_in = protocol_word;
                        top->data_en = 1;
                    } else {
                        break; // EOF der Eingangsdatei erreicht
                    }
                }
            }
        }

        top->eval();
        if (main_time < 100000) tfp->dump(main_time); // Dateigrößenschutz für VCD
        main_time++;

        // ---------------------------------------------------------------------
        // RISING EDGE (Takt steigt)
        // ---------------------------------------------------------------------
        top->clk = 1;
        top->eval();

        // EMV / GLITCH FILTER (smi_rx_16 Bit Protokollempfänger)
        // Strobe erst nach 5 Taktzyklen wieder wegnehmen
        if (!top->rst) {
           if (clock_counter == 5) { 
             top->data_en = 0;
           }       
        }

        // Ausgangsdaten des Mischers/DAC abgreifen (sobald Reset inaktiv ist)
        if (!top->rst) {
            int16_t rf_sample = (int16_t)top->dac_out; 
            rf_out.write(reinterpret_cast<const char*>(&rf_sample), sizeof(rf_sample));
        }

        if (main_time < 100000) tfp->dump(main_time); // Dateigrößenschutz für VCD
        main_time++;

        // Taktschrittzähler inkrementieren
        if (!top->rst) {
            clock_counter = (clock_counter + 1) % config.clock_modulo;
        }
    }

    std::cout << "Simulation erfolgreich beendet." << std::endl;
    std::cout << "Verarbeitete Samples/Takte: " << (main_time / 2) << std::endl;

    tfp->close();
    iq_in.close();
    rf_out.close();

    delete top;
    delete tfp;
    return 0;
}
