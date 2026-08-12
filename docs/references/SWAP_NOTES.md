# Sample-Paar-Swap bei SMI-DMA-Packing (`SWAP_SAMPLE_PAIRS`)

Diese Notiz dokumentiert, warum `smi_udp_streaming_adc.c` (RX) und
`smi_tcp_streaming_dac.c` (TX) einen konfigurierbaren
`SWAP_SAMPLE_PAIRS`-Mechanismus enthalten, was dahintersteckt, und warum
er **fallweise selbst verifiziert werden muss** statt blind übernommen
zu werden.

## Kurzfassung

Mit `pack_data = 1` (Treiber-Setting) bzw. dem `PXLDAT`-Bit (Hardware-
Register) packt die SMI-DMA-Engine des BCM283x mehrere 8- oder 16-Bit-
Samples in ein gemeinsames 32-Bit-DMA-Wort. Je nach Bit-Breite, Treiber-
Version und Übertragungsrichtung (Lesen vs. Schreiben) kann dabei die
Reihenfolge benachbarter Samples im Speicher gegenüber der tatsächlichen
zeitlichen Reihenfolge vertauscht sein. `SWAP_SAMPLE_PAIRS` korrigiert
das nachträglich in Software.

**Wichtig:** Es gibt keine einzige, für alle Fälle gültige Antwort, ob
der Swap nötig ist. Die Quellenlage unten zeigt bewusst widersprüchliche
Datenpunkte – das ist kein Fehler in der Recherche, sondern der Grund,
warum ihr das an eurer eigenen Hardware/Kernelversion nachprüfen müsst.

## Was die Quellen sagen

### 1. Broadcom-Datenblatt (SMI, Abschnitt "Output Data Modes")

Der offizielle (aus dem Broadcom-Datenblatt extrahierte) Text zum
16-Bit-Modus mit `PXLDAT=1` / `WFORMAT=0` beschreibt die Zuordnung
zwischen FIFO-Wort und den beiden externen Transfers klar: die unteren
16 Bit eines 32-Bit-FIFO-Worts bilden die erste, die oberen 16 Bit die
zweite Übertragung. Auf einem Little-Endian-System (ARM) entspricht das
der **natürlichen, unvertauschten** Speicherreihenfolge – laut Datenblatt-
Prosa wäre für den reinen 16-Bit-Fall also **kein** Swap nötig.

Für den **8-Bit**-Modus (RGB565-Packing-Diagramm im selben Abschnitt)
zeigt das Datenblatt dagegen ein Bit-Layout, das nach Umrechnung auf
Little-Endian-Bytes einer paarweisen Vertauschung entspricht – hier
**ist** laut Diagramm ein Swap "eingebaut".

→ Quelle: *Secondary Memory Interface (SMI) – Extracted from Broadcom
data sheet*, G.J. van Loo, 26.11.2017 (kursiert öffentlich u.a. hier:
https://github.com/cariboulabs/cariboulite/blob/main/docs/Secondary%20Memory%20Interface.pdf)

### 2. Jeremy Bentham, iosoft.blog – Referenzcode (Apache-2.0)

**Kein Swap, 16-Bit, funktionierend:** In `rpi_smi_adc_test.c` (direkter
Registerzugriff über `/dev/mem`, kein Kernel-Treiber) liest
`adc_dma_end()` die 16-Bit-Samples aus dem DMA-Puffer strikt sequenziell
– ohne jede Vertauschung. Dieser Code ist durch Oszilloskop-Messungen
(u.a. 25 MS/s Video-Capture) als korrekt funktionierend dokumentiert.

**Swap vorhanden, 8-Bit, LED-Pixelmodus:** In `rpi_pixleds.c` (WS2812-
Treiber, 8-Bit-Modus mit `pxldat=1`) wird vor jedem DMA-Transfer
`swap_bytes()` aufgerufen, welches benachbarte 16-Bit-Werte per
`__builtin_bswap16()` vertauscht – exakt passend zum RGB565-Diagramm im
Datenblatt.

**Kommentar aus der Community (Blog-Diskussion, User "icarletto",
30.08.2021):** Beim direkten DAC-Testcode (8-Bit, `pxldat=0`, kein
Packing) wurde ein Bug gemeldet: ohne Packing werden nur 25% der Daten
tatsächlich ausgegeben. Als Fix wird vorgeschlagen, entweder auf
32-Bit-Transfers umzustellen oder `pxldat=1` zu setzen – mit dem
Hinweis, dass dabei die Samples analog zur RGB565-Bitreihenfolge
umsortiert werden müssen. Das bestätigt unabhängig, dass
Packing-Aktivierung beim **Schreiben** ebenfalls eine Umsortierung nach
sich zieht, nicht nur beim Lesen.

→ Quellen:
- https://iosoft.blog/2020/07/16/raspberry-pi-smi/ (Artikel + Kommentare)
- https://iosoft.blog/2020/06/11/fast-data-capture-raspberry-pi/
- Sourcecode: https://github.com/jbentham/rpi (Apache License 2.0,
  Copyright (c) Jeremy P Bentham 2020 – bei Wiederverwendung Attribution
  beibehalten)

### 3. Unser eigener, empirisch verifizierter Fall

Für unseren konkreten RX-Pfad (`smi_udp_streaming_adc.c`, 16-Bit,
`pack_data=1`, Kernel 6.12, Zugriff über den Linux-Kernel-Treiber
`/dev/smi` statt direktem `/dev/mem`-Zugriff) wurde empirisch verifiziert
– durch Aufnahme eines Rampen-Testmusters und Vergleich in GNU Radio –
dass ein paarweiser Sample-Swap **tatsächlich nötig** ist, um korrekte
Sample-Reihenfolge zu erhalten. Das widerspricht der Datenblatt-Prosa
für den reinen 16-Bit-Fall (siehe oben), deckt sich aber im Grundmuster
("Packing erfordert Reordering") mit Benthams 8-Bit-Erfahrung und dem
Community-Kommentar.

**Plausible Erklärung für die Diskrepanz:** Unser Code geht über den
Linux-Kernel-Treiber (`ioctl(BCM2835_SMI_IOC_WRITE_SETTINGS)`,
`read()`/`write()`-Syscalls auf `/dev/smi`), nicht über direkte
Register-Manipulation wie Benthams Code. Der Treiber könnte intern
zusätzliches Packing/Unpacking vornehmen, das vom rohen, im Datenblatt
beschriebenen Hardware-Verhalten abweicht. Verifiziert ist das nicht –
es ist die naheliegendste Hypothese, keine bestätigte Tatsache.

Für den TX-Pfad (`smi_tcp_streaming_dac.c`) wurde der gleiche Swap
strukturell übernommen (gleicher Treiber, gleicher 16-Bit-Modus,
gleiches `pack_data=1`), aber **nicht separat auf mehreren PIs/Treiberversionen verifiziert** – siehe
Abschnitt "Was noch offen ist" unten.

## Zusammenfassung der Quellenlage (Tabelle)

| Quelle | Modus | Zugriffsweg | Swap nötig? |
|---|---|---|---|
| Broadcom-Datenblatt (Prosa) | 16-Bit, PXLDAT=1/WFORMAT=0 | Hardware-Register direkt | Nein |
| Broadcom-Datenblatt (Diagramm) | 8-Bit, RGB565-Format | Hardware-Register direkt | Ja |
| Kernel driver (`bcm2835_smi.c`, `raspberrypi/linux`, verified rpi-4.4.y through rpi-5.4.y) | 8-bit write (`SMIDSW_WSWAP` auto-set); no equivalent exists for 16-bit or for any read width | Kernel driver, register level | Yes (8-bit write only); no mechanism exists at all for reads |
| Bentham `rpi_smi_adc_test.c` | 16-Bit | `/dev/mem` direkt | Nein (verifiziert per Oszi) |
| Bentham `rpi_pixleds.c` | 8-Bit | `/dev/mem` direkt | Ja (`swap_bytes()`) |
| Community-Kommentar (icarletto) | 8-Bit, DAC | `/dev/mem` direkt | Ja ("RGB565 ordering") |
| **Unser RX-Pfad** | 16-Bit | Kernel-Treiber `/dev/smi` | **Ja** (verifiziert: Rampe + GNU Radio) |
| **Unser TX-Pfad** | 16-Bit | Kernel-Treiber `/dev/smi` | Angenommen (strukturell übernommen, **nicht separat verifiziert**) |

## Was das für Nachbauer bedeutet

Der Code-Mechanismus (`SWAP_SAMPLE_PAIRS`-Schalter +
`swap_sample_pairs()`, an `g_applied_width`/`g_current_width` gekoppelte
`unit_size`) ist generisch genug, um alle oben aufgeführten Fälle
abzudecken – ihr müsst nichts umbauen, nur den Schalterwert für eure
eigene Kombination aus Pi-Modell, Kernelversion und Bit-Breite prüfen.

**Symptome, die auf eine falsche Swap-Einstellung hindeuten können:**
- Signal wirkt verrauscht/verzerrt, obwohl die Pipeline grundsätzlich läuft
- Gespiegelte/unerwartete Komponenten im Wasserfall bei der halben Samplerate

**Test ohne Logikanalysator:** Ein bekanntes Rampen- oder Testmuster
durch die Pipeline schicken (z.B. `tcp_test.py` für den DAC-Pfad, oder
ein einfacher Testpuffer für den ADC-Pfad) und die empfangene
Samplefolge gegen das gesendete Muster vergleichen. Wenn jedes zweite
Samplepaar vertauscht erscheint, `SWAP_SAMPLE_PAIRS` umschalten (0/1)
und erneut testen.

**Neu verifizieren bei:**
- Wechsel der Kernel-/Treiberversion
- Wechsel des Raspberry-Pi-Modells
- Wechsel zwischen 8-Bit- und 16-Bit-Modus
- Wechsel zwischen RX- und TX-Pfad (Lesen und Schreiben sind nicht
  garantiert symmetrisch – siehe Diskussion oben)

## Weiterführende Links

- Broadcom-SMI-Datenblatt (öffentliche Kopie via CaribouLite-Projekt):
  https://github.com/cariboulabs/cariboulite/blob/main/docs/Secondary%20Memory%20Interface.pdf
- Jeremy Bentham, "Raspberry Pi Secondary Memory Interface (SMI)":
  https://iosoft.blog/2020/07/16/raspberry-pi-smi/
- Jeremy Bentham, "Fast data capture with the Raspberry Pi":
  https://iosoft.blog/2020/06/11/fast-data-capture-raspberry-pi/
- Bentham Sourcecode-Repository (Apache License 2.0):
  https://github.com/jbentham/rpi
