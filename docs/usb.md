# USB host controller driver (`dwchci`)

A host controller driver for the Synopsys DesignWare USB 2.0 OTG core (DWC2) as wired on the
BCM2835, letting Windows Embedded Compact 7 use USB keyboards, mice, hubs and mass storage on the
Raspberry Pi 1 family (notably the Pi Zero, whose single micro-USB OTG port is wired straight to
the SoC). The driver is `dwchci.dll`; it links the CE USB 2.0 host library `hcd2lib` and plugs
into the standard CE USB stack, so HID, storage and other class drivers (such as the
[USB Ethernet driver](ethernet.md)) load above it unchanged.

> This file supersedes the old `USB_PLAN.md`. It describes the driver as it stands and the work
> still ahead. Build/boot mechanics for the whole BSP live in `CLAUDE.md`.

## Current state

Everything below is implemented and **verified on a Pi Zero** unless noted. Control, bulk and
interrupt transfers work on the root port and through hubs (including split transactions for
full/low speed devices behind a high speed hub). HID input, USB mass storage (with `SYSGEN_FATFS`
to mount FAT/exFAT), multi-level hub topologies and device hot-plug all work.

| Area | State |
|---|---|
| Register mapping, core identification | Done |
| Core reset, forced host mode, host init, FIFO sizing | Done |
| Root port: power, reset, connect/speed detect (FS/LS/HS) | Done |
| Enumeration + control pipes (hcd2lib integration) | Done |
| Bulk pipes (mass storage) | Done |
| Interrupt pipes + start-of-frame scheduler (HID) | Done |
| Composite devices (multi-endpoint wireless receivers) | Done |
| Hubs: control + bulk + interrupt split transactions | Done |
| Transaction-translator bandwidth scheduler (`CDwcTt`) | Done (coarse; see future goals) |
| Hot-plug teardown, disconnect/wedge recovery, nested hubs | Done |
| Isochronous pipes | **Not implemented** (pipe creation refused) |
| Power management (suspend/resume, `IClass`) | **Not implemented** |
| Direct (zero-copy) DMA for bulk transfers with a client physical address | Done |
| USB Ethernet driver (SMSC LAN9512, Pi 1 Model B) | Done - separate driver, see [ethernet.md](ethernet.md) |

### Resolved problems worth remembering

- **Disconnect wedge (composite HID).** Once a device is physically gone the core cannot drain the
  periodic request queue, so a channel polling an interrupt-IN endpoint never halts (`CHENA` stuck,
  `CHHLTD` never fires) and jams the whole host. Recovery is a **core reset**: `HandleDisconnect`
  re-runs `HostInit` + `EnableInterrupts` whenever `DisableAllChannels` reports any stuck channel.
- **Behind-hub disconnect.** Removing a device behind a hub raises no root-port `DISCONNINT`, so
  teardown runs through `AbortTransfer` -> `AbortOne` -> `ChannelForceOff`. `ChannelForceOff` must
  halt a busy interrupt-IN channel the same way `DisableAllChannels` does (turn it to OUT,
  `EPDIR=0`, with `CHENA|CHDIS`, then wait), or the stuck channel kills all interrupt transfers
  while bulk keeps working.
- **Interrupt over-polling.** After a report completed, the client re-issued immediately and the
  pipe polled again at once, flooding the HID queue (`Queue is full. Dropping packet`, seen with a
  Microsoft low-speed mouse). `IssueTransfer` now holds a fresh interrupt poll to the `bInterval`
  boundary (`m_dwLastPollFrame`).
- **Nested-hub status-poll spin.** With the root port down, a hub's status-change thread issued
  interrupt transfers that failed synchronously and spun. Such transfers are now left *pending* on
  a down port instead of failed, so the thread blocks until the port returns or the device is torn
  down.
- **Split decision.** `m_fDoSplit` is `!fIsHighSpeed && bHubAddress != 0 && root port is HS`. It
  must **not** key off `ttContext` (hcd2lib leaves it NULL during addr-0 enumeration behind a hub,
  which deadlocks enumeration), and it must be gated on the root port running at high speed (a
  full/low speed device behind a *full speed* hub has no TT - see the limitation below).

## Hardware limitation: low speed behind a full-speed hub

A low-speed device plugged into a **full-speed** hub cannot be reached on this silicon. The BCM2835
core has a dedicated full-speed PHY alongside a UTMI+ high-speed PHY (`GHWCFG2=0x228DDD50`: HS
PHY = UTMI, FS PHY = dedicated). In that configuration the core cannot generate the low-speed PRE
(preamble) packets needed to talk to a low-speed device while the root port runs at full speed. The
Linux dwc2 driver refuses this exact case (`hcd.c dwc2_hcd_urb_enqueue` returns `-ENODEV` for
LS + dedicated-FS-PHY + UTMI-HS-PHY + full-speed root port).

`CDwcCore::LowSpeedUnreachable` detects the condition and `CDwcPipe::IssueTransfer` fails such a
transfer cleanly with `USB_DEVICE_NOT_RESPONDING_ERROR`, rather than program a channel that would
put a low-speed token on the full-speed bus, babble, and make the core disable the root port - a
reset loop that otherwise took down the hub and every device on it. Only the unreachable device
fails to enumerate; everything else stays up.

Still fine: a low-speed device **on the root port** (the port negotiates low speed), and a
low-speed device behind a **high-speed** hub (reached through the hub's TT with split
transactions).

## Architecture

`dwchci.dll` links the CE USB 2.0 host library `hcd2lib`
(`public\common\oak\drivers\usb\hcd\usb20\usb2com`), which provides the `HCD_*` stream entry
points, the USBD interface, the root-hub and external-hub logic, enumeration and `CPhysMem`. We
provide a `CHcd` subclass, the pipe factories and the `HcdPdd_*` functions. The CE EHCI driver
(`...\usb20\ehci`) is the template for conventions.

Source in `raspi2\src\drivers\dwchci`:

| File | Content |
|---|---|
| `dwc2reg.h` | Register offsets and masks (from Linux `hw.h`). No bitfield structs. |
| `dwclog.h` | Debug-zone logging macros (`DWCERR`/`DWCWARN`/`DWCINIT`/`DWCINFO`). |
| `dwccore.cpp/.h` | `CDwcCore`: register mapping, dump, core reset, host init, port control, channel primitives. No CE USB stack dependency. |
| `cdwchcd.cpp/.h` | `CDwcHcd : CHcd`: root-hub virtuals, interrupt thread, channel table, pipe list, SOF scheduler, `CreateHCDObject`. |
| `cpipe.cpp/.h` | `CDwcPipe : CPipeAbs`: transfer queue, control/bulk/interrupt state machine, split state machine, abort, completion, the four `Create*Pipe` factories. |
| `dwctt.cpp/.h` | `CDwcTt`: one transaction translator per high-speed hub; coarse per-microframe bandwidth budget for periodic split pipes. |
| `system.cpp` | PDD: `CPhysMem` pool, HCD object, `HcdPdd_*`, the `DBGPARAM`. |
| `sources`, `makefile`, `dwchci.def` | Build files. |

### Key design decisions

- One lock (`CDwcHcd::LockHc`) protects the channel table, pipe list, all transfer queues and the
  channel registers. Client callbacks and client buffers are touched only in `CDwcPipe::Complete`,
  which runs **without** the lock.
- One transfer per pipe is active at a time, on one channel, one attempt at a time; the channel is
  released after every attempt.
- Buffer DMA mode. By default client data is copied through `CPhysMem` (uncached, contiguous,
  32-byte aligned), so there are no cache or alignment issues; IN buffers are rounded up to a whole
  packet. Bus address = physical address + `DmaBusOffset` (registry, default `0x40000000`,
  verified).
- **Direct DMA.** A **bulk** transfer whose client passes the physical address of a physically
  contiguous buffer is DMAed straight to/from that buffer - no `CPhysMem` allocation and no copy -
  when the address and the virtual address are 32-byte (cache line) aligned and, for IN, the size
  is a whole number of max packets (the channel is programmed in whole packets). The buffer may be
  cached: `IssueTransfer` writes its dirty lines back (`CACHE_SYNC_WRITEBACK`), and `Complete`
  discards the received bytes (`CACHE_SYNC_DISCARD`) before the callback. Only a *write back* is
  done before an IN, never a discard: for a 16 KB buffer (the size of the L1 data cache) the OAL
  turns a range discard into a whole-cache invalidate, which evicted everyone's working set on
  every transfer. The Ethernet driver uses this path (cached `AllocPhysMem` buffers); CE's USB
  mass storage driver also passes physical addresses (uncached buffers) and takes it when the size
  fits - verified with checksummed copies both ways. The uncached copy was the main per-byte cost of
  network receive.
- `HCINTMSK` = channel-halted + AHB error; the reason for a halt is decoded from the other `HCINT`
  bits (transfer complete, stall, babble, NAK, then errors with three retries).
- For split transactions the HCCHAR `MULTICNT` field is the error/retry count: 3 for
  interrupt/isoc, 1 for control/bulk (per dwc2). Behind a high-speed hub `HFNUM` counts
  microframes, so a full/low speed interrupt interval is `bInterval * 8`.
- Port change bits are kept in software (`m_dwPortChange`); the library clears them through
  `RootHubFeature`, and the interrupt thread acknowledges the hardware bits.

### Logging

Logging uses CE debug zones (DBGPARAM named `DWCHCI`, registered in `HcdPdd_DllMain`). Defaults:

| Zone | Default | Content |
|---|---|---|
| Errors | on | real failures |
| Warnings | on | transfer/split failures, refused/unreachable devices, channels that would not halt, unexpected interrupt bits |
| Init | off | register dump, host-init and reset progress |
| Info | off | enumeration, pipe "alive", per-disconnect diagnostics |

A normal Debug boot is quiet; turn on the Init/Info zones (kernel zone editor) for the full
register dump and per-event trace. KITL serial output costs ~30 ms per line, so never log per
transfer attempt. In a retail build `DEBUGMSG` compiles out entirely.

## Hardware facts (verified on a Pi Zero)

- `MmMapIoSpace(0x20980000, 0x1000)` works from a kernel-mode driver. USB block base `0x20980000`,
  `SYSINTR_USBHOST = 17` (OAL maps it to VideoCore IRQ 9).
- `GSNPSID=0x4F54280A` (core 2.80a: reset completes when `CSFTRST` self-clears; no `HFIR` reload).
- `GHWCFG2=0x228DDD50`: internal DMA, UTMI+ HS PHY, dedicated FS PHY, 8 channels, dynamic FIFO.
- `GHWCFG3=0x0FF000E8`: FIFO depth 4080 words. `GHWCFG4=0x1FF00020`: 8-bit UTMI (60 MHz PHY clock),
  no descriptor DMA.
- Firmware leaves: `GUSBCFG=0x20402700` (force-host set), host mode, `HCFG=1`, `HFIR=48000`,
  `HPRT0=0`, FIFO sizes RX 4096 / NP TX 256 / periodic TX 512.
- After our init: `GUSBCFG=0x20002400` (incl. `USBTRDTIM=9` for the 8-bit PHY), `GAHBCFG=0x30`
  (+ bit 0 once interrupts are on), `HCFG=0`, FIFOs RX 774 / NP 256 / periodic 512, `HPRT0=0x1000`.
- Speed detection works for FS, LS and HS. `HFIR` = 59999 (FS/LS) or 7499 (HS) at the 60 MHz clock.
- Quirks: `DISCONNINT` fires around a port reset while the device is still connected, and `CONNDET`
  can re-latch from contact bounce - check `HPRT0.CONNSTS` before acting. Halting a channel on a
  *present* device needs `CHENA|CHDIS` written together (IN channel turned to OUT, `EPDIR=0`); once
  the device is gone no channel will halt and only a core reset recovers it. Flushing the FIFOs
  does not clear a stuck `CHENA`.

## hcd2lib integration notes

- `HcdMdd_CreateHcdObject` must be called inside `HcdPdd_Init`; it calls `CreateHCDObject` then
  `DeviceInitialize`.
- The hub thread calls `ResetAndEnablePort` twice per attach, reads the speed from `GetPortStatus`
  after the reset, and clears each change bit with `RootHubFeature(CLEAR, bit | 0x10)`.
- The control pipe is created with address 0 and max packet 8; the device address comes with every
  `IssueTransfer`, and `ChangeMaxPacketSize` is called after the first descriptor.
- Because the root hub is declared high speed, the library calls `AddedTt(0, port)` even for a
  full/low speed device on the root port and passes hub address 0 to the pipe - hence the split
  rule keys off `bHubAddress != 0`, not off `ttContext` (which is NULL during addr-0 enumeration
  behind a hub).
- A control stall must **not** set the pipe's halted flag: after a failed transfer the library
  calls `IsPipeHalted` and gives up on the device if it is set.
- Client callbacks run outside the controller lock; completion copies IN data, sets
  `*lpfComplete`/`*lpdwError`/`*lpdwBytesTransferred`, then calls the callback, all in `__try`.

## Building and testing

- Use the **Debug** configuration (Release sets `IMGNOKITL=1`, so there is no debug output). The OS
  design needs `SYSGEN_USB` and `SYSGEN_USB_HID_CLIENTS`, plus `SYSGEN_FATFS` to mount USB storage.
  Each configuration is sysgen'd separately.
- Build `src\drivers\dwchci` (stop the KITL/debugger connection first, or the release-dir DLL is
  locked and the copy step fails while the link reports 0 errors), makeimg, copy `kernel.img`, boot
  and capture the KITL Output window.
- Source files (`*.c/.cpp/.h/.s/.inc/.def` and the build files) must be **CRLF**, ASCII, with a
  trailing newline; `build.exe` is the original 2009 binary and is fragile with exotic `sources`
  syntax and long lines.

## Future goals / remaining work

In rough priority order:

1. ~~USB Ethernet class driver~~ - done, see [ethernet.md](ethernet.md).
2. **Driver clean-up (remainder of M5).** Logging is now zoned; still open: drop the unused
   `src\INC\dwcRegs.h` (the original author's unverified bitfield header), and decide whether to
   register the power-management `IClass`.
3. **Isochronous pipes.** `CreateIsochPipe` currently refuses. Needed only for USB audio or UVC
   webcams; HID, storage and Ethernet do not use isoch. Would exercise the periodic scheduler
   harder than HID does.
4. **Power management.** `HCD_SUSPEND_ON_REQUEST` is not accepted and no power `IClass` is
   registered, so there is no selective suspend / resume yet.
5. **TT bandwidth scheduler refinement.** `CDwcTt` uses a coarse per-microframe byte budget that is
   adequate for low-bandwidth HID/FS behind a hub; a workload that saturates a full-speed TT
   (e.g. heavy full-speed isoc/bulk behind a hub) could need a more exact schedule. Optional NAK-
   throttle refinement for split interrupt pipes also remains.

### Loose ends (not the USB driver)

- The boot log shows `OEMInterruptEnable: ERROR enabling unknown SysIntr=11` just before GWES
  loads. It predates the USB driver's use of interrupts; source not yet found.

## Reference sources

Linux dwc2 (`drivers/usb/dwc2`, host files GPL-2.0+ OR BSD-3-Clause) is the behavioural model;
CSUD (Chadderz's Simple USB Driver, MIT) and the USPi/Circle projects also drive this controller.
Check each licence before copying code into the BSP.
