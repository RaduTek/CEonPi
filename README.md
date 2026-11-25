# Windows Embedded Compact (CE) on Raspberry Pi

This project is a Board Support Package (BSP) for the BCM2835-based Raspberry Pi single-board computers (Raspberry Pi 1 A/B, Pi Zero/Zero W), made for Windows Embedded Compact 7 (CE 7.0).

<img src="docs/images/splash_logo_2x.png" alt="Windows Embedded Compact 7 + Raspberry Pi splash boot logo" width="400"><br>

<img src="docs/images/pi_1_ce_logo.jpg" alt="Raspberry Pi 1 Model B with a 3D-printed CE logo on top of it" width="400">&nbsp;&nbsp;<img src="docs/images/pi_zero_booted.jpg" alt="Raspberry Pi Zero booted to WEC7 desktop" width="400"><br>

<small>[3D model for the CE logo](https://www.printables.com/model/1675195-windows-ce-logo)</small>

This project was started out in 2013 by **Doug Boling** (Boling Consulting Inc, 
[www.bolingconsulting.com](http://bolingconsulting.com)) over on [Codeplex](https://web.archive.org/web/20140417034241/http://ceonpi.codeplex.com/) (RIP), but development stopped at the USB host controller driver. All the important work of getting the bootloader set up, a basic display driver and debugger connection has been layed out, but due to no USB drivers it wasn't usable.

See [Original README of 2013 project](docs/codeplex_readme.md) and [original author update from 2018](http://bolingconsulting.com/2018/what-happened-to-the-raspberry-pi-project/).

I picked up work in late 2026 and got a USB host driver working, making this BSP now usable. Still there's more drivers needed to get this project into a usable state.

Images with KITL disabled (`IMGNOKITL=1`) boot very fast, around 10 seconds from the moment power is applied.

### Disclaimers

**This BSP isn't intended for any serious use.** Windows CE is very much deprecated and has been replaced everywhere with Linux and especially Android for HMI.

**AI assistance has been used to develop these drivers.** This is merely a hobby project, and I only have basic knowledge of both the CE driver stack and the actual hardware of the Pi.<br>
This README document is mostly handwritten, but architecture docs under `docs/` are predominantly AI generated.

## Board support

Most of the work was done on the Pi 1 Model B. Images are also tested on the Pi Zero W, which works because it's based on the same BCM2835 SoC.

| Board | Features |
|-|-|
| Pi 1 Model B | HDMI, USB host, Ethernet, 3.5mm Audio |
| Pi Zero (W) | HDMI, USB host |

## Installation

See [docs/installation.md](docs/installation.md)

## Hardware driver support

### Current

These are hardware features that are functional

| Feature | Driver State | Comments |
|-|-|-|
| UART | Basic | Used for KITL debugger connection with Visual Studio |
| [Audio](docs/audio.md) | Complete | Support PWM audio output on the Pi 1 Model B with dithering enhancements for quality |
| Display | Accelerated (CPU/DMA/GPU) | HDMI or composite TV output at the native display resolution (from the firmware, or set in `config.txt`), 16 bpp. Faster software drawing (cached framebuffer, optimized blits), DMA fills, VideoCore hardware cursor, direct framebuffer access for video players. DirectDraw with a hardware video overlay[^3]: YV12 video scaled and colour-converted by the VideoCore. See [catalog items](docs/catalog_items.md) |
| [USB Host](docs/usb.md) | Almost complete | Supports all speeds (low[^1], full, high), USB hubs, input devices and mass storage  |
| [SD Host](docs/sdhost.md) | Basic | Mounts FAT/exFAT partitions from the SD card, read and write. PIO only[^2] |
| [USB Ethernet](docs/ethernet.md) | Working | SMSC LAN9512 on the Pi 1 Model B: DHCP, IPv4/IPv6, firmware MAC address. ~5.6 MB/s receive, ~3.8 MB/s send (Release build) |
| Power | Basic | CPU idles with WFI. Restart through the SoC watchdog; Start > Suspend shuts down (firmware halt, power cycle to restart). Optional apps: CPU idle monitor, Shut Down dialog |
| Real-Time Clock | Software | The Pi has no battery-backed RTC. The clock runs from the 1 MHz system timer and starts at the image build date on each boot; time set in Date/Time lasts until reboot |

[^1]: Low-speed USB devices won't work when connected to a Full-speed USB hub directly attached to the root port.<br>
Only applies to full-speed USB 1.1 hubs and the Pi Zero, since Pi 1 has a built-in USB 2.0 hub.<br>
This is a hardware limitation of the DWC2 controller, see [docs/usb.md](docs/usb.md) for more details.

[^2]: SD transfers use PIO (programmed I/O), not DMA. The BCM2835 Arasan's built-in SDHCI DMA (SDMA/ADMA2) does not work: the controller needs the external BCM DMA engine with EMMC DREQ pacing (as the FreeBSD and Linux drivers use), which the CE standard SD host MDD cannot drive. Driving the external DMA engine is a possible future improvement.

[^3]: The overlay is a VideoCore display plane on top of the desktop, set up through the firmware mailbox (no VCHIQ). With [TCPMP](https://github.com/radutek/TCPMP) (patched for CE 7's DirectDraw) a 360p video scaled to 1080p plays smoothly without tearing at about 75% CPU, against 100% CPU and heavy tearing when drawing to the framebuffer. Decoding is still done by the CPU. There is no colour key, so the overlay covers any window over the video area; the app has to hide it when it isn't visible (patched TCPMP does). Only planar YUV 4:2:0 (YV12) is supported.

### Future goals

Things I plan on getting working, in order of priority

| Feature | Comments |
|-|-|
| GPIO, I2C, SPI, UART | Let CE apps control pins and peripherals (need to see if there's a standard CE API for this) |
| Hardware video decoding (VCHIQ) | H.264/MPEG-4 decoding on the VideoCore through the firmware's VCHIQ interface (MMAL/OpenMAX), feeding the existing hardware video overlay |
| SD Host DMA | Current SD driver is PIO only; add external BCM DMA engine support (DREQ-paced) for faster transfers |
| USB Function | Useful only on the Pi Zero, let the Pi be connected as a USB device to a computer<br>(for ActiveSync or Mass Storage) |
| USB Isochronous mode | Needed for USB audio and video class devices<br>CE only includes a USB audio driver |

### Stretch goals

These are features I don't have much hope at implementing

| Feature | Comments |
|-|-|
| SDIO Wi-Fi | Support for the built-in Wi-Fi of the Pi Zero W |
| Bluetooth | Support for the built-in Bluetooth of the Pi Zero W |
| 3D GPU Acceleration | OpenGL ES on the VideoCore IV 3D core. I just know that the driver situation on Linux is not the greatest, with Broadcom obviously being at fault |


### Other features

- Splash screen on boot as an option, instead of the KITL output
