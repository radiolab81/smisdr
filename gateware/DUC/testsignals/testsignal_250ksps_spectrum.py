import numpy as np
import matplotlib.pyplot as plt

def plot_iq_spectrum(filename="testsignal_250ksps_ci16.iq", fs=0.25e6):
    print(f"Lese Datei '{filename}' ein...")
    
    # --- 1. Daten einlesen ---
    # Die Daten liegen als rohe 16-Bit Integer vor
    raw_data = np.fromfile(filename, dtype=np.int16)
    
    # --- NEU: Hardware-Dekodierung (In-Band Signaling entfernen) ---
    # 1. Tags extrahieren (die obersten beiden Bits)
    # Bitweise Shiften und mit 0x03 (Binär 11) maskieren
    tags = np.bitwise_and(np.right_shift(raw_data, 14), 0x03)
    
    # 2. 14-Bit Payload wieder in korrekte Vorzeichen (Sign-Extension) wandeln
    # Der FPGA macht das mit { {2{data_sync[13]}}, data_sync[13:0] }.
    # In NumPy geht das am elegantesten so: 2 Bits nach links shiften (löscht die Tags,
    # schiebt das Vorzeichen an die 16-Bit-Grenze) und dann arithmetisch 2 Bits nach rechts.
    payload_signed = np.right_shift(np.left_shift(raw_data, 2), 2)
    
    # --- 2. De-Interleaving ---
    # Gerade Indizes (0, 2, 4...) sind Inphase (I)
    # Ungerade Indizes (1, 3, 5...) sind Quadratur (Q)
    i_data = payload_signed[0::2]
    q_data = payload_signed[1::2]
    
    i_tags = tags[0::2]
    q_tags = tags[1::2]
    
    # --- NEU: Hardware-Tag Check ---
    print("\n" + "="*40)
    print(" IN-BAND SIGNALING CHECK (Tags)")
    print("="*40)
    i_tag_errors = np.sum(i_tags != 0) # I sollte immer 00 sein
    q_tag_errors = np.sum(q_tags != 1) # Q sollte immer 01 sein
    
    if i_tag_errors == 0 and q_tag_errors == 0:
        print("✅ ERFOLG: Alle I/Q-Tags sind hardwarekonform gesetzt (I=00, Q=01).")
    else:
        print(f"❌ FEHLER: Falsche Tags gefunden! I-Fehler: {i_tag_errors}, Q-Fehler: {q_tag_errors}")

    # --- 3. Statistiken & 14-Bit Aussteuerung prüfen ---
    print("\n" + "="*40)
    print(" SIGNAL-STATISTIKEN (DSP-Payload getrennt)")
    print("="*40)
    
    # Hilfsfunktion für die Berechnung und Ausgabe
    def print_stats(name, data):
        d_min = np.min(data)
        d_max = np.max(data)
        d_mean = np.mean(data)
        d_std = np.std(data) # Standardabweichung
        print(f"[{name}-Zweig] Aussteuerung:")
        print(f"  Min: {d_min:7d}  |  Max: {d_max:7d}")
        print(f"  Mittelwert: {d_mean:7.2f}  |  Abweichung (StdDev): {d_std:7.2f}\n")
        return d_min, d_max
        
    print_stats("I", i_data)
    print_stats("Q", q_data)
    
    # --- 14-Bit Check für die decodierte Payload ---
    LIMIT_MIN = -8192
    LIMIT_MAX = 8191
    
    # Zähle alle Samples, die den 14-Bit Bereich verlassen
    i_overflows = np.sum((i_data < LIMIT_MIN) | (i_data > LIMIT_MAX))
    q_overflows = np.sum((q_data < LIMIT_MIN) | (q_data > LIMIT_MAX))
    total_overflows = i_overflows + q_overflows
    
    print("--- 14-BIT PAYLOAD OVERFLOW CHECK ---")
    if total_overflows == 0:
        print("✅ ERFOLG: Die decodierte Payload passt exakt in 14 Bit.")
        print("   Das Basisbandsignal ist unbeschädigt.")
    else:
        print("❌ FEHLER: Werte überschreiten den 14-Bit Bereich!")
        print(f"   I-Zweig Overflows: {i_overflows}")
        print(f"   Q-Zweig Overflows: {q_overflows}")
    print("="*40 + "\n")
    
    # --- 4. Komplexes Basisbandsignal rekonstruieren ---
    # I ist der Realteil, Q der Imaginärteil
    complex_signal = i_data + 1j * q_data
    
    print(f"Erfolgreich {len(complex_signal)} reine DSP-Samples an die FFT übergeben.")

    # --- 5. Spektrum zeichnen ---
    plt.figure(figsize=(10, 6))
    nfft_len = 8192
    win = np.blackman(nfft_len) # Alternativ: np.hanning(nfft_len)
    # plt.psd nutzt Welch's Methode
    plt.psd(complex_signal, NFFT=nfft_len, Fs=fs, window=win, color='darkblue')
    
    # --- Optische Anpassungen ---
    plt.title("Spektrale Leistungsdichte der decodierten Payload (ci16)")
    plt.xlabel("Frequenz im Basisband (Hz)")
    plt.ylabel("Leistungsdichte (dB/Hz)")
    plt.grid(True, which="both", ls="--", alpha=0.7)
    
    # Die X-Achse auf die Nyquist-Bandbreite begrenzen (-fs/2 bis +fs/2)
    plt.xlim([-fs/2, fs/2])
    
    # Die y-Achse -40 bis 60dB begrenzen 
    plt.ylim([-40, 60])
    
    # Markierungen für unsere erwarteten Träger setzen
    expected_carriers = [-100e3, -75e3, -50e3, 0.0, 50e3, 75e3, 100e3]
    for carrier in expected_carriers:
        plt.axvline(x=carrier, color='red', linestyle=':', alpha=0.8)
        
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    plot_iq_spectrum()