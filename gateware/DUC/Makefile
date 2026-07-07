# Variablen
VERILATOR = verilator
PYTHON = python3

# Hier das oberste Modul angeben (das, was die Pins nach außen führt)
TOP_MODULE = duc_top  #top_system

# Automatisch alle .v Dateien im aktuellen Verzeichnis finden
V_SRC = $(wildcard *.v)
CPP_SRC = sim_main.cpp

OBJ_DIR = obj_dir
BIN_NAME = V$(TOP_MODULE)
BIN = $(OBJ_DIR)/$(BIN_NAME)

# Zielverzeichnis (übergeordneter Ordner) und finaler Pfad
TARGET_DIR = .
FINAL_BIN = $(TARGET_DIR)/$(BIN_NAME)

# Standard-Ziel: Alles bauen, kopieren und aus dem übergeordneten Ordner starten
all: $(FINAL_BIN)
	@echo "--- Simulation wird gestartet ---"
	$(FINAL_BIN)

# Schritt 1: LUT mit Python generieren
sin_table.hex: gen_lut.py
	@$(PYTHON) gen_lut.py

# Schritt 2: Verilator aufrufen, C++ Binary bauen UND kopieren
$(BIN): $(V_SRC) $(CPP_SRC) sin_table.hex
	$(VERILATOR) -Wall --cc --exe --build -j 0 --trace \
		--top-module $(TOP_MODULE) \
		$(CPP_SRC) $(V_SRC)

# ieses Target triggert den Build und kopiert das Binary nur bei Änderungen
$(FINAL_BIN): $(BIN)
	cp $(BIN) $(FINAL_BIN)

# Aufräumen (löscht jetzt auch das kopierte Binary im übergeordneten Ordner)
clean:
	rm -rf $(OBJ_DIR) sin_table.hex waveform.vcd $(FINAL_BIN)

.PHONY: all clean
