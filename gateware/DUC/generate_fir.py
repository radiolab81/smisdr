import numpy as np
from scipy.signal import firwin

# --- Parameter ---
fs = 5.0e6           # Systemtakt (5 MSps)
numtaps = 121        # Ursprüngliche Anzahl Taps (Symmetrisch)
q_format = 15        # Für 16-Bit Signed

# Die drei Modi: [Rate-Name, Cutoff-Frequenz, Rate-Index]
modes = [
    {"name": "250 ksps", "cutoff": 110e3, "rate_idx": 0},
    {"name": "500 ksps", "cutoff": 245e3, "rate_idx": 1},
    {"name": "1.25 MSps", "cutoff": 650e3, "rate_idx": 2}
]

print("    initial begin")

for mode in modes:
    rate_idx = mode["rate_idx"]
    base_idx = rate_idx * 16 # Offset-Berechnung (0, 16, 32)
    
    print(f"        // Init Rate {rate_idx} (Index {base_idx}-{base_idx+9})")
    
    # 1. FIR berechnen
    coeffs = firwin(numtaps, mode["cutoff"], fs=fs, window=('kaiser', 8.6))
    coeffs = coeffs / np.sum(coeffs) # Unity Gain
    coeffs_q15 = np.round(coeffs * ((1 << q_format) - 1)).astype(int)
    
    # 2. Hardware-Anpassung: Den 0-ten Tap verwerfen. Wir nutzen Index 1 bis 60 
    # (was genau 60 Werten entspricht, die auf 6 ROMs * 10 Zyklen verteilt werden)
    relevant_taps = coeffs_q15[1:61]
    
    # 3. In die 6 ROMs verteilen
    for rom_idx in range(6):
        # 10 Werte pro ROM aus dem relevant_taps Array extrahieren
        start_tap = rom_idx * 10
        end_tap = start_tap + 10
        chunk = relevant_taps[start_tap:end_tap]
        
        assignments = []
        for tdm_cycle in range(10):
            val = chunk[tdm_cycle]
            sign_str = "" if val >= 0 else "-"
            abs_val = abs(val)
            rom_addr = base_idx + tdm_cycle
            
            # String passgenau formatieren für das Verilog Layout
            assignment_str = f"coeff_rom_{rom_idx}[{rom_addr}]={sign_str}16'sd{abs_val};"
            assignments.append(assignment_str.ljust(27)) # Für saubere Spaltenausrichtung
            
        # Eine komplette Zeile pro ROM ausgeben
        print("        " + "".join(assignments).strip())
        
    print("") # Leerzeile zwischen den Raten

print("    end")