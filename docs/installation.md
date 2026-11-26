# Installation and configuration

This is very easy. An SD card of any size is needed, formatted to a single FAT partition (FAT32 for cards over 2 GB). Every image build (Make Run-Time Image) creates an `sdcard` folder in the Platform Builder release directory with everything the card needs; copy the **contents** of that folder to the root of the card:

    bootcode.bin       Raspberry Pi firmware
    start.elf          Raspberry Pi firmware
    fixup.dat          Raspberry Pi firmware
    start_cd.elf       cut-down firmware, used with gpu_mem=16
    fixup_cd.dat       cut-down firmware, used with gpu_mem=16
    LICENCE.broadcom   firmware licence
    config.txt
    kernel.img

If SD card persistence is enabled, `Windows`, `Program Files` and other folders will also be part of the `sdcard` folder. The SD card becomes the root file system, and the files above will be visible in My Device. To fix this, set the system FAT file attribute to all the system boot files.

The firmware files and `config.txt` live in [raspi2/files/sdboot](raspi2/files/sdboot); the firmware comes from the official Raspberry Pi firmware repository ([raspberrypi/firmware](https://github.com/raspberrypi/firmware/tree/master/boot)) and is redistributed under its licence (`LICENCE.broadcom`). I was a little surprised that the latest work just fine, I expected I'd have to dig up the old boot files from 2013.

The `config.txt` file is quite short:

    hdmi_force_hotplug=1
    disable_overscan=1
    init_uart_clock=3000000
    gpu_mem=32

None of these config lines are needed to boot the kernel, but do come in handy:

- `hdmi_force_hotplug=1` makes the display work even if it's not attached at power on time
- `disable_overscan=1` makes the image fill the entire screen (needed at 1080p)
- `init_uart_clock=3000000` sets the UART clock to 3 MHz (the default is 48 MHz)<br>
needed so the UART works properly at 115200 baud (future investigation into configuring UART clock dividers for 115200 @ 48 MHz needed)
- `gpu_mem=32` reduce amount of memory allocated to the GPU to free up more memory for general use - may be removed or changed later depending on how the Video driver evolves

### Composite (TV) output

To use the composite TV output instead of HDMI, replace the first two lines of `config.txt` with:

    hdmi_ignore_hotplug=1
    sdtv_mode=2
    sdtv_aspect=1

- `hdmi_ignore_hotplug=1` uses composite even if an HDMI display is connected. `hdmi_force_hotplug=1` must be removed, as it keeps the output on HDMI
- `sdtv_mode` selects the TV standard: `0` NTSC, `1` Japanese NTSC, `2` PAL, `3` Brazilian PAL
- `sdtv_aspect` selects the aspect ratio: `1` 4:3, `3` 16:9
- `disable_overscan=1` is left out, so the firmware's default border keeps the edges of the desktop visible on a TV. Put it back if your TV shows the whole picture

No changes to the CE image are needed: the BSP asks the firmware for the display size at boot, so the desktop runs at the TV resolution. The composite signal comes out of the yellow RCA jack on the Pi 1 Model B, the 4-pole 3.5 mm jack on the B+/A+, and the unpopulated "TV" pads on the Pi Zero.

`kernel.img` is the built CE kernel together with it's ROM file system. It is `NK.nb0` without romimage's padding (cut at the real image end), made by `PostMakeimg.bat` (through `mksdcard.ps1`); `NK.nb0` itself stays in the release directory. With the "SD card as root file system" catalog item, the `sdcard` folder also holds the desktop shortcuts and startup folders (see [docs/sdroot.md](docs/sdroot.md)). The image is linked at offset `0x80008000` and is loaded in the same way as the Linux kernel.
