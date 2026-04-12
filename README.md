# smisdr
Baseband/RF over Ethernet powered by Raspberry Secondary Memory Interface for Software Defined Radio

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



