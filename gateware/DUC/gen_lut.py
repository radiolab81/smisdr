import numpy as np

# Parameter: 1024 Einträge, 16-Bit Signed Integer
depth = 1024
max_val = 32767 

with open("sin_table.hex", "w") as f:
    for i in range(depth):
        sin_val = np.sin(2 * np.pi * i / depth)
        int_val = int(sin_val * max_val)
        # Formatierung als 4-stelliger Hex-Wert (Zweierkomplement)
        f.write(f"{int_val & 0xFFFF:04x}\n")
