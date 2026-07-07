import numpy as np
import wave
import os

def make_cmd_word(ctrl, index, payload):
    """
    Erstellt ein 16-Bit Command-Wort laut HDL/Testbench-Spezifikation:
    Bit [15:14] = ctrl, Bit [13:8] = index, Bit [7:0] = payload
    """
    return ((int(ctrl) & 0x03) << 14) | ((int(index) & 0x3F) << 8) | (int(payload) & 0xFF)

def calculate_ftw(frequency, clock_freq=50e6):
    """ Berechnet das 32-Bit Tuning Word basierend auf dem 50 MHz FPGA-Takt. """
    ftw = int(round((frequency / clock_freq) * (2**32)))
    return ftw & 0xFFFFFFFF

def extract_metadata_exact(filepath):
    """
    Parst den Dateinamen exakt nach der C++ Logik aus cohiplayer_smi_tui.cpp:
    Sucht nach 'kHz' und geht bis zum vorherigen Trennzeichen ('_' oder ' ') zurück.
    Liest die Samplerate direkt aus dem WAV-Header (fmt Chunk).
    """
    filename = os.path.basename(filepath)
    center_freq_hz = 0.0
    
    # Exakte C++ Logik-Abbildung
    khz_pos = filename.find("kHz")
    if khz_pos != -1:
        sub_str = filename[:khz_pos]
        # Finde das letzte Vorkommen von '_' oder ' ' vor 'kHz'
        start_idx = max(sub_str.rfind("_"), sub_str.rfind(" "))
        if start_idx == -1:
            start_idx = 0
        else:
            start_idx += 1
        try:
            center_freq_hz = float(sub_str[start_idx:]) * 1000.0
        except ValueError:
            print(f"Warnung: Konnte Frequenz aus Substring '{sub_str[start_idx:]}' nicht parsen.")

    # Samplerate aus WAV extrahieren
    with wave.open(filepath, 'rb') as wav:
        sample_rate_hz = wav.getframerate()
        channels = wav.getnchannels()
        width = wav.getsampwidth()
        
        if channels != 2 or width != 2:
            raise ValueError(f"Fehler: WAV muss Stereo und 16-Bit sein (Kanäle: {channels}, Breite: {width} Bytes).")
            
    return sample_rate_hz, center_freq_hz

def process_cohiradia_file(input_wav, output_iq, duration_sec=1.0):
    print(f"Analysiere Datei: {input_wav}")
    sample_rate, center_freq = extract_metadata_exact(input_wav)
    
    print(f"-> Extrahierte Samplerate: {sample_rate} Hz")
    print(f"-> Extrahierte Mittenfrequenz: {center_freq} Hz")
    
    # 1. Hardware-Parameter berechnen (Referenztakt 50 MHz)
    target_ftw = calculate_ftw(center_freq)
    target_rate = calculate_ftw(sample_rate)
    
    # Berechne den clock_counter Modulo-Wert für das Verilator-Timing
    # 50 MHz / (sample_rate * 2) -> mal 2, da I und Q nacheinander übertragen werden
    recommended_modulo = int(50e6 / (sample_rate * 2))
    print(f"-> Empfohlener Testbench-Modulo für diese Rate: {recommended_modulo}")

    # 2. In-Band Befehlssequenz aufbauen (Little Endian Bytes)
    cmd_sequence = [
        # Befehl 1: NCO Frequenz-Shift ('S')
        make_cmd_word(2, 63, ord('S')),
        make_cmd_word(2,  0, (target_ftw & 0xFF)),
        make_cmd_word(2,  1, ((target_ftw >> 8) & 0xFF)),
        make_cmd_word(2,  2, ((target_ftw >> 16) & 0xFF)),
        make_cmd_word(2,  3, ((target_ftw >> 24) & 0xFF)),
        make_cmd_word(3,  0, ord('E')),

        # Befehl 2: Eingangs-Samplerate ('R')
        make_cmd_word(2, 63, ord('R')),
        make_cmd_word(2,  0, (target_rate & 0xFF)),
        make_cmd_word(2,  1, ((target_rate >> 8) & 0xFF)),
        make_cmd_word(2,  2, ((target_rate >> 16) & 0xFF)),
        make_cmd_word(2,  3, ((target_rate >> 24) & 0xFF)),
        make_cmd_word(3,  0, ord('E'))
    ]

    # 3. WAV-Audiodaten für die gewünschte Dauer einlesen
    with wave.open(input_wav, 'rb') as wav:
        num_frames = int(sample_rate * duration_sec)
        num_frames = min(num_frames, wav.getnframes())
        raw_bytes = wav.readframes(num_frames)
        
    # Daten in 16-Bit Signed umwandeln (interleaved I, Q, I, Q)
    audio_samples = np.frombuffer(raw_bytes, dtype=np.int16)
    
    i_samples_16 = audio_samples[0::2]
    q_samples_16 = audio_samples[1::2]
    
    # 16-Bit WAV-Samples auf 14-Bit Signed herunterskalieren (Arithmetic Right Shift)
    i_scaled = np.right_shift(i_samples_16, 2)
    q_scaled = np.right_shift(q_samples_16, 2)
    
    # Untere 14 Bit maskieren, um ungewollte Vorzeichenerweiterungen im RAM-Array zu löschen
    i_pure = i_scaled & 0x3FFF
    q_pure = q_scaled & 0x3FFF
    
    # 4. In-Band Multiplex-Tags setzen (Bit [15:14])
    # I-Sample bekommt '00' -> Verschiebung bleibt bei 0
    i_tagged = i_pure | (0b00 << 14)
    # Q-Sample bekommt '01' -> Bit 14 wird gesetzt
    q_tagged = q_pure | (0b01 << 14)
    
    # Erneutes Interleaving der fertig getaggten Hardware-Wörter
    iq_interleaved = np.empty(len(i_tagged) * 2, dtype=np.uint16)
    iq_interleaved[0::2] = i_tagged
    iq_interleaved[1::2] = q_tagged

    # 5. Alles zusammen in die Ausgabedatei schreiben
    with open(output_iq, "wb") as f:
        # Zuerst das Befehlspaket schreiben
        cmd_array = np.array(cmd_sequence, dtype=np.uint16)
        f.write(cmd_array.tobytes())
        # Danach den Signalstrom anhängen
        f.write(iq_interleaved.tobytes())
        
    print(f"Erfolg! '{output_iq}' wurde generiert ({len(iq_interleaved)//2} I/Q-Paare geschrieben).\n")


if __name__ == "__main__":
    # BEISPIEL FÜR EINEN AUFRUF:
    # Der Dateiname MUSS das Frequenz-Muster enthalten, um korrekt geparst zu werden!
    input_file = "cohi_real_file_1250kHz.wav"
    #input_file = "cohi_real_file_6090kHz.wav"
    #input_file = "cohi_real_file_198kHz.wav"
    
    output_file = "testbench_passthrough_ci16.iq"
    
    # Dummy-Datei generieren, falls man direkt lokal testen willst
    if not os.path.exists(input_file):
        print(f"Erstelle temporäre Testdatei '{input_file}'...")
        with wave.open(input_file, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(250000) # 250 ksps
            dummy = (np.sin(np.linspace(0, 2*np.pi*1000, 500000)) * 20000).astype(np.int16)
            w.writeframes(dummy.tobytes())
            
    # Verarbeite genau 1 Sekunde Signal
    process_cohiradia_file(input_file, output_file, duration_sec=1.0)