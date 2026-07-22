# smisdr
Baseband/RF over Ethernet - powered by Raspberry Secondary Memory Interface (DMA - driven) - for Software Defined Radio

![main01](https://github.com/radiolab81/smisdr/blob/main/www/schematic.jpg)

For high-speed connection of a DAC/ADC in SDR applications, the Raspberry Pi offers the Secondary Memory Interface (SMI). To enable this interface, you must make the following changes to the /boot/firmware/config.txt file to avoid resource conflicts, as the SMI occupies almost all GPIOs in the header.

```console
# For more options and information see
# http://rptl.io/configtxt
# Some settings may impact device functionality. See link above for details

# Uncomment some or all of these to enable the optional hardware interfaces
dtparam=i2c_arm=off
dtparam=i2s=off
dtparam=spi=off
dtparam=i2c0=off
dtparam=i2c1=off
enable_uart=0

# Enable audio (loads snd_bcm2835)
dtparam=audio=off

# Additional overlays and parameters are documented
# /boot/firmware/overlays/README

# Automatically load overlays for detected cameras
#camera_auto_detect=1

# Automatically load overlays for detected DSI displays
#display_auto_detect=1

# Automatically load initramfs files, if found
auto_initramfs=1

# Enable DRM VC4 V3D driver
dtoverlay=vc4-kms-v3d
max_framebuffers=2

# Don't have the firmware create an initial video= setting in cmdline.txt.
# Use the kernel's default instead.
disable_fw_kms_setup=1

# Disable compensation for displays with overscan
disable_overscan=1

# Run as fast as firmware / board allows
arm_boost=1

[cm4]
# Enable host mode on the 2711 built-in XHCI USB controller.
# This line should be removed if the legacy DWC2 controller is required
# (e.g. for USB device mode) or if USB support is not required.
otg_mode=1

[cm5]
dtoverlay=dwc2,dr_mode=host


# arm_64bit=0 -> not for trixie (deb13)
[all]
dtoverlay=smi
dtoverlay=smi-dev
arm_64bit=0
core_freq=250
core_freq_min=250
force_turbo=1
```

#### Important: We are using a pure 32-bit kernel (armv7l) here.
```console
pi@SMISDR:~/smisdr $ uname -a
Linux SMISDR 6.12.47+rpt-rpi-v7l #1 SMP Raspbian 1:6.12.47-1+rpt1~bookworm (2025-09-16) armv7l GNU/Linux
```
After a reboot with the settings made above, a successfully activated SMI is now available in the device tree.

```console
pi@SMISDR:~/smisdr $ ls /dev/smi 
/dev/smi
```

The pin assignment of the header to the DAC (or ADC) is shown in the following diagram.

```console
/*
 * SMI 16-Bit Pin-Belegung auf dem Raspberry Pi (GPIO):
 * --------------------------------------------------
 * Daten-Bits (D0-D15):
 * SD0  : GPIO 8  (Pin 24) | SD8  : GPIO 16 (Pin 36)
 * SD1  : GPIO 9  (Pin 21) | SD9  : GPIO 17 (Pin 11)
 * SD2  : GPIO 10 (Pin 19) | SD10 : GPIO 18 (Pin 12)
 * SD3  : GPIO 11 (Pin 23) | SD11 : GPIO 19 (Pin 35)
 * SD4  : GPIO 12 (Pin 32) | SD12 : GPIO 20 (Pin 38)
 * SD5  : GPIO 13 (Pin 33) | SD13 : GPIO 21 (Pin 40)
 * SD6  : GPIO 14 (Pin  8) | SD14 : GPIO 24 (Pin 15)
 * SD7  : GPIO 15 (Pin 10) | SD15 : GPIO 25 (Pin 26)
 *
 * Steuer-Signale:
 * SWE  : GPIO 7  (Pin 26) - SMI Write Enable (Taktet die Daten in den DAC)
 *
 * Hinweis:
 * Die GPIOs müssen ggf. auf die Alternate Function 1 'SMI' gesetzt werden.
 * Das 'smi-dev' Overlay übernimmt dies normalerweise beim Booten automatisch.
 */
```
By setting the core_freq
to 250 MHz in the config.txt, the following clock rates result, with which a DAC / ADC can work very well.

```console
/*
target-rate	cycles (total)	real-rate	error
5.0 MSPS	  25	            5.0000 MSPS		0% (ok)
6.25 MSPS	  20	            6.2500 MSPS		0% (ok)
10.0 MSPS	  12.5	           10.4167 MSPS		+4.1% (bad choice)
12.5 MSPS	  10	           12.5000 MSPS		0% (ok)
15.625 MSPS	  8	               15.6250 MSPS		0% (ok)
25.0 MSPS	  5	               25.0000 MSPS		0% (ok) */
```

### repo-structure
- `README.md`: This file
- `smi_util.c`: Tool to read/write the current SMI settings and send/receive test data. You can build it with `make` (see Makefile)
- `smi_sinus.c`: Tool for generating and outputting a sine waveform at a specific data rate, can be build by `build_smi_sinus.sh`
- `tcp_test.py`: test tool for generating and outputting a sine waveform at a specific data rate via TCP/localhost to smi_tcp_streaming_dac process
- `gateware`: HDL code for FPGA extensions such as hardware-accelerated I/Q processing, DUC, DDC, ...
- `smi_tcp_streaming_dac.c`: Main tool for streaming baseband or RF data, receives data on port 1234 from external computers/apps such as GNU Radio or from localhost with internal apps, can be build by `build_smi_tcp_streamig_dac.sh`

  Similar to a Red Pitaya, it receives commands on port 5000 for on-the-fly adjustment of the sample rate and bus width (8/16 bits).

for example:

#### set sample rate to 5 MSPS
echo -n "rate 5" | nc -w 1 192.168.1.135 5000

#### set sample rate to 10 MSPS
echo -n "rate 10" | nc -w 1 192.168.1.135 5000

#### set sample rate to 12.5 MSPS
echo -n "rate 12.5" | nc -w 1 192.168.1.135 5000

#### set 8 bit dac width
echo -n "width 8" | nc -w 1 192.168.1.135 5000

#### set 16 bit dac width
echo -n "width 16" | nc -w 1 192.168.1.135 5000


192.168.1.135 ip addr of smisdr device (raspi4)

The streaming daemon instantly displays the control data from port 5000 and adjusts the signal processing of the SMI accordingly in real time.  

```console
pi@SMISDR:~/smisdr $ sudo ./smi_tcp_streaming_dac
[DATA] Warte auf Netzwerk-Stream auf Port 1234...
core_f=250000496
[CTRL] Update: Ziel 5.00 MSPS -> Real 5.0000 MSPS (Cycles: 25 [6/13/6])
[DATA] Client verbunden!
```
Because the secondary memory interface is DMA-driven, the CPUs remain practically idle even at higher RF data rates via ADC/DAC. This allows - similar to the Red Pitaya - the installation of custom SDR applications on the "smisdr" device to, for example, filter, resample or frequency-shift the incoming data stream using software NCOs. An option for a digital up- and down-converter directly within low-cost FPGA HATs you will find in the gateware folder. This allows I/Q samples to be transmitted directly via the SMI bus; thanks to the FPGA's more powerful NCOs, these samples can then be shifted across a much wider frequency range. This benefits smaller smiSDR systems that are not based on the Raspberry Pi 4.

![htop](https://github.com/radiolab81/smisdr/blob/main/www/htop.jpg)*htop on rpi4 while streaming RF over GigabitEthernet to SMIDAC*

Example app:

- a small I/Q WAV player (or recorder) for the COHIRADIA project (https://www.cohiradia.org/de/ or https://github.com/radiolab81/COHIRADIAStreamer), see https://github.com/radiolab81/cohiplayer_smi

The smisdr represents a very cost-effective approach/replacement for the ADALM2000, Red-Pitaya, or the obsolete FL2000 USB-VGA adapter (8-bit only). Usable DACs range from simple 8-10 bit R2R-ladder-DACs to common 12-14 bit parallel-DACs (and ADCs) from manufacturers like Analog, Microchip, or TI.

The ability to easily build your own HATs with the desired bit width provides access to all frequency ranges within signal bandwidth corresponding to the Secondary Memory Interface.

While the Raspberry Pi 4 is the ideal candidate for streaming via SMI, older/smaller RPi versions (3B, Zero 2) with reduced performance could still be a viable option. Although these lack fast Gigabit Ethernet, they can be operated in USB Ethernet Gadget mode! In this mode, the Raspberry Pis are connected directly to the streaming computer via USB, running apps like GNU Radio. They themselves become an Ethernet card and can therefore receive/transmit data via TCP/IP . This is the only way to achieve low-latency, high-speed I/O on these boards; don't even think about using the Wi-Fi on these boards, it would be a frustrating endeavor. Of course, with these Raspberry Pi versions, despite the DMA-driven SMI I/O, the possibilities regarding additional internal signal processing/generation are limited (for example, via additional apps). They don't come close to the "RPi4 flagship". Forget about single Core Raspberry Pi (Pi 1 or Zero) unless you have too much time to waste. 

The Raspberry Pi 5? An interesting candidate with one very serious flaw for this project: it lacks a secondary memory interface. The Pi 5's high-speed I/O is more akin to the Raspberry Pi Pico with PIO. Very interesting and usable, but better suited to a different, specific project.

How do I activate USB Ethernet gadget mode?

Add the following to the very bottom of your config.txt file:  `dtoverlay=dwc2`

```console
# arm_64bit=0 -> not for trixie (deb13)
[all]
dtoverlay=smi
dtoverlay=smi-dev
arm_64bit=0
core_freq=250
core_freq_min=250
force_turbo=1
dtoverlay=dwc2
```
In the cmdline.txt file, add the following after rootwait: `modules-load=dwc2,g_ether`

The entire file should look something like this:

```console
console=serial0,115200 console=tty1 root=PARTUUID=7416d161-02 rootfstype=ext4 fsck.repair=yes rootwait modules-load=dwc2,g_ether
```

Then restart your pi. The Raspberry Pi should be connected directly to the USB port of the RF-streaming PC. If necessary, use a USB power splitter cable to supply power to the Raspberry Pi if your USB port cannot provide the required current. Note that not all USB ports on a Raspberry Pi are usable for this USB-to-Ethernet tunnel!

***In all cases, however, the same principles apply as with other SDR applications: use short, high-quality data cables; best Gigabit Ethernet connections are useless if the RF data has to be transmitted over a poor network or a congested router; use direct connections whenever possible. When reading or writing RF data from storage media, use FAST(!) storage media and ensure that the data is stored unfragmented(!) on the medium.***

For an SD card image of an already installed smisdrOS see release.


## Using with GNU Radio

smisdr talks to the outside world over two independent, plain TCP sockets — there is no proprietary framing to deal with, which makes it trivial to drive from GNU Radio.

| Port | Direction | Content |
|---|---|---|
| **1234** | data | Raw, pre-computed RF/baseband samples, 8-bit or 16-bit, sent straight through as a byte stream. `smi_tcp_streaming_dac` just forwards whatever arrives to the SMI/DAC bus at the currently configured rate and bit width. |
| **5000** | control | Plain ASCII text commands (`rate <MSPS>`, `width 8`, `width 16`) sent over short-lived connections, one command per connection, then the socket is closed again. |

Because port 1234 expects nothing but finished samples, any flowgraph that ends in a **TCP Sink** connected to smisdr's IP on port 1234 will stream. For 8-bit widths, convert your samples to `byte`/`unsigned char` before the sink; for 16-bit widths, convert to `short`. No GNU Radio-side smisdr blocks are required for this base package — it's just I/O.

The control port is the part that isn't obvious from the data path alone, so here's how a companion flowgraph typically drives it:

### Control flowgraph pattern

1. **Rate control (`rate <MSPS>`)**
   A GNU Radio variable (e.g. a `QT GUI Range` bound to `samp_rate`) is fed into a `Variable to Message` block. This block only fires a message when the variable's value actually *changes* at runtime — it is not a timer — so a small Python block can open a short TCP connection to port 5000 and send `rate <value>` (scaled from Hz to MSPS) exactly when the user moves the slider. On flowgraph start, a **Python Snippet** (`Section: Main - After Start`) sends the current rate once, so the device and the GUI start in sync without needing a timer or a repeated send.

2. **Width control (`width 8` / `width 16`)**
   Two `QT GUI Message Push Buttons` each publish a `pressed` message when clicked. Each is wired to its own small Python message-sink block that ignores the message content — the arrival of *any* message is the trigger — and simply opens a short TCP connection to port 5000 and sends a fixed command (`width 8` or `width 16`).

3. **Connection handling**
   Every command uses its own brief connection (open → send → close), matching the way the daemon expects control input (equivalent to `echo -n "rate 5" | nc -w1 <ip> <port>`). Connection errors are caught so a temporarily unreachable device doesn't crash the flowgraph — it just logs and continues.

None of this needs a custom OOT block; it's ~30 lines of Python per command type using the standard library `socket` module, wired up with GNU Radio's message-passing blocks (`Variable to Message`, `QT GUI Message Push Button`, `Message Debug`, or a custom `Embedded Python Block`).

An example (`smisdr_control.grc`) implementing exactly this pattern — sample-rate slider with change-only updates, an initial rate push on start, and two width buttons — can be found in `/grc/`.

![grc_ex](https://github.com/radiolab81/smisdr/blob/main/www/grc_control_smisdr.png)

### Extension package: in-band signaling with `gr-smisdr`

The control-socket pattern above applies to the **base package**, where the FPGA/DAC only ever sees finished RF samples and rate/width changes go over the separate port-5000 control channel.

The **extension package** (FPGA DUC/DDC gateware) multiplexes rate and NCO-shift commands *in-band*, inside the 16-bit sample stream itself, using the `smisdr.encoder`/`smisdr.decoder` GNU Radio OOT blocks documented in [`gateware/gr-smisdr/README.md`](gateware/gr-smisdr/README.md). If you're working with the DUC/DDC-capable hardware, use those blocks (and their `cmd` message ports / `set_shift()` / `set_sample_rate()` calls). 
Because sending I/Q interleaved samples doubles data rate on smi bus, its importent to double the sample rate for smi configuration on port 5000 (2x rate of i/q baseband file)!


