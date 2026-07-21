# gr-smisdr

**GNU Radio 3.10 out-of-tree module for the smiSDR 16-bit in-band signaling
protocol (smiBus / ESP32 PARLIO).**

This module provides two C++ blocks — `smisdr.encoder` and `smisdr.decoder`
— that translate between a normal complex I/Q stream in GNU Radio and the
raw 16-bit protocol word stream consumed/produced by the smiSDR FPGA core
described in [`gateware/README.md`](../README.md) and implemented in
[`gateware/smi_rx_16bit.v`](../smi_rx_16bit.v). They let you drive the FPGA
DUC/DDC path — including live sample-rate and NCO-shift reconfiguration —
directly from a GNU Radio flowgraph, over a File Sink/Source or a TCP
socket, with no additional glue code required on the host side.

Both blocks have been verified bit-exact against the reference C++
testbench (`sim_main.cpp`, Verilator) and the reference Python encoder
(`cohi_wav_to_smi_iq.py`), and validated in a live loopback with
[COHIRADIAStreamer](https://github.com/radiolab81/COHIRADIAStreamer)
against a running `smi_tcp_streaming_dac` instance.

---

## 1. Why this exists

The smiSDR gateware accepts baseband I/Q samples over a 16-bit parallel
bus and performs hardware DUC/DDC (see the main gateware README for the
rationale). To keep the bus protocol simple and avoid a second control
channel (I²C/SPI), sample-rate and NCO-shift reconfiguration is
multiplexed *in-band*, using the two most-significant bits of every
16-bit word as a type tag:

| Word type      | `[15:14]` | `[13:8]`         | `[7:0]`              |
|-----------------|:---------:|:-----------------:|:----------------------:|
| I-Sample        | `00`      | 14-bit I data `[13:0]` (spans into bits 13:8)                |
| Q-Sample        | `01`      | 14-bit Q data `[13:0]` (spans into bits 13:8)                |
| Command Init    | `10`      | `111111` (0x3F, sync)  | ASCII command (`'R'`, `'S'`) |
| Param Chunk     | `10`      | byte index `0..3` (LE) | parameter data byte     |
| Command End     | `11`      | ignored            | ASCII `'E'` (execute)   |

GNU Radio has no native concept of this kind of tagged, variable-format
word stream, so a direct `complex -> short` type conversion chain
(`Float To Complex` → `Complex To Interleaved Short` etc.) cannot produce
it — the interleaving logic, the 14-bit truncation/masking, the tuning
word arithmetic and the command-injection state machine all need custom
code. That is what this OOT module implements.

---

## 2. Block reference

### 2.1 `smisdr.encoder` — complex → smiSDR protocol

```
smisdr.encoder(sample_rate, shift_hz, master_clock=50e6,
               scale=8191, inject_at_start=True)
```

| Port          | Direction | Type      | Notes                                          |
|---------------|-----------|-----------|-------------------------------------------------|
| `iq_in`       | in        | stream, `complex` | normalized baseband I/Q, `\|I\|,\|Q\| <= 1.0`      |
| `cmd`         | in        | message   | live command trigger, see §2.3                 |
| `smi_out`     | out       | stream, `short` (16-bit) | raw protocol word stream                |

| Parameter          | Meaning                                                                                   |
|---------------------|--------------------------------------------------------------------------------------------|
| `sample_rate`       | Input I/Q sample rate in Hz. Encoded as an `'R'` command (32-bit tuning word).             |
| `shift_hz`          | Initial NCO shift frequency in Hz. Encoded as an `'S'` command.                            |
| `master_clock`      | FPGA reference clock used for the 32-bit tuning-word calculation. Default 50 MHz, matching `sim_main.cpp` and `cohi_wav_to_smi_iq.py`. |
| `scale`             | Full-scale value used to convert normalized `complex` input into 14-bit signed integers. Default **8191** (`2^13 - 1`) — see §2.4 for why. |
| `inject_at_start`   | If `True`, the `'S'`/`'R'` command sequence is emitted before the very first I/Q sample, mirroring the default behavior of `sim_main.cpp`. |

The block converts each incoming complex sample into two consecutive
16-bit words (I, then Q — order matters, see §3), clipped to ±1.0 and
scaled/rounded to a 14-bit signed integer. Pending in-band
commands (queued via `set_shift()`/`set_sample_rate()` or the `cmd`
message port) always take priority and are drained word-by-word before
the next I/Q pair is emitted, so a command sequence is never split across
partial output buffers.

### 2.2 `smisdr.decoder` — smiSDR protocol → complex

```
smisdr.decoder(master_clock=50e6, scale=8191, cmd_timeout_words=0)
```

| Port          | Direction | Type      | Notes                                                          |
|---------------|-----------|-----------|------------------------------------------------------------------|
| `smi_in`      | in        | stream, `short` (16-bit) | raw protocol word stream (File/TCP Source, or the encoder directly) |
| `iq_out`      | out       | stream, `complex` | reconstructed, normalized baseband I/Q                        |
| `cmd`         | out       | message   | one PMT dict per successfully parsed command, see below         |

| Parameter             | Meaning                                                                                       |
|-------------------------|-------------------------------------------------------------------------------------------------|
| `master_clock`          | FPGA reference clock, used to convert the received 32-bit tuning word back into Hz.            |
| `scale`                  | Full-scale value used to convert 14-bit signed samples back into normalized `complex`. Must match the encoder's `scale` for a correct round-trip. |
| `cmd_timeout_words`      | Optional software watchdog (in received words): resets a stalled command parse back to idle. `0` disables it. Mirrors `TIMEOUT_CYCLES` in `smi_rx_16bit.v`, but word-clocked rather than bus-clocked since the block has no notion of the FPGA's bus clock. |

On every completed `'R'`/`'S'` command the decoder publishes a PMT
dictionary on the `cmd` message port:

```
{"cmd": "R" | "S", "raw": <uint32 tuning word>, "hz": <double, decoded frequency>}
```

Connect this to a `Message Debug` block for inspection, or to a custom
Python/C++ message handler to reflect the live configuration into your
own application state (e.g. updating a GUI frequency display, as done by
COHIRADIAStreamer).

### 2.3 Triggering commands at runtime

Besides `inject_at_start`, the encoder accepts commands at any point
during a running flowgraph, either by calling the block's public API
directly from Python/C++:

```python
self.enc.set_shift(3.5e6)          # new 'S' command, injected before the next I/Q pair
self.enc.set_sample_rate(125000)   # new 'R' command
```

or asynchronously via the `cmd` message input port, using a PMT pair of
`(symbol . number)`:

```python
import pmt
self.msg_out.message_port_pub(pmt.intern("cmd"),
                               pmt.cons(pmt.intern("shift"), pmt.from_double(3.5e6)))
```

Both mechanisms are equivalent — a message on `cmd` simply calls
`set_shift()`/`set_sample_rate()` internally — and both are thread-safe;
the pending command is queued and drained atomically at the next
opportunity in `general_work()`, so it can never tear a command sequence
or an I/Q sample pair.

### 2.4 A note on `scale` and clipping safety

The protocol's native I/Q resolution is 14-bit signed, i.e. the range
`[-8192, +8191]`. The block's `scale` parameter controls how a
normalized `complex` sample (`±1.0` full scale) maps onto that range.
The default, **8191**, is a *symmetric peak-normalized* mapping:
`+1.0 → +8191`, `-1.0 → -8191`. This is the conventional choice for
generic DSP sources (signal generators, filters, arbitrary complex
blocks) because it can never overflow: the most negative 14-bit code
(`-8192`) is simply never used, trading one code point for guaranteed
symmetry and headroom.

An alternative value, **8192**, exactly reproduces the arithmetic
right-shift (`i16 >> 2`, i.e. divide-by-4) used by
`cohi_wav_to_smi_iq.py` to down-convert 16-bit WAV audio to the 14-bit
protocol width, *provided* the input comes from a `Wav File Source`
block. `Wav File Source` normalizes 16-bit PCM by dividing by `2^15 =
32768`; since the largest representable 16-bit code is `32767 < 32768`,
that normalization can never reach exactly `+1.0`, so `scale=8192` is
safe in that specific pipeline. It is **not** a safe default for
arbitrary sources, where a genuine `+1.0` sample would, before clamping,
compute to `+8192` — one code past the positive rail, wrapping to
`-8192` after the 14-bit mask (a sign flip at the signal peak).

---

## 3. Protocol implementation notes

The implementation was checked word-for-word against `smi_rx_16bit.v`
rather than only against the prose description in `gateware/README.md`,
and intentionally reproduces two behaviors of the hardware receiver that
are easy to get wrong from the documentation alone:

1. **I/Q latch ordering.** An I-sample word only updates an internal
   latch; nothing is emitted until the *following* Q-sample word arrives,
   at which point both values are emitted together. If a stream is cut
   or resynchronized mid-pair, the decoder — like the hardware — will
   silently pair the latched I value with whatever Q arrives next. Always
   keep the I→Q ordering intact when constructing raw protocol streams.

2. **No completeness check on Command End.** A `Command End` word
   (`ctrl=11`, payload `'E'`) commits whatever is currently in the
   4-byte parameter assembly buffer, **without verifying that all 4
   parameter bytes actually arrived**. This is a property of the
   hardware receiver, not a bug introduced by this module — the decoder
   deliberately reproduces it rather than "fixing" it, so that
   host-side simulation and the real FPGA behave identically even in
   malformed-stream edge cases.

The 32-bit tuning word arithmetic (`round((f / f_clk) * 2^32)`, 50 MHz
reference clock) and the little-endian byte ordering of the 4-byte
parameter chunks match `sim_main.cpp` and `cohi_wav_to_smi_iq.py`
exactly.

---

## 4. Building

### 4.1 Dependencies (Ubuntu/Debian, GNU Radio 3.10)

```bash
sudo apt install gnuradio-dev cmake build-essential pybind11-dev python3-dev
```

### 4.2 Build & install

Build against the **same install prefix as your system GNU Radio**
(commonly `/usr` for a distro package, `/usr/local` for a from-source
build) so that `gnuradio-companion` picks up the GRC block definitions
without extra configuration:

```bash
cd gateware/gr-smisdr
mkdir build && cd build
cmake ..   # match your GNU Radio install prefix -DCMAKE_INSTALL_PREFIX=/usr...
make -j$(nproc)
sudo make install
sudo ldconfig
```

### 4.3 Verify the install

```bash
python3 -c "from gnuradio import smisdr; print(smisdr.encoder, smisdr.decoder)"
```

```bash
python3 ../examples/loopback_example.py
```

This runs an in-process encoder → decoder loopback with a synthetic test
tone, prints the two startup commands (`'S'`, `'R'`) decoded back out on
the console, then live-injects a shift and a rate change after 2/3
seconds to exercise the runtime trigger path.

### 4.4 GNU Radio Companion (GRC)

Restart `gnuradio-companion` after installing (block definitions are
only re-scanned at startup) and search for **"smiSDR"** in the block
tree — both `smiSDR In-Band Encoder` and `smiSDR In-Band Decoder` should
appear under the `smisdr` category.

If GRC still doesn't list them, either your `CMAKE_INSTALL_PREFIX` isn't
one of GRC's scanned block paths, or the shared library isn't on the
linker path yet. Check:

```bash
ls $(pkg-config --variable=prefix gnuradio-runtime 2>/dev/null || echo /usr)/share/gnuradio/grc/blocks/smisdr_*
```

and, if empty, add the actual install path to GRC's *Preferences →
"Local blocks path(s)"*, or set `GRC_BLOCKS_PATH` before launching
`gnuradio-companion`.

---

## 5. Example flowgraphs

Ready-to-open `.grc` files, tested end-to-end (hex-editor byte
verification, live-decoded against `smi_tcp_streaming_dac` and
COHIRADIAStreamer) are provided in `examples/grc/`:

### `loopback.grc`
![loopback flowgraph](examples/grc/loopback_grc.png)

Top half: `Wav File Source` → `Float To Complex` → `smisdr.encoder`
(`Full-Scale 8192`, `Inject Cmd at Start = Yes`) → `smi_out` written both
to a `Virtual Sink` (stream ID `TX`) and a `File Sink` for offline
inspection.

Bottom half: a `File Source`/`Virtual Source` feeds the same word stream
into `smisdr.decoder`, which reconstructs the I/Q stream onto a
`QT GUI Sink` (spectrum/waterfall/constellation) and republishes every
parsed `'R'`/`'S'` command on its `cmd` message port, visible live in the
`Message Debug` block. Use this flowgraph to validate a `.wav` → smiSDR
conversion entirely inside GNU Radio, without touching the FPGA or a
network link.

### `smisdr_sim.grc`
![decoder-only flowgraph](examples/grc/smisdr_sim_grc.png)

A minimal decoder-only flowgraph: `TCP Source` (client to
`smi_tcp_streaming_dac`'s streaming port, or to a live
`COHIRADIAStreamer` session) → `smisdr.decoder` → `QT GUI Sink` + command
`Message Debug`. This is the fastest way to visually confirm that a live
FPGA/host stream is being interpreted correctly — including watching the
`'S'`/`'R'` commands roll by in real time as they're issued from the
sending application.

![COHIRADIAStreamer feeding smisdr_sim.grc live](examples/grc/COHIRADIAStreamer_on_smiSDR_sim.png)

Live validation: COHIRADIAStreamer streaming a COHIRADIA `.wav` archive
file over TCP with `I/Q Mode = SW or FPGA DUC (16 Bit with In-Band
Signaling)` enabled, decoded in real time by `smisdr_sim.grc`. The
`Message Debug` log on the left confirms the exact `hz`/`raw` values for
both the `'S'` (shift) and `'R'` (rate) commands issued at stream start,
matching the file's metadata (`1.25 MHz` shift, `1.25 MSPS` rate) shown
in the streamer's file-info panel.

### Python examples

* `examples/loopback_example.py` — programmatic encoder→decoder
  loopback with a synthetic tone and live command injection, useful as
  a smoke test after building (see §4.3).

---

## 6. Directory layout

```
gateware/gr-smisdr/
├── CMakeLists.txt                  top-level OOT module build file
├── README.md                       this file
├── include/gnuradio/smisdr/        public C++ headers (encoder.h, decoder.h, api.h)
├── lib/                            C++ block implementations (encoder_impl.*, decoder_impl.*)
├── python/smisdr/                  pybind11 bindings + gnuradio.smisdr Python package
├── grc/                            GRC block definitions (smisdr_encoder.block.yml, smisdr_decoder.block.yml)
└── examples/
    ├── loopback_example.py
    ├── loopback.grc / loopback_grc.png
    └── smisdr_sim.grc ── COHIRADIAStreamer_on_smiSDR_sim.png
```

---

## 7. Related documentation

* Protocol specification & rationale: [`gateware/README.md`](../README.md)
* Reference HDL receiver: [`gateware/smi_rx_16bit.v`](../smi_rx_16bit.v)
* Reference Verilator testbench: `sim_main.cpp`
* Reference Python encoder (WAV → protocol file): `cohi_wav_to_smi_iq.py`
* Live streaming host application: [COHIRADIAStreamer](https://github.com/radiolab81/COHIRADIAStreamer)
* SMI bus / host DMA layer: top-level [`smisdr` README](../../README.md), `smi_tcp_streaming_dac.c`
