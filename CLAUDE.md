# CEonPi: Windows CE 7 BSP for Raspberry Pi

## What this project is
A Board Support Package (BSP) that runs Windows Embedded Compact 7 (Windows CE 7) on the
Raspberry Pi 1 family (BCM2835, ARM1176JZF-S). It started as dboling's CodePlex project
`ceonpi`. The original author never shipped a working USB host controller driver.

**Goal:** get the Pi Zero (BCM2835, 512 MB, one micro-USB OTG port wired straight to the SoC)
booting this BSP, then write a USB host controller driver for the Synopsys DesignWare OTG
controller so keyboards, mice and USB Ethernet work.

## How we work (important)
- Claude Code runs on the host PC in a git clone of this repo. It can read and edit files but
  **cannot build or run anything**.
- The build happens in a Windows 7 VM (Platform Builder 7 + Visual Studio 2008 SP1) at
  `C:\WINCE700`. This repo is the contents of `C:\WINCE700\platform\raspi2`.
- The user pulls changes in the VM, builds, copies `kernel.img` to the SD card, boots the Pi
  and reports results (serial/KITL logs, photos of the screen, build logs).
- After every change, say exactly what to build, what to expect, and which log lines or
  behaviour confirm success. Prefer small, separately testable changes and small commits.
- Never claim hardware behaviour you have not verified. State assumptions plainly and say how to
  test them. If a register or bit is uncertain, say so.

## File rules (the build tools are old and fragile)
- Build-system and source files must use **CRLF** line endings and plain ASCII, with a trailing
  newline: `dirs`, `sources`, `makefile`, `sources.cmn`, `*.c`, `*.cpp`, `*.h`, `*.s`, `*.inc`,
  `*.def`, `*.bib`, `*.reg`, `*.dat`, `*.bat`. `.gitattributes` enforces this on checkout. Do not
  use tools that rewrite line endings, and check diffs for LF-only files.
- Never touch binary files (`*.bmp`, `*.suo`, `Thumbs.db`).
- `build.exe` is the original 2009 binary and has crashed with heap corruption, so avoid
  exotic `sources` syntax, very long lines or non-ASCII characters.
- Do not change the memory map (`files/config.bib`, `INC/image_cfg.*`) without asking.

## Repository layout (under `raspi2/`)
- `raspi2.bat`: environment for the BSP. Sets `IMGFLASH=1` (kernel loaded from the SD card
  without a bootloader) and `BSP_NOTOUCH=1`.
- `files/`: `config.bib` (memory map), `platform.bib`, `platform.reg`, `PostMakeimg.bat`
  (renames `nk.nb0` to `kernel.img`).
- `src/INC/`: `BCM2835.h` (peripheral registers), `dwcRegs.h` (DesignWare OTG registers,
  unverified against hardware), `image_cfg.h/.inc`, `bsp_cfg.h`.
- `src/oal/oallib/`: `startup.s` (GPIO and UART setup), `init.c` (OEMInit, powers USB on through
  the mailbox), interrupt and debug code. The OAL already maps a USB host SYSINTR to VideoCore
  IRQ 9.
- `src/kitl/` and `src/MyKitlCore/`: KITL over the PL011 UART (`kitlSerDrv.c`).
- `src/drivers/raspiBasic_ddi/`: display driver (framebuffer through the mailbox).
- `src/bootloader/sboot`, `sloader`: bootloaders (not used when `IMGFLASH=1`).

## Boot and hardware facts learned so far
- SD card root (FAT32): `bootcode.bin`, `start.elf`, `fixup.dat`, `kernel.img`, `config.txt`.
- `config.txt` needs:
  - `hdmi_force_hotplug=1` (display).
  - `init_uart_clock=3000000`. The BSP hard-codes the UART divisors (integer 1, fractional 40,
    in three `startup.s` files) for a 3 MHz UART clock. Current firmware defaults to 48 MHz. For
    that clock the divisors would be 26 and 3.
- KITL runs over **UART0 (PL011)**: GPIO14 TX, GPIO15 RX, GPIO17 RTS, 115200 8N1, hardware flow
  control in Platform Builder. The README calls it "UART1" but the code uses UART0.
  `UART_readData Error! 500` means a baud-rate mismatch.
- KITL is on by default (`IMGNOKITL=1` disables it). The boot waits for Platform Builder.
- The image links at `0x80008000` when `IMGFLASH=1`. `DRAM_SIZE` is 256 MB and `OEMInit` caps
  main memory at 128 MB, while the Zero has 512 MB.
- Status LED is GPIO16, which is not the Zero's activity LED.

## USB driver: current state
- **No driver source exists in this repo.** `platform.reg` references `dwchci.dll`,
  `simpusb.dll` and a placeholder for the SMSC LAN95xx Ethernet chip (VID 1060 / PID 60416), but
  none of it is built. In `platform.bib` the `uhci.dll` and `ehci.dll` lines are commented out.
- Present: `INC/dwcRegs.h`, the OAL interrupt hook, the mailbox USB power-on, and the USB block
  base address `0x20980000`.

## USB driver: progress (2026-11-15)
- **Read `USB_PLAN.md` in the repo root first.** It holds the current plan, milestone status,
  the next test step, verified hardware facts and the design of the next milestone.
- Driver source is `src/drivers/dwchci` (`dwchci.dll`, links the CE `hcd2lib`, loaded from
  `Drivers\BuiltIn\DWCUsbHost`). The milestones M0-M5 replace steps 2-4 below: after
  root-port bring-up the driver plugs straight into the CE USB stack.
- The OS design needs catalog items `SYSGEN_USB` and `SYSGEN_USB_HID_CLIENTS`. Each build
  configuration is sysgen'd separately. Use **Debug**: Release has `IMGNOKITL=1`.
- M1 done and verified on a Pi Zero (register dump at driver load):
  - `MmMapIoSpace(0x20980000)` works from a kernel-mode driver.
  - `GSNPSID=0x4F54280A` (core 2.80a), `GHWCFG2=0x228DDD50` (internal DMA, UTMI+ HS PHY,
    dedicated FS PHY, 8 channels, dynamic FIFO), `GHWCFG3=0x0FF000E8` (FIFO depth 4080 words),
    `GHWCFG4=0x1FF00020` (8-bit UTMI, so 60 MHz PHY clock; no descriptor DMA).
  - State left by the firmware: `GUSBCFG=0x20402700` (force-host already set), host mode,
    `HCFG=1`, `HFIR=48000`, `HPRT0=0` (port not powered), FIFO reset sizes RX 4096, NP TX 256,
    periodic TX 512.
- M2 done and verified on a Pi Zero: core reset, forced host mode, host init, port power and the
  OAL interrupt path (`SYSINTR_USBHOST` = 17) all work. Full, low and high speed devices are
  detected with the right speed; `HFIR` is 59999 (FS/LS) or 7499 (HS) at the 60 MHz PHY clock.
  Observed: the core raises `DISCONNINT` around a port reset while the device is still
  connected, and `CONNDET` can re-latch from contact bounce, so both must be checked against
  `HPRT0.CONNSTS`.
- M3 done and verified on a Pi Zero: `CDwcHcd` (root hub virtuals, IST, channel table) in
  `cdwchcd.cpp`, control pipes in `cpipe.cpp`, PDD in `system.cpp`. A mouse (045e:00cb) and a
  keyboard (17ef:60ba) both enumerated (descriptor read from addr 0, re-read from addr 1, real
  VID/PID), confirming the control-pipe path and `DmaBusOffset=0x40000000`.
  `HCD_SUSPEND_ON_REQUEST` is deliberately not used (no `SuspendHC` support).
- M4 written, not yet built or tested: bulk and interrupt pipes in `cpipe.cpp`
  (`CreateBulkPipe`/`CreateInterruptPipe` now return a `CDwcPipe`). Bulk reuses the control
  data-stage machinery with a per-pipe data toggle; interrupt pipes add a start-of-frame
  scheduler (`SofWaiterAdd/Remove` in `cdwchcd.cpp`, `EnableSof/DisableSof` and
  `CurFrame/FrameReached` in `dwccore.h`) that re-polls one `bInterval` apart on NAK/overrun.
  Isochronous pipe creation still returns NULL.
- Design facts worth keeping: hcd2lib calls `AddedTt` for a full/low speed device on the root
  hub too, because the root hub is declared high speed; pipes with hub address 0 need no split.
  The device address comes with each `IssueTransfer`, not with pipe creation. Client callbacks
  run outside the controller lock.
- DMA bus address offset (`DmaBusOffset` = `0x40000000`) is verified by M3.
- KITL serial output costs about 30 ms per line, so do not log per transfer attempt.
- The boot log shows `OEMInterruptEnable: ERROR enabling unknown SysIntr=11` around the time
  GWES loads. It is not caused by the USB driver (M1 enabled no interrupt); source not yet found.

## USB driver plan
1. Root port only. Power on, core reset, host init, port power and reset, detect connect and
   speed. Log everything through KITL debug output.
2. Enumerate one device with control transfers on one channel.
3. Interrupt-IN transfers for a HID keyboard.
4. Integrate with Windows CE's USB host stack as a proper host controller driver. Inspect the CE 7
   sample controller drivers under `C:\WINCE700\public\common\oak\drivers\usb` and verify the
   real folder names and interfaces there. Do not guess the HCD interface.
5. Hubs and split transactions, then a USB Ethernet class driver.

Known traps: the DWC OTG controller needs split transactions for low and full speed devices
behind a high-speed hub. DMA needs bus addresses, not CE physical or virtual addresses, so
check how the display driver passes addresses to the GPU. Cache coherency matters on the
ARM1176.

## Reference code
CSUD (Chadderz's Simple USB Driver), Linux dwc2, FreeBSD dwc_otg and the USPi and Circle
projects all drive this controller. Use them as documentation. **Check each licence before copying
code into this BSP.**

## Debugging notes
- Kernel debug output goes through KITL (`KITLOutputDebugString`, `RETAILMSG` and `DEBUGMSG`) to
  Visual Studio's Output window. Ask the user to save it to a file in the shared folder so
  you can read it.
- `build.log`, `build.err` and `build.wrn` are written next to the platform in the VM. Ask for
  them when a build fails.
