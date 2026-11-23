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
- The VM builds straight from this working tree (no commit or pull needed to test). The user
  builds, copies `kernel.img` to the SD card, boots the Pi and reports results (serial/KITL logs,
  photos of the screen, build logs, `bootlog.txt` in the repo root, which is gitignored).
- After every change, say exactly what to build, what to expect, and which log lines or
  behaviour confirm success. Prefer small, separately testable changes.
- **Only tested code gets committed** (since 2026-11-23). Leave changes uncommitted until the user
  reports a successful test, then commit when asked (feature work goes on a branch, e.g. `vc4-gpu`).
- Never claim hardware behaviour you have not verified. State assumptions plainly and say how to
  test them. If a register or bit is uncertain, say so.

## File rules (the build tools are old and fragile)
- Build-system and source files must use **CRLF** line endings and plain ASCII, with a trailing
  newline: `dirs`, `sources`, `makefile`, `sources.cmn`, `*.c`, `*.cpp`, `*.h`, `*.s`, `*.inc`,
  `*.def`, `*.bib`, `*.reg`, `*.dat`, `*.bat`. `.gitattributes` enforces this on checkout. Do not
  use tools that rewrite line endings, and check diffs for LF-only files.
- Never touch binary files (`*.bmp`, `*.suo`, `Thumbs.db`, the Pi firmware in `files/sdboot`).
- `build.exe` is the original 2009 binary and has crashed with heap corruption, so avoid
  exotic `sources` syntax, very long lines or non-ASCII characters.
- Do not change the memory map (`files/config.bib`, `INC/image_cfg.*`) without asking.

## Repository layout (under `raspi2/`)
- `raspi2.bat`: environment for the BSP. Sets `IMGFLASH=1` (kernel loaded from the SD card
  without a bootloader) and `BSP_NOTOUCH=1`.
- `files/`: `config.bib` (memory map), `platform.bib`, `platform.reg`, `PostMakeimg.bat`
  (runs `mksdcard.js`: builds `<release dir>\sdcard`, a complete card: everything in
  `files/sdboot` (Pi firmware, `LICENCE.broadcom`, our `config.txt`), `kernel.img` (a copy of
  `nk.nb0`) and the `initobj.dat` folders/shortcuts when `BSP_SDROOT=1`; copy its contents to a
  blank FAT32 card). The firmware must stay in the `sdboot` subfolder: makeimg merges every
  top-level `*.dat` in the release dir, and `fixup.dat` is binary (`.gitattributes` marks
  `files/sdboot` firmware as binary; the `*.dat` CRLF rule would corrupt it).
  `PostFmergeBib.bat` (runs `sdoffload.js`): with `BSP_SDOFFLOAD=1` drops the modules listed as
  `; SDOFFLOAD` in `platform.bib` from `ce.bib` before romimage; `mksdcard.js` puts them in
  `sdcard\Windows`. With the Microsoft "ROM-only File System" item (no `fsysram`) and
  `BSP_SDROOT=1`, the card is also the bootable volume: registry hives persist in the card's
  `\Windows\Registry` (OAL answers `IOCTL_HAL_GET_HIVE_CLEAN_FLAG` "keep"), and `config.bib`
  shrinks `FSRAMPERCENT` (both through `@CESYSGEN` conditions on `FILESYS_FSYSRAM`). See
  `docs/sdroot.md`.
- BSP drivers are opt-in catalog items (`raspi2/Catalog/raspi2.pbcxml`, 2026-11-23): `BSP_USBHOST`
  (dwchci), `BSP_ETHERNET` (smsc95xx, nested inside `BSP_USBHOST`), `BSP_SDHOST` (sdhcbcm, replaced our
  old opt-out `BSP_NOSD`), `BSP_AUDIO`, plus `BSP_USEVIDASDEBUGPORT` (OAL on-screen debug output for
  no-KITL images). Display drivers are two mutually exclusive items (`<ChooseOneGroup>`, Location
  `Device Drivers\Display`): `BSP_VC4DISP` (vc4disp) and `BSP_RASPIDISPLAY` (raspiBasic_ddi), replacing
  the old opt-out `BSP_NORASPIDISPLAY`; `platform.bib`/`.reg` let vc4disp win if both are set; with
  neither the image is headless (untested). `BSP_NODISPLAY` (standard, `common.bib`/`.reg`) stays the
  outer gate. `BSP_NOUSB` is a **standard** variable read by the public `common.bib`/`common.reg`
  (drops the whole USB stack), so keep it as an outer gate, never repurpose it. The sample design
  `RaspiCE/RaspiCE/RaspiCE.pbxml` selects the driver items in all three configurations.
  Core OS dependencies live in `raspi2/cebasecesysgen.bat` (`if "%BSP_X%"=="1" set SYSGEN_Y=1`,
  the stock IMX313DS pattern; cesysgen.bat calls it first in each pass). Tested 2026-11-23: a
  `<SysgenVariable>` on a BSP item does NOT make PB show or select dependencies.
- `src/INC/`: `BCM2835.h` (peripheral registers), `dwcRegs.h` (DesignWare OTG registers,
  unverified against hardware), `image_cfg.h/.inc`, `bsp_cfg.h`.
- `src/oal/oallib/`: `startup.s` (GPIO and UART setup), `init.c` (OEMInit, powers USB on through
  the mailbox), interrupt and debug code. The OAL already maps a USB host SYSINTR to VideoCore
  IRQ 9.
- `src/kitl/` and `src/MyKitlCore/`: KITL over the PL011 UART (`kitlSerDrv.c`).
- `src/drivers/raspiBasic_ddi/`: original display driver (framebuffer through the mailbox), kept as a
  fallback. `src/drivers/vc4disp/`: its successor (see "Display driver" below).
- `src/bootloader/sboot`, `sloader`: bootloaders (not used when `IMGFLASH=1`).

## Boot and hardware facts learned so far
- SD card root (FAT32): `bootcode.bin`, `start.elf`, `fixup.dat`, `kernel.img`, `config.txt`
  (all generated into `<release dir>\sdcard` by the build; `config.txt` source is
  `files/sdboot/config.txt`).
- `config.txt` needs:
  - `hdmi_force_hotplug=1` (display).
  - `init_uart_clock=3000000`. The BSP hard-codes the UART divisors (integer 1, fractional 40,
    in three `startup.s` files) for a 3 MHz UART clock. Current firmware defaults to 48 MHz. For
    that clock the divisors would be 26 and 3.
- KITL runs over **UART0 (PL011)**: GPIO14 TX, GPIO15 RX, GPIO17 RTS, 115200 8N1, hardware flow
  control in Platform Builder. The README calls it "UART1" but the code uses UART0.
  `UART_readData Error! 500` means a baud-rate mismatch.
- KITL is on by default (`IMGNOKITL=1` disables it). The boot waits for Platform Builder.
- The image links at `0x80008000` when `IMGFLASH=1`. The whole `kernel.img` is resident RAM
  (about 25 MB for a Checked image). `NKLEN` is only a ceiling (`AUTOSIZE=ON` returns the unused
  part to RAM).
- Main memory size (2026-11-22, verified): `OEMInit` (`init.c` `GetMainMemoryEnd`) sets
  `dwMainMemoryEndAddress` from the firmware "get ARM memory" property tag (`0x00010005`, via
  `OALMailboxPropertyTag` in `ioctl.c`), falling back to 128 MB if the query fails. The value must be
  exact: the kernel only shrinks it during its RAM probe, and VideoCore memory is real RAM the probe
  would accept. `config.bib` `RAM` ends at `0x88000000` as the baseline. Measured: Pi 1 Model B
  (256 MB) with `gpu_mem=32` -> RAM ends at ~`0x8E000000`, ~199 MB for CE (was ~103 MB); Pi Zero
  (512 MB) with default GPU memory -> ~422 MB. The `[OAL]` log line from `OEMInit` is not in the KITL
  log (KITL is not connected yet); confirm from the kernel's `Total Memory Sections` line.
  `gpu_mem=16` does not boot with only `start.elf`/`fixup.dat` on the card (LED 4 flashes; probably
  needs the cut-down `start_cd.elf`/`fixup_cd.dat`, untested).
- Status LED is GPIO16, which is not the Zero's activity LED.
- Boot splash and display resolution (2026-11-23, verified): `OEMInitDebugSerial` asks the firmware
  for the physical display size (property tag `0x00040003`, `GetBootDisplaySize` in
  `src\Common\VidConDebugOut\video.cpp`) and allocates the framebuffer at that size. The query runs
  before the cache globals exist, so it uses the uncached RAM view `0xA0002800` and bus alias
  `0xC0002800` with no cache maintenance, and that works. `VID_WIDTH`/`VID_HEIGHT` are only the fallback. The
  desktop (`raspiBasic_ddi`) takes its mode from `GetVideoSettings`, so it runs at native resolution
  (verified on 1920x1080 and 1680x1050 monitors). `bootsplash.cpp` draws the splash: centred logo
  `splash.bmp` + marquee bar (`ldbar.bmp`/`ldfill.bmp`, animated from the Timer 3 tick ISR, stopped
  by `ddi.c` `SetMode`); in on-screen log mode (KITL / `BSP_USEVIDASDEBUGPORT=1`) only `splashsm.bmp`
  top-left, with the full-width console scrolling below it. The BMPs are uncompressed (`U`) FILES
  entries in `platform.bib`.

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
- The old boot-log line `OEMInterruptEnable: ERROR enabling unknown SysIntr=11` was the kernel
  enabling `SYSINTR_IST_BGT` (`nkintr.h`: `SYSINTR_DEVICES`+3). `intr.c` now accepts every
  kernel-defined SYSINTR below `SYSINTR_FIRMWARE` (except `RESCHED`) as a software no-op
  (`IsSoftwareSysIntr`, same pattern as the VM tree's `platform\ipod6g`).

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
- **M1 done and verified on a Pi Zero** (PIO path; `DisableDMA=1`). The PDD
  (`src\drivers\sdhcbcm`, prefix SHC) over the CE standard MDD brings the stack up: card
  enumerates through `sdbus`/`sdmemory`, the partition mounts and a **Storage Card** volume is
  browsable with file read/write. Verified on both a 1 GB SDSC card (`Profile=SDMemory`,
  NumSectors=2010112) and a 4 GB SDHC card (`Profile=SDHCMemory`, high capacity, NumSectors=7736320).
  - **Data-timeout-clock trap (the one real bug found in M1).** The Arasan's data-timeout counter
    runs off **SDCLK**, not a separate TMCLK (FreeBSD `bcm2835_sdhci`:
    `SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK` + `SDHCI_QUIRK_BROKEN_TIMEOUT_VAL`). `TimeoutClockFrequency`
    was first set to the 200 MHz base clock; the MDD then computes a Timeout Control value of `0x10`
    to hit its 2.7 s target, which **overflows the 4-bit TC register field to 0** (the minimum
    ~41 us window). A fast card returns data inside 41 us (the 1 GB SDSC card mounted), but a slower
    4 GB SDHC card fails **every** data transfer with a Data Timeout (`ERROR INT STATUS=0x10`,
    CMD6/CMD17), so the MBR cannot be read and the volume will not mount. Fix: set
    `TimeoutClockFrequency` = 25 MHz (`0x017D7840`, the SD data clock) in `platform.reg`; the MDD then
    computes TC `0xD` (log line `Using timeout control value of 0xd`) - in range, real window ~2.7 s.
    `BaseClockFrequency` stays 200 MHz (it drives the clock divider: 25 MHz data clock, 781250 Hz ID
    clock). The ID clock is out of spec (wants <=400 kHz; 8-bit divider caps the minimum); both cards
    tolerate it - revisit with the SDHCI-3.0 10-bit divider in M3.
  - **Build trap (bit us twice).** `makeimg` builds the image registry from the copy of
    `platform.reg` in the **flat release dir**; it does **not** refresh it from the platform tree. A
    `platform.reg` edit needs **Copy Files to Release Directory** (or a manual copy into
    `...\RelDir\raspi2_ARMV6_Debug\`) **before** `makeimg`, or the stale value is baked in. Confirm by
    the `timeout control value of 0x..` log line changing. (Same lesson as the M1 SD-DLL packaging.)
- **M2 (DMA) attempted and abandoned - built-in SDHCI DMA does not work on this silicon.**
  The shipping SD driver is **PIO only** (`DisableDMA=1`); the card mounts and reads/writes reliably.
  Do not re-enable `DisableDMA=0` expecting it to work.
  - What was tried: a `CSDHCSlotBcmSDMA` subclass (swapped in via an overridden `HandleInsertion`)
    that biased the MDD's page physical addresses to VideoCore bus addresses before the SDMA System
    Address register write, plus enabling the OAL `IOCTL_HAL_GET_CACHE_INFO` handler (see below).
  - What happened: the address translation was verified correct (`bus0 = pa0 + offset`), but the
    first multi-block DMA read **hangs** - the engine arms and the command issues, yet no SDMA
    boundary or Transfer Complete interrupt ever fires and no data moves. Tried both the
    `0x40000000` (L2-coherent) and `0xC0000000` (uncached) bus aliases; identical hang, so it is not
    addressing or cache coherency.
  - Root cause: the BCM2835 Arasan's EMMC FIFO needs **DREQ flow control**, which the controller's
    built-in SDMA (and ADMA2) master does not provide. FreeBSD `bcm2835_sdhci.c` and the Linux
    drivers therefore **never use SDHCI SDMA/ADMA2**; they drive SD data through the **external
    BCM2835 DMA engine** (DREQ-paced, `BCM_DMA_DREQ_EMMC`), with PIO for sub-block transfers. The CE
    standard MDD only knows built-in SDMA/ADMA2, so it cannot drive DMA on this controller at all.
  - **Future DMA path (not done):** integrate the external BCM2835 DMA engine (the DREQ-paced
    controller behind `src/INC/DMA.h`) and feed the SD data path directly, bypassing the MDD's
    transfer machinery - the CE equivalent of FreeBSD's `SDHCI_PLATFORM_TRANSFER` hooks (which the
    MDD does not expose). This is a milestone in its own right.
  - **Kept from the M2 work:** the OAL now answers `IOCTL_HAL_GET_CACHE_INFO`
    (`src\oal\oallib\ioctl.c`, the handler line was commented out) via the common
    `OALIoCtlHalGetCacheInfo` over `g_oalCacheInfo`. It was broken (returned FALSE -> `CeGetCacheInfo`
    failed), which a future DMA path would need; it is harmless to PIO.

## Display driver: vc4disp (started 2026-11-23)
- `src\drivers\vc4disp` (`vc4disp.dll`, class `Vc4GPE`, GPE based) replaces `raspiBasic_ddi` step by
  step; the old driver stays as the fallback catalog item.
- How both work today: the OAL allocates the framebuffer at boot (`InitVideoSystem`, 16 bpp =
  `VID_BITSPP`) and describes it as the single mode of `IOCTL_HAL_DDI` (`src\oal\oallib\ddi.c`). The
  `phFrameBase` field carries the **uncached static VA** (`0xA0000000 | pa`), not a physical address.
  `DDHAL_COMMAND_SET_MODE` only stops the on-screen console/splash; `SetPalette`/`Power` are stubs.
  All drawing is GPE software emulation with a software cursor.
- **D0 done and verified (2026-11-23, Pi 1 Model B):** vc4disp loads and boots to the desktop; log
  line `vc4disp: mode 1 1920x1080 16 bpp stride 3840 fb 0xae7fe000` (GWES passes the OAL's `modeId`
  1, which the OAL ignores; `fb` is the uncached `0xA...` view as expected).
- **D1 done and verified (2026-11-23, Pi 1 Model B):** GDI draws through the cached static view
  (`0x8...`, the OAL address table maps all 512 MB) and `FlushRect` cleans the D-cache over the touched
  rows after each blit (`BltComplete`, dst rect), line (bounds + 1 px) and cursor draw/erase. Registry
  `CachedFrameBuffer` (default 1, 0 = old uncached). UI renders correctly. `gdibench.exe`
  (`src\apps\gdibench`, `BSP_GDIBENCH`) at 1920x1080x16, uncached -> cached, kpix/s: Fill 21430 ->
  46071, Invert 1635 -> 2146, Scroll 40135 -> 80575, Text 5022 -> 5609, Upload 36714 -> 43319,
  Readback 21779 -> 40546.
- GPE emulation fast paths at 16 bpp (`public\common\oak\drivers\display\emulrotate\ebbltsel.cpp`
  `EmulatedBltSelect16`): BLACKNESS/WHITENESS, solid PATCOPY, text (0xAAF0), same-format SRCCOPY/SRCAND/
  SRCPAINT/SRCINVERT, 1/4 bpp -> 16 copies. Everything else (DSTINVERT, format conversion `pConvert`
  e.g. 24/32 bpp DIB -> screen, stretch, transparent, alpha) falls to the generic per-pixel path:
  ~2 Mpix/s (the Invert result), i.e. ~1 s per full 1080p screen. Measured with the extended gdibench
  (D1 driver, kpix/s): Up565 42839, Up24 2055, Up32 1862, Stretch 1659, Transp 1767, AlphaC 1003,
  AlphaP 740 - this is why IE7 takes ~1 s to paint a page.
- `GPE::EmulatedBlt` (`gpe\swblt.cpp` `SelectBlt`) already tries three libraries inside `gpe.lib`:
  `FastBltSelect` (`emulrotate\fastblt.cpp`: 32->565 copy, 32->565 and 16->16 alpha, 16->16
  transparent, ...), `EmulatedBltSelectNN`, and GenBlt (`genblt`, a run-time blit code generator; it
  skips alpha and transparent). **`FastBltSelect` returns at once unless the global `g_fUseFastBlt`
  (C++ `?g_fUseFastBlt@@3HA`, default FALSE, nothing in the public tree sets it) is TRUE**; the driver
  sets it (registry `FastBlt`). GenBlt's ARM generator is compiled only under `USE_GENERATED_BLTS`
  (`cpulibs\arm\genblt_cpu\genblt_arm.cpp`); whether the shipped lib has it is unknown, so the driver
  logs `g_CpuFuncs.pfnPCodeToCpuCode`. GPE fills a surface's mask palette (`FormatPtr()`) from the
  XLATEOBJ on first use (`ddi_if.cpp` `SetSurfaceFormat`), which the 565 checks rely on.
- **FastBlt measured (2026-11-23, Pi 1 Model B):** log `vc4disp: FastBlt on, GenBlt code generator off`
  (the shipped ARMv6 GenBlt has no code generator). kpix/s off -> on: Stretch 1659 -> 11943, Transp
  1767 -> 20525, AlphaC 1003 -> 5954, AlphaP 740 -> 3862. Up24/Up32/Invert unchanged (no FastBlt
  routine; FastBlt's 32->565 copy wants a source with an alpha mask, plain 32 bpp DIBs lack one), so
  `vc4blt.cpp` adds the driver's own 24/32 bpp -> 565 copies and 16 bpp DSTINVERT (registry
  `DriverBlt`), gated by probing GPE's colour converter with known colours (`ConverterIs888To565`).
- **DriverBlt verified (2026-11-23, Pi 1 Model B):** kpix/s before -> after: Up24 2082 -> 16023, Up32
  1882 -> 13290, Invert 2145 -> 26607. gdibench's three source tiles (565/24/32 bpp) match, UI and
  IE7 render correctly, IE7 paints pages much faster. Remaining: some lag between mouse-wheel scroll and
  the screen update in IE7 (not investigated).
- Plan: D0 copy + clean-up (no behaviour change);
  D1 speed up drawing (GDI reads from uncached memory: cached shadow surface or write-combined
  mapping); D2 driver-owned mode set through a generic mailbox property-tag IOCTL (16/32 bpp, blanking
  for `PowerHandler`); D3 firmware hardware cursor (tags `0x00008010/11`, unverified); D4 DMA fills and
  copies (bus addresses, interrupt, free channel; raspiBasic's disabled code used channel 1, which the
  Linux DT mask `0x7f35` reserves for the VideoCore - check); D5 optional DirectDraw flip/vsync. V3D
  (OpenGL ES) is out of scope.

## Power: idle, restart, shutdown (2026-11-21)
- **Idle:** `OEMIdle` (`src\oal\oallib\power.c`) does drain-write-buffer + CP15 WFI
  (`RaspiCpuWaitForInterrupt` in `startup.s`). It accumulates `curridlehigh:curridlelow` in 1 us
  system-timer counts, with `idleconv`=1000, so `GetIdleTime()` is in ms. Verified: builds, boots,
  and a power meter shows a small drop at idle.
- **Restart / shutdown (verified on a Pi 1 Model B):** both go through the PM watchdog block at
  `0x20100000`.
  - Verified: a file copied USB -> SD before Suspend survived the halt (checksum matched; the
    kernel logs `FSREG: Flushing registry` / `FSRAM: Flushing cached file data` before
    `Calling OEMPowerOff...`). Restart and Power off from `poweroff.exe` work. The board stays up
    when idle for more than 5 minutes.
  - `OEMPowerOff`'s own log line never shows: KITL is already powered down by then. The kernel's
    `Calling OEMPowerOff...` is the last line before the halt.
  - Registers: `RSTC` +0x1C, `RSTS` +0x20, `WDOG` +0x24, password `0x5A000000`; constants in
    `BCM2835.h`.
  - `OALRaspiWatchdogReset(fHalt)` arms `WDOG`=10 ticks and sets `RSTC` to full reset.
  - With `fHalt` it first ORs `0x555` (partition 63) into `RSTS`, so `bootcode.bin` halts instead
    of booting.
  - `IOCTL_HAL_REBOOT` -> `OALIoCtlHalReboot` (table flag `OAL_IOCTL_FLAG_NOCS`, never returns) =
    restart.
  - `OEMPowerOff` = halt, so **Suspend is the shutdown** on this BSP. The Explorer Suspend button is
    re-enabled in `platform.reg`, and `ACSuspendTimeout`/`BattSuspendTimeout` are set to 0 so the PM
    never suspends (halts) on idle.
- **Optional apps** (`src\apps`, always built, put in the image by a `BSP_` variable / BSP catalog
  item, each with a desktop shortcut through `files\*.lnk` + `platform.dat`):
  - `idlemon.exe` (`BSP_IDLEMON`): CPU load / idle % / `GetIdleTime()`.
  - `poweroff.exe` (`BSP_POWEROFF`): resource dialog with Restart (`SetSystemPowerState`
    `POWER_STATE_RESET`, falling back to `IOCTL_HAL_REBOOT`) / Power off (`POWER_STATE_SUSPEND`) /
    Cancel.
- CE has no `FrameRect`. Use `Rectangle` with `NULL_BRUSH`.

## RTC (software clock, 2026-11-21)
- The Pi 1 has no battery-backed RTC. `src\oal\oallib\rtc.c` keeps the time from the 64-bit 1 MHz
  system timer (`CHI:CLO`, read hi/lo/hi): now = base `FILETIME` + (count - base count) * 10.
- The base is the image build date (`__DATE__`, 00:00), set lazily on first use (not in `OEMInit`).
  `OEMSetRealTime` moves the base. Nothing persists across reboots (user decision). It holds local
  time, as is usual in CE.
- Alarm: `OEMSetAlarmTime` stores a counter deadline. The Timer 3 tick ISR calls `OALRtcCheckAlarm`
  when it would otherwise return `SYSINTR_NOP`, and returns `SYSINTR_RTC_ALARM` (= 13,
  `SYSINTR_DEVICES`+5) once the deadline passes.
- Pattern from the stock `platform\common\src\common\rtc\timer\rtc.c`: `NKSystemTimeToFileTime` /
  `NKFileTimeToSystemTime` (declared in `nkexport.h` via `oal.h`) and `INTERRUPTS_ENABLE` around the
  64-bit state. `IOCTL_HAL_INIT_RTC` stays unregistered, because it would override the build-date
  base.
- The CE 7 tree from the VM is visible on the host at `Z:` (`public\`, `platform\`). Check values
  there instead of guessing.
  
## USB Ethernet driver (SMSC LAN9512, Pi 1 Model B) - working (2026-11-21)
- **Read `docs/ethernet.md` first.** Source `src\drivers\smsc95xx` (`smsc95xx.dll`): USB client for
  0424:ec00 and an NDIS 5.1 deserialized miniport in the same DLL. After attach it calls
  `NdisRegisterAdapter(L"SMSC95XX", L"SMSC95XX1")`; there is deliberately no `Linkage\Route`.
- MAC from the firmware through the OAL `IOCTL_HAL_MY_GET_MAC_ADDRESS` (property tag 0x00010003).
  Trap: an OAL change needs `src\oal` rebuilt (check `nk.map` for the new symbol), or the IOCTL is
  missing and the driver falls back to 02:52:50:69:00:01.
- Performance is CPU-bound (ARM1176). Release ~5.6 MB/s receive / ~3.8 MB/s send; Checked is about
  half (hcd2lib `CPhysMem` memsets every block in DEBUG). TX packs up to 8 frames per bulk-OUT
  transfer (`SMSC_TX_PACK_MAX`); dwchci DMAs bulk transfers directly into client buffers when the
  client passes a physical address (`fDirect` in `cpipe.cpp`). Never discard (invalidate) a 16 KB
  range before an IN: the OAL turns it into a whole-L1 invalidate per transfer.
- **Do not use the Microchip WinCE LAN95xx driver source or binaries**: its licence makes the
  source confidential and forbids reverse engineering. FreeBSD `if_smsc.c` (BSD) is the reference.

## Analog audio (Pi 1 3.5 mm jack) - working (2026-11-22)
- **Read `docs/audio.md` first.** Source `src\drivers\pwmaudio` (`pwmaudio.dll`): our PDD
  (`wavepdd.cpp`) and PWM/DMA engine (`pwmhw.cpp`) under Microsoft's prebuilt wave MDD
  (`wavemdd.lib`, linked from the public tree, not copied). Catalog items `BSP_AUDIO` (driver) and
  `BSP_AUDIOTEST` (`src\apps\audiotest`); the OS design needs Waveform Audio (`SYSGEN_AUDIO`).
- Hardware (verified on a Pi 1 Model B): GPIO40 = PWM0 = **left**, GPIO45 = PWM1 = right (ALT0); PWM
  clock PLLD/2 = 250 MHz (**PLLD/1 = 500 MHz does not work**: FIFO never drains, jack buzzes); DMA
  channel 4 (DREQ 5) -> `SYSINTR_AUDIO` = 19 (GPU IRQ 20, OAL `intr.c`).
- **One FIFO word per PWM period** (channels alternate): `range = clock / (2 * rate)` = 2834 at
  44.1 kHz. `clock / rate` plays at half speed.
- CE 7's waveapi **software mixer** sits in front of the driver (every open is 44.1 kHz 16-bit
  stereo; several apps play at once). Hence a write-cursor ring (16 x 256 frames, finished blocks
  cleared to silence) and headers completed **on copy**; completing on play and padding blocks with
  silence made playback choppy.
- IST priority 110 (`Priority256`); at 251 KITL output during boot starved it. Registry `Dither`
  (0/1/2, default 2 = TPDF + noise shaping).

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
