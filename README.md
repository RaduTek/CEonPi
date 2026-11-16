# Windows Embedded Compact (CE) on Raspberry Pi

This project is a Board Support Package (BSP) for the BCM2835-based Raspberry Pi single-board computers (Raspberry Pi 1 A/B, Pi Zero/Zero W), made for Windows Embedded Compact 7 (CE 7.0).

<img src="docs/pi_zero_booted.jpg" alt="Raspberry Pi Zero booted to WEC7 desktop" height="350">

This project was started out in 2013 by **Doug Boling** (Boling Consulting Inc, 
[www.bolingconsulting.com](http://bolingconsulting.com)), but development stopped at the USB host controller driver. All the important work of getting the bootloader set up, a basic display driver and debugger connection has been layed out, but due to no USB drivers it wasn't usable.

See [Original README of 2013 project](docs/codeplex_readme.md) and [original author update from 2018](http://bolingconsulting.com/2018/what-happened-to-the-raspberry-pi-project/).

I picked up work in late 2026 and got a USB host driver working, making this BSP now usable. Still there's more drivers needed to get this project into a usable state.

Images with KITL disabled (`IMGNOKITL=1`) boot very fast, around 10 seconds from the moment power is applied.

### Disclaimers

**This BSP isn't intended for any serious use.** Windows CE is very much deprecated and has been replaced everywhere with Linux and especially Android for HMI.

**AI assistance has been used to develop these drivers.** This is merely a hobby project, and I only have basic knowledge of both the CE driver stack and the actual hardware of the Pi.<br>
This README document is handwritten, but architecture docs under `docs/` are mostly AI generated.

## Building the image

**TBA**: Description of how to install WEC7 Platform Builder and get the project ready

## Installation

This is very easy. An SD card of any size is needed, formatted to a single FAT partition. Then the following files need to be placed on it's root:

    bootcode.bin
    config.txt
    fixup.dat
    kernel.img
    start.elf

`bootcode.bin`, `fixup.dat` and `start.elf` come from the official Raspberry Pi firmware repository ([raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot)). I was a little surprised that the latest work just fine, I expected I'd have to dig up the old boot files from 2013.

The `config.txt` file is quite short:

    hdmi_force_hotplug=1
    disable_overscan=1
    init_uart_clock=3000000

None of these config lines are needed to boot the kernel, but do come in handy:

- `hdmi_force_hotplug=1` makes the display work even if it's not attached at power on time
- `disable_overscan=1` makes the image fill the entire screen (needed at 1080p)
- `init_uart_clock=3000000` sets the UART clock to 3 MHz (the default is 48 MHz)<br>
needed so the UART works properly at 115200 baud (future investigation into configuring UART clock dividers for 115200 @ 48 MHz needed)

`kernel.img` is the built CE kernel together with it's ROM file system. This file is the `NK.nb0` renamed by the `PostMakeimg.bat` script, and can be found in the Platform Builder project output folder. The image is linked at offset `0x80008000` and is loaded in the same way as the Linux kernel.

## Board support

The original project was only tested on the original Raspberry Pi Model B, since that was the only board that existed at the time. Luckily, the Pi Zero uses the same SoC, and the original BSP has booted to a desktop just fine with no modifications.

For the moment, I've only done work on the Pi Zero W, but I intend on getting some OG Pi boards to test on those as well.

| Board | Features |
|-|-|
| Pi Zero (W) | HDMI, USB host |

## Hardware driver support

### Current

These are hardware features that are functional

| Feature | Driver State | Comments |
|-|-|-|
| UART | Basic | Used for KITL debugger connection with Visual Studio |
| Display | Basic | Fixed resolution, no acceleration whatsoever |
| [USB Host](docs/usb.md) | Almost complete | Supports all speeds (low[^1], full, high), USB hubs, input devices and mass storage  |

[^1]: Low-speed USB devices won't work when connected to a Full-speed USB hub directly attached to the root port.<br>
Only applies to full-speed USB 1.1 hubs and the Pi Zero, since Pi 1 has a built-in USB 2.0 hub.<br>
This is a hardware limitation of the DWC2 controller, see [docs/usb.md](docs/usb.md) for more details.

### Future goals

Things I plan on getting working, in order of priority, after getting Pi 1 boards

| Feature | Comments |
|-|-|
| Real-Time Clock | Most of the needed code seems to be there, but the clock in the taskbar is frozen at 12:00 |
| SD Host | Support mounting FAT partitions from the SD card |
| USB Ethernet | Implement a driver for the SMSC ethernet controller hub of the Pi 1 |
| Audio | Support the built-in analog audio output of the Pi 1 model B |
| GPIO, I2C, SPI, UART | Let CE apps control pins and peripherals (need to see if there's a standard CE API for this) |
| Better Display | Support Composite output, no hardcoded resolution in CE image |
| USB Function | Useful only on the Pi Zero, let the Pi be connected as a USB device to a computer<br>(for ActiveSync or Mass Storage) |
| USB Isochronous mode | Needed for USB audio and video class devices<br>CE only includes a USB audio driver |

### Stretch goals

These are features I don't have much hope at implementing

| Feature | Comments |
|-|-|
| SDIO Wi-Fi | Support for the built-in Wi-Fi of the Pi Zero W |
| Bluetooth | Support for the built-in Bluetooth of the Pi Zero W |
| GPU Acceleration | I just know that the driver situation on Linux is not the greatest, with Broadcom obviously being at fault |


### Other features

- Splash screen on boot as an option, instead of the KITL output