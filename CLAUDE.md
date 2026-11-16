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
- **Read `docs/usb.md` first.** It documents the driver's current state, architecture, verified
  hardware facts, the low-speed-behind-a-FS-hub limitation and the remaining work / future goals.
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
- M4 tested: bulk and interrupt pipes in `cpipe.cpp` (`CreateBulkPipe`/`CreateInterruptPipe`
  return a `CDwcPipe`). Bulk reuses the control data-stage machinery with a per-pipe data
  toggle; interrupt pipes add a start-of-frame scheduler (`SofWaiterAdd/Remove` in `cdwchcd.cpp`,
  `EnableSof/DisableSof` and `CurFrame/FrameReached` in `dwccore.h`) that re-polls one
  `bInterval` apart on NAK/overrun. Single keyboard and mouse work. A composite device
  (Logitech Bolt 046d:c548, Microsoft 045e:0800 - both composite wireless receivers with
  several interrupt-IN endpoints) enumerated and delivered data, but exposed a disconnect wedge:
  once the device is gone the core cannot drain the periodic request queue, so polling channels
  will not halt (`CHENA` stuck), which jams the host (port will not re-enable, next device will
  not enumerate). `CDwcCore::DisableAllChannels` reports the stuck count and `HandleDisconnect`
  recovers by re-running `HostInit` + `EnableInterrupts` (a core reset) whenever any channel is
  stuck. Retest pending. Isochronous pipe creation still returns NULL.
- Split transactions (hubs): control + bulk splits (Step 2) **verified** - FS and LS devices
  enumerate behind a HS hub (`dwctt.h`/`dwctt.cpp` `CDwcTt`, one per TT via `AddedTt`; HCSPLT bits
  in `dwc2reg.h`; split fields in `DWC_CHANNEL_SETUP`/`ChannelStart`; `CDwcPipe::OnSplitInterrupt`
  SSPLIT/CSPLIT machine). HS mass storage behind a hub also works. Verified trap:
  `m_fDoSplit` must **not** key off `ttContext`, which hcd2lib leaves NULL during the addr-0
  enumeration of a device behind a hub; it is `!fIsHighSpeed && bHubAddress != 0 && root port is
  HS`. The root-port-speed gate matters: a FS/LS device behind a **full speed** hub (e.g. a Dell
  keyboard with a built-in FS hub) has **no TT** - the whole tree below a FS top-level hub is
  full/low speed and the hub is a plain repeater. Splitting to it puts malformed SSPLIT/CSPLIT
  tokens on the wire and the core disables the babbling FS root port (observed as a reset loop
  when a LS mouse is plugged into such a hub). Splits apply only when the root port negotiated
  HIGH speed, the only case where a HS hub's TT exists; otherwise the core drives the device
  directly (low speed uses PRE preambles via HCCHAR.LSPDDEV), as for a device on the root port.
  Root-port devices report hub 0. For split
  transactions the HCCHAR `MULTICNT` field is the error/retry count: 3 for interrupt/isoc, 1 for
  control/bulk (dwc2 `hcd.c:1239`), already in `ChannelStart`.
- **Hardware limit - low speed behind a full speed hub is not supported.** The BCM2835 core has a
  dedicated full speed PHY plus a UTMI+ high speed PHY (`GHWCFG2=0x228DDD50`: HS PHY=UTMI, FS
  PHY=dedicated). In that configuration the core **cannot generate low speed PRE packets while the
  root port runs at full speed**, so a low speed device behind a full speed hub is unreachable -
  the dwc2 driver refuses it (`hcd.c dwc2_hcd_urb_enqueue` returns `-ENODEV` for
  LS + dedicated-FS-PHY + UTMI-HS-PHY + full-speed root port). We did the opposite and programmed
  the channel, so the core put a low speed token on the FS bus, the device babbled and the core
  disabled the root port - a reset loop that took down the hub and every device on it (observed
  when a LS mouse is plugged into a Dell keyboard's built-in FS hub). `CDwcCore::LowSpeedUnreachable`
  detects the condition and `CDwcPipe::IssueTransfer` fails the transfer cleanly
  (`USB_DEVICE_NOT_RESPONDING_ERROR`), so only that one device fails to enumerate while the rest of
  the bus stays up. A low speed device behind a **high speed** hub (root port at HS) still works via
  splits; a low speed device **on the root port** still works (root port negotiates LS). Only
  low-speed-behind-a-FS-hub is impossible on this silicon.
- Interrupt splits (Step 3) tested on hardware: HID behind a hub delivers input (`split interrupt
  pipe ep 0x8x alive`). `CDwcTt::Reserve/Release` gives each periodic split pipe a start microframe
  from a coarse per-microframe byte budget; `OpenPipe` reserves it (releases in `ClosePipe`);
  `ScheduleNextInterval` aligns the start-split to that microframe; and the interrupt-split
  handshake paths wait one poll interval (not an immediate retry) when the endpoint has no data, so
  an idle HID endpoint does not hammer the TT. Behind a HS hub `HFNUM` counts microframes, so a
  FS/LS interrupt interval is `bInterval * 8`. Full plan in `~/.claude/plans/lucky-bouncing-frog.md`.
- Behind-hub disconnect fix: unplugging a HID device from a hub raises no root-port `DISCONNINT`,
  so the `HandleDisconnect` reset-recovery never runs; teardown goes through `AbortTransfer` ->
  `AbortOne` -> `ChannelForceOff`. `ChannelForceOff` must halt a busy interrupt-IN channel the
  same way `DisableAllChannels` does (turn it to OUT, `EPDIR=0`, with `CHENA|CHDIS`, then wait),
  or the stuck channel jams the periodic request queue and all interrupt transfers die while bulk
  (non-periodic queue) keeps working - observed as HID that never recovers after a hub unplug.
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

## SD card storage driver (goal: mount the SD card as a FAT volume in CE)
- Plan/decision: drive the **Arasan eMMC (SDHCI)** controller at `0x20300000` (GPU IRQ 62) and
  reuse the CE standard SD host MDD `SDHCBase.lib` (`CSDHCBase`/`CSDHCSlotBase`, under
  `public\common\oak\drivers\sdcard\SDHCDrivers\SDIOStdHC\base`); write only a thin BCM2835 PDD, a
  sibling of the shipped `pci` PDD. Not the Broadcom SDHOST (`0x20202000`, IRQ 56), which has no
  datasheet and no MDD. Driver source is `src\drivers\sdhcbcm` (`sdhcbcm.dll`). DMA from the start
  (the MDD has SDMA + 32-bit ADMA2 built in). Full plan: `~/.claude/plans/typed-weaving-flame.md`.
- **M0 done and verified on a Pi Zero** (probe driver, register/GPIO dump at load):
  - `MmMapIoSpace(0x20300000)` works from a kernel-mode driver.
  - `HOST_VERSION=0x9902` (spec-version field 2 = SDHCI 3.00, vendor 0x99).
  - `PRESENT_STATE=0x1fff0000`: card inserted, state stable, detect signal, write-enabled, all
    CMD/DAT lines idle-high. The card is on the Arasan.
  - GPIO 48-53 function-select = **ALT3** (`GPFSEL4=0x3f209809 GPFSEL5=0x00000fff`), which routes
    those pins to this controller. So the firmware wired the SD card to the Arasan; no pin re-route
    or `config.txt` change is needed.
  - **`CAPABILITIES`=0x00000000 (both words).** The BCM2835 Arasan does not implement the SDHCI
    capabilities register (Linux `SDHCI_QUIRK_MISSING_CAPS`; FreeBSD `bcm2835_sdhci` hardcodes caps
    and reads the clock from the mailbox). Consequence for M1: the MDD reads caps in
    `CSDHCSlotBase::Init` to derive base clock, DMA mode, Vdd windows and max block length, so the
    PDD must **override `GetCapabilities()` to synthesize caps** and **supply the base clock from
    the VideoCore mailbox** (the OAL has only the simple power mailbox; the property-tag query must
    be done driver-side, modelled on `src\bootloader\sloader\PropTest.cpp`). FreeBSD's
    `bcm2835_sdhci.c` (BSD) has the exact synthesized caps and the clock query to copy.
- **M0.5 done and verified on a Pi Zero.** Rather than a driver-side mailbox, the base clock is
  read through a new OAL IOCTL `IOCTL_HAL_MY_GET_EMMC_CLOCK` (`ioctl.c` `OALIoCtlHalGetEmmcClock`):
  the VideoCore "get clock rate" property tag for the EMMC clock, via the OAL's mailbox and the
  shared 0x2000 buffer (clean-before / invalidate-after for ARM<->VideoCore coherence; the buffer's
  cached VA 0x80002000 doubles as the bus address, as the framebuffer path does). The probe reads it
  with `KernelIoControl`. Result: **EMMC base clock = 200000000 Hz (200 MHz)** - this is
  `BaseClockFrequency` for M1.

## Reference code
CSUD (Chadderz's Simple USB Driver), Linux dwc2, FreeBSD dwc_otg and the USPi and Circle
projects all drive the USB controller. For the SD host, FreeBSD `bcm2835_sdhci.c`/`bcm2835_sdhost.c`
(BSD) and NetBSD `sdhc.c` (BSD) are the permissive references; Linux (`bcm2835.c`,
`sdhci-bcm2835.c`) and U-Boot are GPL (documentation only). Use them all as documentation.
**Check each licence before copying code into this BSP.**

## Debugging notes
- Kernel debug output goes through KITL (`KITLOutputDebugString`, `RETAILMSG` and `DEBUGMSG`) to
  Visual Studio's Output window. Ask the user to save it to a file in the shared folder so
  you can read it.
- `build.log`, `build.err` and `build.wrn` are written next to the platform in the VM. Ask for
  them when a build fails. A single-directory `build` can fail only at the *copy to the flat
  release dir* step (`NMAKE ... U1077: 'if'`, no `LNK` lines) while the link itself reports
  `0 Errors`: an active **KITL connection holds a lock on the release-dir DLL**. Stop the
  KITL/debugger connection, then rebuild. Confirm by comparing the cesysgen target DLL timestamp
  (newer) with the one in `...\RelDir\raspi2_ARMV6_Debug\` (stale).
