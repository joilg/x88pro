# H96 Max RK3528

Native Armbian extension for the H96 Max RK3528 board.

The board DTS is installed through the rockchip64-7.3 archive's
dts-directories and auto-patch-dt-makefile configuration.

RK3528 TSADC support is supplied by Armbian's existing rk3528-thermal.patch.
The H96 SoC fragment intentionally does not redefine the TSADC node.

The RK3528 HDMI/VOP2 patch is active. It lives in
patch/kernel/archive/rockchip64-7.3/rk35xx-rk3528-display-hdmi-vop2.patch.
The rk35xx- prefix sorts it after the RK3562 VOP2 patch.

## CPU performance

The image selects the performance governor and requests CPU boost at boot.
The current device tree has no boost frequency, so the kernel rejects boost.
The highest available CPU frequency is 2.016 GHz. Thermal limits remain active.
The SCMI clock rounds requests from 2.112 to 2.4 GHz down to 2.016 GHz.

## SD card

The SD controller supports SDR50 and SDR104 at up to 150 MHz.
A tested card reached 63.6 MiB/s with SDR104 at 148.5 MHz.
Repeated direct reads matched, but long-term reliability remains untested.

## Front panel

The board boot script runs `h96max-early.scr` before loading the kernel.
It shows `boot` and flashes the USB icon once. Linux then flashes the icon
until the system is ready. U-Boot cannot continue flashing during kernel startup.

The wrapper loads `h96max-main.scr`, which is built from the standard
Rockchip boot script. Rebuild the script after you change it:

```sh
mkimage -C none -A arm -T script -d /boot/h96max-main.cmd /boot/h96max-main.scr
```

The kernel configuration sets the line display's initial message to `boot`.
This prevents the default scrolling kernel message during driver initialization.

## Graphics and audio

Lima provides Mali-450 OpenGL ES 2.0 and limited desktop OpenGL support.
Mali-450 does not support hardware Vulkan or OpenCL through Lima.
The separate RK3528 video decoder uses the W132D VDPU381 compatibility patch.
H.264, HEVC and VP9 test clips decoded correctly with a temporary driver and
device tree. Their output matched software decoding byte for byte.
The tests included 1080p H.264 and 720p VP9. The rebuilt image requires testing.
Chromium hardware video decoding is not configured. In testing, Chromium 156
required OpenGL ES 3 for its GPU process. Lima provides OpenGL ES 2 on Mali-450.

The analog output played a test tone. The optical output emitted light during
playback, but sound through a receiver has not been verified. The HDMI audio
card has registered, but sound through HDMI has not been verified.
The analog codec driver comes from the W132D Linux mainline port.
PipeWire mirrors analog and HDMI playback to optical output. The optical output
is active during playback. Direct ALSA playback bypasses PipeWire mirroring.
