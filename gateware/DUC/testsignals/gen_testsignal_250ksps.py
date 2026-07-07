import numpy as np

def generate_baseband_file():
    fs = 0.25e6           # Abtastrate (250 kHz)
    duration = 1.0        # 1 Sekunde
    num_samples = int(fs * duration)

    # 3 Träger generieren
    f1, f2, f3, f4, f5, f6, f7 = -100e3, -75e3, -50e3, 0.0, 50e3, 75e3, 100e3
    t = np.arange(num_samples) / fs
    
    amp = 0.1
    signal = (amp * np.exp(2j * np.pi * f1 * t) +
              amp * np.exp(2j * np.pi * f2 * t) +
              amp * np.exp(2j * np.pi * f3 * t) +
              amp * np.exp(2j * np.pi * f4 * t) +
              amp * np.exp(2j * np.pi * f5 * t) +
              amp * np.exp(2j * np.pi * f6 * t) +
              amp * np.exp(2j * np.pi * f7 * t))
    #signal = (amp * np.exp(2j * np.pi * f1 * t))    

    # --- 1. SICHERS CLIPPING AUF 14-BIT SIGNED RANGE ---
    # Ein 14-Bit Signed Integer geht von -8192 bis +8191.
    # Wir nutzen 8100 als sichere Amplitude, um Clipping-Verzerrungen zu vermeiden.
    i_signal = np.real(signal) * 8100
    q_signal = np.imag(signal) * 8100
    
    # Hartes Begrenzen, falls Floating-Point-Ungenauigkeiten auftreten
    i_scaled = np.clip(i_signal, -8192, 8191).astype(np.int16)
    q_scaled = np.clip(q_signal, -8192, 8191).astype(np.int16)

    # --- 2. HARDWARE PROTOKOLL-MASKIERUNG (In-Band Tagging) ---
    # Schritt A: Wir maskieren die unteren 14 Bit (0x3FFF), um die 
    # ungewollte 16-Bit Vorzeichenerweiterung (Sign-Extension) zu löschen.
    i_pure_14bit = i_scaled & 0x3FFF
    q_pure_14bit = q_scaled & 0x3FFF

    # Schritt B: Setzen der Steuerbits in Bit [15:14] laut Verilog-Parser
    # I-Sample bekommt '00' -> entspricht einer Verschiebung von 0x0000
    i_tagged = i_pure_14bit | (0b00 << 14) 
    
    # Q-Sample bekommt '01' -> entspricht einer Verschiebung von 0x4000
    q_tagged = q_pure_14bit | (0b01 << 14)

    # --- 3. Interleaving (I, Q, I, Q ...) ---
    iq_interleaved = np.empty(num_samples * 2, dtype=np.int16)
    iq_interleaved[0::2] = i_tagged
    iq_interleaved[1::2] = q_tagged

    # Binärdatei schreiben
    filename = "testsignal_250ksps_ci16.iq"
    with open(filename, "wb") as f:
        f.write(iq_interleaved.tobytes())

    print(f"Erfolg! Datei '{filename}' wurde hardwarekonform erstellt.")
    print(f"Format: 14-Bit Signed mit In-Band Multiplex-Tags (Bit 15:14)")

if __name__ == "__main__":
    generate_baseband_file()