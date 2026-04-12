import socket
import numpy as np
import time

# Konfiguration
PI_IP = "127.0.0.1"  # <--- Hier die IP deines Raspberry Pi eintragen
PORT = 1234
SAMPLE_RATE = 5_000_000  # 5 MSPS
FREQ = 1_000_000         # 1 MHz Sinus
DURATION = 120            # Sekunden, die gestreamt werden sollen

def start_stream():
    # 1. Puffer für eine Periode oder einen Block vorbereiten
    # Wir nehmen einen Block, der exakt 1 Sekunde Signal entspricht
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    # Sinus erzeugen, skalieren auf 16-Bit (0 bis 65535)
    sine_wave = (32767 * np.sin(2 * np.pi * FREQ * t) + 32768).astype(np.uint16)
    
    # In Bytes umwandeln (Little Endian für den Pi)
    data_bytes = sine_wave.tobytes()

    print(f"Verbinde zu {PI_IP}:{PORT}...")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((PI_IP, PORT))
            print("Verbunden! Streame Daten...")
            
            start_time = time.time()
            while time.time() - start_time < DURATION:
                # Sende den 1-Sekunden-Block
                # TCP kümmert sich um das Buffering, wenn der Pi langsamer abnimmt
                s.sendall(data_bytes)
                
            print("Streaming beendet.")
    except Exception as e:
        print(f"Fehler: {e}")

if __name__ == "__main__":
    start_stream()
