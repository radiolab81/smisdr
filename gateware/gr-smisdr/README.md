# gr-smisdr

GNU Radio 3.10 Out-of-Tree-Modul für das smiSDR 16-Bit In-Band-Signaling-
Protokoll (smiBus / ESP32 PARLIO). Siehe Hauptprojekt für Details zu
Gateware und Protokoll: `smi_rx_16bit.v`, `gateware/README.md`.

Blöcke:

* `smisdr.encoder(sample_rate, shift_hz, master_clock=50e6, scale=8191, inject_at_start=True)`
  complex → short. Message-In-Port `cmd` für Live-Trigger.
* `smisdr.decoder(master_clock=50e6, scale=8191, cmd_timeout_words=0)`
  short → complex. Message-Out-Port `cmd` mit erkannten R/S-Kommandos.

Build- und Integrationsanleitung: siehe Projekt-Dokumentation.
