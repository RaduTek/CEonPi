# Catalog items

The BSP contains a bunch of optional add-ins that can be enabled using the Catalog Items View. Each one sets a `BSP_` environment variable (shown in brackets) and, through [raspi2/cebasecesysgen.bat](raspi2/cebasecesysgen.bat), makes sysgen add the Core OS components it needs ("Selects" below). These are added at sysgen time and may not show as ticked in the Catalog Items View. Items that need another BSP item say so ("Needs").

The hardware drivers are opt-in too: an OS design without the USB host, Ethernet or SD host items gets none of those drivers, and one without a display item gets no display driver. The sample OS design (`RaspiCE`) selects all three plus the VideoCore IV display driver.

- **Applications**
    - **CPU Idle Monitor (demo)** (`BSP_IDLEMON`) - shows CPU load, the % of idle CPU time and `GetIdleTime()`, with a desktop shortcut
    - **Shut Down dialog (poweroff.exe)** (`BSP_POWEROFF`) - convenient way of shutting down or restarting the OS, with a desktop shortcut
    - **Audio test (audiotest.exe)** (`BSP_AUDIOTEST`) - plays left/right test tones in several formats and reports timing; `audiotest file.wav` plays a WAV file <br> Selects Waveform Audio. Needs an audio driver
    - **Volume tray icon (voltray.exe)** (`BSP_VOLTRAY`) - speaker icon in the taskbar with a volume slider and a Mute box, started from the Startup folder <br> Selects Waveform Audio, Common Control and Standard Shell. Needs an audio driver
- **Debugging**
    - **On-screen debug output** (`BSP_USEVIDASDEBUGPORT`) - shows the kernel debug output on the screen during boot. Always on in images with KITL, so this is for images without it (Release) <br> Built into the OAL: rebuild the BSP, not just the image, after changing it
- **Device Drivers\Audio**
    - **Analog audio (Pi 1 3.5 mm jack)** (`BSP_AUDIO`) - PWM audio driver for the 3.5 mm jack of the Pi 1 (the Pi Zero has no jack), see [docs/audio.md](docs/audio.md) <br> Selects Waveform Audio
- **Device Drivers\Display** (choose one; with neither the image has no BSP display driver, which is untested)
    - **Display: VideoCore IV (vc4disp)** (`BSP_VC4DISP`) - the new display driver, where acceleration, runtime mode changes and a hardware cursor will go. For now it behaves like the basic driver: native resolution, 16 bpp, software drawing. Wins if both display items are set
    - **Display: basic framebuffer (raspiBasic_ddi)** (`BSP_RASPIDISPLAY`) - the original driver (Microsoft flat sample), kept as a known-good fallback
- **Device Drivers\Ethernet**
    - **USB Ethernet (SMSC LAN95xx)** (`BSP_ETHERNET`) - driver for the Ethernet chip built into the Pi 1 Model B/B+ (the Pi Zero has none), see [docs/ethernet.md](docs/ethernet.md) <br> Selects USB Host Support and Wired Local Area Network (NDIS, TCP/IP). Needs the "USB host controller" item
- **Device Drivers\SD**
    - **SD card host controller (Arasan eMMC)** (`BSP_SDHOST`) - mounts the boot SD card as `\Storage Card`, see [docs/sdhost.md](docs/sdhost.md) <br> Selects SD Bus Driver, SD Memory, SD Standard Host Controller, FAT File System and Partition Driver
- **Device Drivers\USB**
    - **USB host controller (DWC OTG)** (`BSP_USBHOST`) - USB host driver for keyboards, mice, hubs, mass storage and USB Ethernet, see [docs/usb.md](docs/usb.md) <br> Selects USB Host Support and USB HID Keyboard and Mouse. Add other class drivers (e.g. USB Storage Class Driver) yourself
- **Storage**
    - **SD card as root file system** (`BSP_SDROOT`) - mounts the SD card at `\` instead of `\Storage Card`, so files persist across reboots (and the registry too with the "ROM-only File System" item), see [docs/sdroot.md](docs/sdroot.md) <br> Selects SD Bus Driver, SD Memory, SD Standard Host Controller, FAT File System and Partition Driver. Needs the "SD card host controller" item
    - **Load optional modules from the SD card** (`BSP_SDOFFLOAD`) - leaves some binaries (IE7, network tools) out of kernel.img and loads them from the SD card instead, to reduce kernel.img size and RAM use <br> Selects SD Bus Driver, SD Memory, SD Standard Host Controller, FAT File System and Partition Driver. Needs the "SD card as root file system" and "SD card host controller" items
