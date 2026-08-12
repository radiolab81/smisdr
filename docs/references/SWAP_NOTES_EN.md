# Sample-Pair Swap under SMI DMA Packing (`SWAP_SAMPLE_PAIRS`)

This note documents why `smi_udp_streaming_adc.c` (RX) and
`smi_tcp_streaming_dac.c` (TX) contain a configurable
`SWAP_SAMPLE_PAIRS` mechanism, what causes the need for it, and why it
must be **verified case by case** rather than copied blindly.

## Summary

With `pack_data = 1` (driver setting) / the `PXLDAT` bit (hardware
register), the SMI DMA engine on the BCM283x packs several 8- or 16-bit
samples into a shared 32-bit DMA word. Depending on bit width, driver
version, and transfer direction (read vs. write), the order of adjacent
samples in memory can end up swapped relative to their actual time
order. `SWAP_SAMPLE_PAIRS` corrects this afterwards in software.

**Important:** There is no single answer that holds for every setup.
The sources below deliberately show conflicting data points — that's
not a research gap, it's the reason you need to verify this on your own
hardware/kernel combination.

## What the sources say

### 1. Broadcom datasheet (SMI, "Output Data Modes" section)

The official (Broadcom-datasheet-extracted) text on 16-bit mode with
`PXLDAT=1` / `WFORMAT=0` clearly describes the mapping between FIFO word
and the two external transfers: the lower 16 bits of a 32-bit FIFO word
form the first transfer, the upper 16 bits the second. On a
little-endian system (ARM) that corresponds to the **natural,
unswapped** memory order — per the datasheet prose, the pure 16-bit
case would need **no** swap.

For **8-bit** mode (the RGB565 packing diagram in the same section),
the datasheet instead shows a bit layout that, once translated to
little-endian bytes, corresponds to a pairwise swap — here the diagram
**does** imply a swap.

→ Source: *Secondary Memory Interface (SMI) – Extracted from Broadcom
data sheet*, G.J. van Loo, 26 Nov 2017 (circulates publicly e.g. here:
https://github.com/cariboulabs/cariboulite/blob/main/docs/Secondary%20Memory%20Interface.pdf)

### 2. Jeremy Bentham, iosoft.blog — reference code (Apache-2.0)

**No swap, 16-bit, working:** In `rpi_smi_adc_test.c` (direct register
access via `/dev/mem`, no kernel driver), `adc_dma_end()` reads the
16-bit samples out of the DMA buffer strictly sequentially — no
swapping at all. This code is documented as working correctly via
oscilloscope measurements (including a 25 MS/s video capture).

**Swap present, 8-bit, LED pixel mode:** In `rpi_pixleds.c` (WS2812
driver, 8-bit mode with `pxldat=1`), `swap_bytes()` is called before
every DMA transfer, swapping adjacent 16-bit values via
`__builtin_bswap16()` — matching exactly the RGB565 diagram in the
datasheet.

**Comment from the community (blog discussion, user "icarletto",
30 Aug 2021):** A bug was reported in the direct DAC test code (8-bit,
`pxldat=0`, no packing): without packing, only 25% of the data actually
gets output. The suggested fix is either to switch to 32-bit transfers
or to set `pxldat=1`, with the note that doing so requires reordering
the samples to match the RGB565 bit order. This independently confirms
that enabling packing on the **write** side also requires reordering,
not just on read.

→ Sources:
- https://iosoft.blog/2020/07/16/raspberry-pi-smi/ (article + comments)
- https://iosoft.blog/2020/06/11/fast-data-capture-raspberry-pi/
- Source code: https://github.com/jbentham/rpi (Apache License 2.0,
  Copyright (c) Jeremy P Bentham 2020 — keep attribution if reused)

### 3. Our own, empirically verified case

For our specific RX path (`smi_udp_streaming_adc.c`, 16-bit,
`pack_data=1`, kernel 6.12, accessed through the Linux kernel driver
`/dev/smi` rather than direct `/dev/mem` register access), we
empirically verified — by streaming a ramp test pattern and comparing
it in GNU Radio — that a pairwise sample swap **is actually required**
to get correct sample order. This contradicts the datasheet prose for
the pure 16-bit case (see above), but matches the underlying pattern
("packing requires reordering") seen in Bentham's 8-bit experience and
the community comment.

**Plausible explanation for the discrepancy:** our code goes through
the Linux kernel driver (`ioctl(BCM2835_SMI_IOC_WRITE_SETTINGS)`,
`read()`/`write()` syscalls on `/dev/smi`) rather than direct register
manipulation like Bentham's code. The driver may perform additional
packing/unpacking internally that differs from the raw hardware
behaviour described in the datasheet. This is unverified — it's the
most plausible hypothesis, not a confirmed fact.

For the TX path (`smi_tcp_streaming_dac.c`), the same swap was carried
over structurally (same driver, same 16-bit mode, same `pack_data=1`)
but **not separately verified** — see "What's still open" below.

## Summary table

| Source                        | Mode                       | Access path              | Swap needed?                                                     |
| ----------------------------- | -------------------------- | ------------------------ | ---------------------------------------------------------------- |
| Broadcom datasheet (prose)    | 16-bit, PXLDAT=1/WFORMAT=0 | Direct hardware register | No                                                               |
| Broadcom datasheet (diagram)  | 8-bit, RGB565 format       | Direct hardware register | Yes                                                              |
| Kernel driver (`bcm2835_smi.c`, `raspberrypi/linux`, verified rpi-4.4.y through rpi-5.4.y) | 8-bit write (`SMIDSW_WSWAP` auto-set); no equivalent exists for 16-bit or for any read width | Kernel driver, register level | Yes (8-bit write only); no mechanism exists at all for reads |
| Bentham `rpi_smi_adc_test.c`  | 16-bit                     | Direct `/dev/mem`        | No (verified via scope)                                          |
| Bentham `rpi_pixleds.c`       | 8-bit                      | Direct `/dev/mem`        | Yes (`swap_bytes()`)                                             |
| Community comment (icarletto) | 8-bit, DAC                 | Direct `/dev/mem`        | Yes ("RGB565 ordering")                                          |
| **Our RX path**               | 16-bit                     | Kernel driver `/dev/smi` | **Yes** (verified: ramp + GNU Radio)                             |
| **Our TX path**               | 16-bit                     | Kernel driver `/dev/smi` | Assumed (carried over structurally, **not separately verified**) |

## What this means if you're building your own

The code mechanism (`SWAP_SAMPLE_PAIRS` toggle + `swap_sample_pairs()`,
with `unit_size` tied to `g_applied_width`/`g_current_width`) is generic
enough to cover every case listed above — you don't need to change the
mechanism itself, just check the toggle value for your own combination
of Pi model, kernel version, and bit width.

**Symptoms that may point to a wrong swap setting:**
- Signal looks noisy/off even though the pipeline is otherwise running
- Metallic/aliased-sounding quality on audio
- Unexpected mirrored components in the waterfall at half the sample rate

**Testing without a logic analyzer:** feed a known ramp or step pattern
through the pipeline (e.g. `tcp_test.py` for the DAC path, or a simple
test buffer for the ADC path) and compare the received sample sequence
against what you sent. If every second sample pair looks swapped,
toggle `SWAP_SAMPLE_PAIRS` (0/1) and re-test.

**Re-verify whenever you:**
- change kernel/driver version
- change Raspberry Pi model
- switch between 8-bit and 16-bit mode
- switch between the RX and TX path (read and write are not guaranteed
  to behave symmetrically — see discussion above)

## Further reading

- Broadcom SMI datasheet (public copy via the CaribouLite project):
  https://github.com/cariboulabs/cariboulite/blob/main/docs/Secondary%20Memory%20Interface.pdf
- Jeremy Bentham, "Raspberry Pi Secondary Memory Interface (SMI)":
  https://iosoft.blog/2020/07/16/raspberry-pi-smi/
- Jeremy Bentham, "Fast data capture with the Raspberry Pi":
  https://iosoft.blog/2020/06/11/fast-data-capture-raspberry-pi/
- Bentham source code repository (Apache License 2.0):
  https://github.com/jbentham/rpi
