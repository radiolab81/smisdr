# Digital Upconverter (DUC) for smiSDR & parlioSDR Frameworks

This folder contains a hardware-verified **Digital Upconverter (DUC)** gateware architecture tailored for the `radiolab81` ecosystem (`smisdr` and `parliosdr`). The design is engineered to accept variable-rate I/Q baseband samples over a high-speed parallel interface (SMI or PARLIO), perform advanced multi-stage multirate interpolation, and shift the signal to RF via a fully synchronous digital complex mixer driving a high-speed DAC.

The architecture features strict strobe-driven execution to eliminate multi-frequency Clock Domain Crossing (CDC) issues, utilizes distributed Block RAM structures for efficient FIR filter implementation, and incorporates rigorous bit-growth management with convergence rounding to maximize Spurious-Free Dynamic Range (SFDR).

---

## 1. System Architecture & Core Concept

The DUC serves as the bridge between the high-throughput, host-side DMA transfer domain and the strictly real-time, high-frequency DAC sampling domain. It processes baseband sample rates ($f_s$) of **250 ksps, 500 ksps, or 1.25 Msps** and upsamples the signal through a hybrid cascaded filter pipeline to a uniform intermediate frequency/sampling rate of **50 Msps**. 

Higher clock frequencies—such as those achieved by configuring PLLs within the synthesis tools—can be set without difficulty, and the signal processing within this chain adapts seamlessly to them. Likewise, additional I/Q bandwidths can be easily integrated. In our test setup, this enables interference-free operation for DATV via QO-100 or for DAB+.

### Functional Block Diagram & Data Flow

```
 Host DMA (SMI/PARLIO Bus)
         │ 16-bit Async Data + Strobe
         ▼
 ┌───────────────────────────────┐
 │         smi_rx_16bit          │ ──► [In-Band Command Parser] ──► NCO FTW / Rate Selection
 └───────────────────────────────┘
         │ 32-bit Packed I/Q (16-bit signed each)
         ▼
 ┌───────────────────────────────┐
 │           sync_fifo           │ (Inferred Dual-Port Block RAM Buffer)
 └───────────────────────────────┘
         │ 32-bit Continuous Stream
         ▼
 ┌───────────────────────────────┐
 │      multirate_upsampler      │ (Dual-Path Logic: Path A for 1.25M; Path B for 250k/500k)
 └───────────────────────────────┘
         │ 5 Msps Uniform Strobe
         ▼
 ┌───────────────────────────────┐
 │       baseband_sharpener      │ (Channel Selection FIR Filter via 6x M9K/BRAM Blocks)
 └───────────────────────────────┘
         │ 5 Msps Filtered I/Q
         ▼
 ┌───────────────────────────────┐
 │        cic_interpolator       │ (3rd Order Cascaded Integrator-Comb, R=10)
 └───────────────────────────────┘
         │ 50 Msps Continuous I/Q
         ▼             ┌────────────────┐
         │             │    nco_dual    │ ◄── [shared_sine_rom] (Quarter-Wave)
         ▼             └────────────────┘
 ┌───────────────────────────────┐     │ Cos/Sin LO
 │           duc_mixer           │ ◄───┘
 └───────────────────────────────┘
         │ 14-bit Offset Binary Output
         ▼
     High-Speed RF DAC (50 MHz)
```

---

## 2. Deep-Dive Module Breakdown

### 2.1 Host Interface & In-Band Signaling (`smi_rx_16bit.v`)
To minimize physical I/O pin count on the FPGA, the architecture routes both high-speed sample data and low-speed control parameters over the same 16-bit parallel bus using **In-Band Signaling**. 
* **Bit Multiplexing:** Bits `[15:14]` act as a packet identifier. `2'b00` and `2'b01` signal raw 14-bit I/Q baseband data, which the module immediately sign-extends to standard 16-bit signed integers. `2'b10` and `2'b11` signal configuration commands.
* **Command Parser:** Implements a state machine (`CMD_IDLE`, `CMD_RECV`) that assembles 32-bit parameters byte-by-byte (Little Endian) to dynamically program the Frequency Tuning Word (FTW) for the NCO (Command `'S'`) or the Interpolation Rate (Command `'R'`).
* **Glitch Filter:** Features a configurable digital glitch filter (`FILTER_TAPS = 4`) on the input strobe (`data_en`) to suppress ringing and transmission line reflections inherent to single-ended parallel buses on development boards.
* **Hardware Watchdog:** An embedded 50,000,000-cycle watchdog timer resets the internal state machine to `CMD_IDLE` if a host-side software application crashes mid-command transmission, preventing lockups.

### 2.2 Elastic Buffering (`sync_fifo.v`)
* **Memory Inference:** Explicitly written to trigger Dual-Port Block RAM inference (`(* ramstyle = "no_rw_check" *)`) across common synthesis tools (Quartus, Gowin EDA, Yosys).
* **Latency Optimization:** Employs zero-latency asynchronous read routing (`assign r_data = ram[r_ptr];`), allowing downstream DSP strobes to fetch samples instantaneously without adding pipeline cycles that could destabilize control loops.

### 2.3 Hybrid Multirate Upsampling Engine (`multirate_upsampler.v`)
Rather than instantiating a singular, resource-heavy interpolator, this engine divides processing into a optimized dual-path structure based on the incoming sample rate:
1. **Path A (1.25 Msps Input):** Implements a Time-Division Multiplexed (TDM) Multiply-Accumulate (MAC) architecture with fixed $L=4$ zero-stuffing to reach the 5 Msps intermediate rate. Accumulations are tracked in a 36-bit wide signed register to eliminate word-growth truncation errors during the MAC phase.
2. **Path B (250 ksps / 500 ksps Input):** Employs a multi-stage kaskade consisting of symmetric Half-Band filters and Polyphase structures. Since every second coefficient of a symmetric Half-Band filter is zero, the required multiplier count is halved. Fixed-point gain compensation is managed via precise bit-shifts (`>>> 10`) to enforce strict unity gain.

### 2.4 The Block RAM "Sharpener" FIR Filter (`baseband_sharpener.v`)
To achieve better stopband attenuation without exhausting the FPGA's logical fabric, the transition-band sharpening FIR filter is designed around physical Block RAM constraints.
* **Dual-Port Parallelization:** A single standard BRAM block permits only two memory accesses per clock cycle. To bypass this bottleneck, the design partitions the filter coefficients and delay lines into **6 discrete M9K/BRAM blocks**. 
* **TDM Computation:** By executing at the 50 MHz core clock while processing a 5 Msps throughput, the filter schedules 10 clock cycles per sample period. This allows the 6 parallel blocks to execute a massive number of multiply-accumulate operations, achieving an steep filter profile that ensures better channel selectivity.
* **Inference Guarding:** Separates un-reset memory matrices from highly resettable control logic, guaranteeing that synthesis tools map the arrays onto hard block memory rather than routing them via distributed flip-flops.

### 2.5 High-Ratio Interpolation (`cic_interpolator.v`)
Brings the 5 Msps filtered stream up to the final 50 Msps DAC rate ($R=10$).
* **Topology:** 3rd-order Cascaded Integrator-Comb (CIC) filter architecture.
* **Bit-Growth Management:** A 3rd-order CIC with an interpolation factor of 10 induces a DC bit-growth of:
  $$\Delta B = N \cdot \log_2(R) = 3 \cdot \log_2(10) pprox 9.96 	ext{ bits}$$
  The internal pipeline is strictly configured to 32 bits (`W_INT = 32`) to accommodate the native 16-bit input plus the required $\sim 10$ bits of growth without risking overflow.
* **Gain Rescaling:** A final right-shift (`>>> 5`) scales the raw growth down dynamically, leaving a clean residue gain of $pprox 3.125$ that perfectly aligns the signal's peak power with the dynamic range of the subsequent complex mixer.

### 2.6 Dual-Phase NCO (`nco_dual.v` & `shared_sine_rom.v`)
Generates high-purity quadrature Local Oscillator ($	ext{LO}$) signals.
* **Quarter-Wave Symmetry Optimization:** The physical ROM (`sin_quarter.hex`) contains only 1024 words representing the first quadrant ($0$ to $\pi/2$) of a sine wave.
* **Phase-to-Amplitude Mapping:** Uses bit `[10]` of the 12-bit address to mirror the index for the second and fourth quadrants, and bit `[11]` (MSB) to invert the sign for the third and fourth quadrants.
* **Quadrature Alignment:** The Cosine component is synthesized by offsetting the Sine phase accumulator tap by exactly 90 degrees (`+ 12'd1024`), ensuring perfect phase orthogonality ($\le 0.001^\circ$ skew) to eliminate unwanted image components during mixing.

### 2.7 Complex Digital Mixer (`duc_mixer.v`)
Executes the final quadrature upconversion:
$$x_{	ext{RF}}[n] = I[n] \cdot \cos(\omega n) - Q[n] \cdot \sin(\omega n)$$
* **Word-Growth & Precision Preservation:** The product of two 16-bit signed integers yields a 32-bit result. The subtraction stage expands this to 34 bits (`reg signed [33:0] rf_sum;`) to safely hold the worst-case peak-to-average combinations without clipping.
* **Convergent Rounding & Dynamic Range Maximization:** Before down-shifting the 34-bit sum to match the physical DAC width (`DAC_BITS`), a rounding offset ($+0.5 	ext{ LSB}$, defined via `ROUND_OFFSET`) is added. This suppresses truncation-induced DC offsets and minimizes the high-frequency quantization noise floor.
* **Offset-Binary Encoding:** Transforms the final signed RF value into an unsigned integer format required by standard single-ended high-speed DACs by summing a precise DC offset equal to $2^{	ext{DAC\_BITS}-1}$.

---

## 3. Verification & Simulation Framework (`sim_main.cpp`)

The gateware is validated using a cycle-accurate C++ simulation framework powered by **Verilator**. The testbench emulates physical hardware signaling and permits software-in-the-loop validation.

### Key Simulation Functionalities:
* **Standard Driver Emulation:** Programmatically constructs In-Band command structures based on command-line flags (`--rate`, `--shift`) to test the input parser's resilience.
* **Passthrough Modus (`--passthrough`):** Bypasses TB-generated in-band signals to ingest raw binary streams (`testbench_passthrough_ci16.iq`) compiled from real-world recordings with `cohi_wav_to_smi_iq.py` or other streaming tools like GNU Radio. This ensures that the exact byte stream emitted by host utility/sdr tools matches the hardware expectations bit-for-bit.
* **Clock-Modulo Alignment:** Dynamically tracks the system clock ratio to assert `data_en` for exactly 5 continuous cycles, verifying the functionality of the hardware glitch filter under realistic timing scenarios.
* **VCD Waveform Guarding:** Limits `.vcd` trace dumping to the first 100,000 cycles to avoid generating massive multi-gigabyte files during long signal sweeps.

---

## 4. Real-World Spectral Performance

The architecture's performance has been verified using real-world wideband multi-carrier RF captures from the **Cohiradia** project. The following analysis highlights the DUC's behavioral response under high-stress conditions.

### 4.1 Mediumwave (MW) Multi-Carrier Scenario
* **Signal Profile:** A high-density network of synthesized historical broadcast AM transmitters (including the simulated Vox-Haus on 783 kHz and various vintage music channels up to 1359 kHz).
* **DUC Performance Analysis:** Processing this massive block at 1.25 Msps tests the full bandwidth capability of the upsampler. The spectrum demonstrates incredibly sharp, vertical AM carriers with symmetric sidebands. The absence of a central spike confirms absolute **LO leakage suppression**, proving that the digital NCO and complex multipliers operate with flawless mathematical precision.

![cohimw1](https://github.com/radiolab81/smisdr/blob/main/gateware/www/cohi_spec_1250kHz.png)
![cohimw2](https://github.com/radiolab81/smisdr/blob/main/gateware/www/cohi_wf_1250kHz.png)

```console
Frequency	SNR	Country	Programme	TX Site
783.0 kHz 	- 	- 	Ansage Vox-Haus (Dauerschleife) 	synthetized
819.0 kHz 	- 	- 	div. Wortbeiträge von Schallplatten mit Rundfunkbezug (20er/30er Jahre) 	-
855.0 kHz 	- 	- 	Deutsche Tanzmusik 1925 	-
891.0 kHz 	- 	- 	div. originale Rundfunkmitschitte (20er/30er Jahre) 	-
927.0 kHz 	- 	- 	Deutsche Tanzmusik 1928 	-
963.0 kHz 	- 	- 	Deutsche Tanzmusik 1930 	-
999.0 kHz 	- 	- 	Amerikanische Tanzmusik (Odeon-Swing-Musik-Serie) 	-
1035.0 kHz 	- 	- 	Originaler Rundfunkbericht RRG aus dem Cotton-Club, New York, 1931 	-
1071.0 kHz 	- 	- 	Deutsche Tanzmusik 1932 	-
1107.0 kHz 	- 	- 	Deutsche Tanzmusik 1938 	-
1143.0 kHz 	- 	- 	Deutsche Tanzmusik 1940 	-
1179.0 kHz 	- 	- 	Rundfunkmitschnitte deutsches Programm der BBC 	-
1215.0 kHz 	- 	- 	Deutscher Propaganda-Swing (über Kurzwelle verbreitet, mit anti-britischen Texten) 	-
1251.0 kHz 	- 	- 	Deutsche Tanzmusik 1943 	-
1287.0 kHz 	- 	- 	Deutsche Tanzmusik 1946-48 	-
1323.0 kHz 	- 	- 	Deutsche Tanzmusik 50er Jahre I 	-
1359.0 kHz 	- 	- 	Deutsche Tanzmusik 50er Jahre II 	-
```


### 4.2 Longwave (LW) Spectrum Profile
* **Signal Profile:** Low-frequency AM broadcasts positioned very close to the DC (0 Hz) boundary line.
* **DUC Performance Analysis:** Modulating signals in close spectral proximity to the carrier frequency risks introducing high-amplitude CIC imaging artifacts and passband ripple distortion. Thanks to the cascaded FIR architectures in the `multirate_upsampler` and the steep stopband attenuation of the `baseband_sharpener`, the noise floor across the entire LW band remains flat and suppressed.

![cohilw1](https://github.com/radiolab81/smisdr/blob/main/gateware/www/cohi_spec_lw.png)

```console
Frequency	SNR	Country	Programme	TX Site
147.3 kHz 	48 	D 	DDH47, FSK METEO 	Pinneberg
153.0 kHz 	44 	ROU 	SRR Antena Satelor R. România Actualități 	Brașov/Bod Colonie
162.0 kHz 	56 	F 	TDF time signal 	Allouis
171.0 kHz 	42 	MRC 	Médi 1 	Nador
189.0 kHz 	43 	ISL 	RÚV Rás 1/RÚV Rás 2 	Gufuskálar (Hellissandur)
198.0 kHz 	56 	G 	BBC Radio4 	Droitwich/Mast A-B; Burghead; Westerglen; Dartford Tunnel;
225.0 kHz 	50 	POL 	Polskie Radio Jedynka 	Solec Kujawski/Kabat
252.0 kHz 	45 	ALG 	Chaîne 3 	Tipaza
```

### 4.3 49m Shortwave Band (SW)
* **Signal Profile:** Receptions of real-world ionospheric shortwave stations (e.g., Channel 292 on 6070 kHz, Moosbrunn, Kall-Krekel) subject to severe atmospheric fading, noise, and erratic signal peaks.
* **DUC Performance Analysis:** High Peak-to-Average Power Ratios (PAPR) originating from multi-station shortwave inputs regularly cause integer overflows in poorly designed mixers, resulting in severe Intermodulation Distortion (IMD). The spectrum shows clean, isolated station peaks. The convergent rounding mechanism inside `duc_mixer.v` ensures that the quantization noise floor stays uniformly low, preserving weak DX stations even when adjacent to high-power international transmitters.

![cohi49m1](https://github.com/radiolab81/smisdr/blob/main/gateware/www/cohi_spec_49m.png)

```console
Frequency	SNR	Country	Programme	TX Site
6005.0 kHz 	- 	SVK 	Radio Slovakia 	Kall-Krekel
6030.0 kHz 	- 	USA 	Radio Marti 	Greenville
6055.0 kHz 	- 	AUT 	Radio Austria (from 12:00: 6070: SM Radio Dessau) 	Moosbrunn
6070.0 kHz 	- 	D 	Channel 292la:de,en,nl,it 	Rohrbach/Waal 93
6085.0 kHz 	- 	D 	various 	Kall/Auf der Heide
6115.0 kHz 	- 	D 	Radio SE-TA 2 	Gera; HFCC: Hartenstein
6130.0 kHz 	- 	HOL 	Radio Europa 	Alpen aan den Rijn
6150.0 kHz 	- 	D 	Europa 24IGHF-Interessengemeinschaft Hochfrequenztechnik e.V. 	Datteln;
6160.0 kHz 	- 	D 	Shortwave Goldcarries Shortwave Radio and other programmes 	Winsen an der Aller
```

---

## 5. Synthesis & Implementation Notes

### Resource Optimization Tricks
* **Multiplier Redirection:** The gateware utilizes the synthesis attribute `(* multstyle = "logic" *)` on specific intermediate stages. This instructs tools like Quartus or Gowin EDA to implement multiplications using logic cells (LUTs) rather than hard DSP blocks. This is a crucial adaptation for smaller FPGAs (e.g., Cyclone IV devices) where dedicated DSP slices are a strict system bottleneck.
* **Memory Blocks:** Ensure that `sin_quarter.hex` is properly referenced in your toolchain's search path to allow successful automatic initialization of the `shared_sine_rom` M4K/Gowin-BSRAM memory structures.

### Recommended Toolchains:
* **Simulation:** Verilator (v5.0 or later) + GtkWave for waveform viewing.
* **Open-Source Synthesis:** OSS CAD Suite (Yosys + nextpnr-himbaechel).
* **Proprietary Synthesis:** Intel Quartus Prime (Lite Edition) or Gowin EDA.
