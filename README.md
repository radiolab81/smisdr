# smisdr
Baseband/RF over Ethernet powered by Raspberry Secondary Memory Interface (DMA - driven) for Software Defined Radio

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
- `smi_util.c`: Tool to read/write the current SMI settings and send/receive test data. You can build it with make (see Makefile)
- `smi_sinus.c`: Tool for generating and outputting a sine waveform at a specific data rate, can be build by build_smi_sinus.sh
- `smi_tcp_streaming_dac.c`: Main tool for streaming baseband or RF data, receives data on port 1234 from external computers/apps such as GNU Radio or from localhost with internal apps, can be build by build_smi_tcp_streamig_dac.sh

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

- `tcp_test.py`: test tool for generating and outputting a sine waveform at a specific data rate via TCP/localhost to smi_tcp_streaming_dac process


The streaming daemon instantly displays the control data from port 5000 and adjusts the signal processing of the SMI accordingly in real time.  

```console
pi@SMISDR:~/smisdr $ sudo ./smi_tcp_streaming_dac
[DATA] Warte auf Netzwerk-Stream auf Port 1234...
core_f=250000496
[CTRL] Update: Ziel 5.00 MSPS -> Real 5.0000 MSPS (Cycles: 25 [6/13/6])
[DATA] Client verbunden!
```
Because the secondary memory interface is DMA-driven, the CPUs remain practically idle even at higher RF data rates via ADC/DAC. This allows - similar to the Red Pitaya - the installation of custom SDR applications on the "smisdr" device to, for example, filter, resample or frequency-shift the incoming data stream using software NCOs.

![htop](https://github.com/radiolab81/smisdr/blob/main/www/htop.jpg)

Example app:

- a small I/Q WAV player (or recorder) for the COHIRADIA project (https://www.cohiradia.org/de/)

The smisdr represents a very cost-effective approach/replacement for the ADALM2000, Red-Pitaya, or the obsolete FL2000 USB-VGA adapter (8-bit only). Usable DACs range from simple 8-10 bit R2R-ladder-DACs to common 12-14 bit parallel-DACs (and ADCs) from manufacturers like Analog, Microchip, or TI.

The ability to easily build your own HATs with the desired bit width provides access to all frequency ranges within signal bandwidth corresponding to the Second Memory Interface.

An SD card image of an already installed smisdrOS will be available shortly.


