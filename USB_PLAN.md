# USB host driver for CEonPi: plan and status

Last updated: 2026-11-16. Milestones 1-4 verified on hardware (root-port HID works, incl. both
composite receivers). Split transactions for hubs: **Step 2 (control + bulk splits) verified** -
FS and LS devices enumerate behind a HS hub, and HS mass storage behind a hub works end to end
(with `SYSGEN_FATFS` added to mount FAT drives). **Step 3 (interrupt splits + periodic microframe
scheduler) verified on hardware: HID behind a hub delivers input.** A behind-hub disconnect bug is
fixed and verified: `ChannelForceOff` (the `AbortOne` fallback, the only teardown path for a device
removed behind a hub since there is no root `DISCONNINT`) now halts a busy interrupt-IN channel
with the `EPDIR=0` turnaround, instead of the ineffective clear-CHENA/set-CHDIS that left the
periodic queue jammed (interrupt transfers dead, bulk still working). Verified in a soak:
repeated HID plug/unplug on the hub, hub unplug/replug, and flash-drive file transfers all work;
`ChannelForceOff` never reported a stuck channel. Hubs with split transactions are now a done
milestone. Full design in `~/.claude/plans/lucky-bouncing-frog.md`.

Benign, not driver bugs: `HidMdd_ProcessInterruptReport: Queue is full. Dropping packet` is the CE
HID MDD dropping a redundant report when GWES drains slower than input arrives (tunable via the HID
MDD input-queue registry; harmless). A burst of `USBD: Invalid handle` during a disconnect is
usbhid issuing on just-freed pipes; the device re-enumerates cleanly afterwards.

Verified split traps: `m_fDoSplit` must be `!fIsHighSpeed && bHubAddress != 0`, not keyed off
`ttContext` (hcd2lib leaves it NULL during addr-0 enumeration behind a hub, which deadlocked
enumeration); and for split transactions HCCHAR `MULTICNT` is the retry count (3 for
interrupt/isoc, 1 for control/bulk, per dwc2).

Resolved issues (both independent, both involved low-speed mice):
- **Interrupt over-polling - FIXED, verified.** After a report completed the client re-issued and
  the pipe polled again at once, so a continuously-moving mouse was polled far faster than its
  `bInterval` and flooded the HID queue (`Queue is full. Dropping packet`, seen with a Microsoft
  low-speed mouse behind a HS hub). `IssueTransfer` now holds a fresh interrupt poll to the
  interval boundary (`m_dwLastPollFrame`). Verified on hardware (MS mouse no longer stutters/drops).
- **Low-speed device behind a full-speed hub - RESOLVED (hardware limit), verified.** A LS mouse
  plugged into a full-speed hub (e.g. a Dell keyboard-hub, 413c:1003, which makes the root bus run
  at FS) cannot be reached on this silicon: the BCM2835 core (dedicated FS PHY + UTMI+ HS PHY)
  cannot emit low-speed PRE packets while the root port runs at full speed, so the LS token babbled
  and the core disabled the root port in a loop. dwc2 refuses this exact case (`hcd.c:1860`,
  `-ENODEV`). `CDwcCore::LowSpeedUnreachable` now detects it and `CDwcPipe::IssueTransfer` fails the
  transfer cleanly (`USB_DEVICE_NOT_RESPONDING_ERROR`) instead of babbling - only that one device
  fails to enumerate, the hub and everything else stay up. Also gated `m_fDoSplit` on the root port
  being HS (a FS/LS device behind a FS hub has no TT). A LS device behind a **HS** hub (splits), or
  straight into the Pi, both still work.

Goal: a keyboard or mouse working on the CE desktop when plugged directly into the Pi Zero's
OTG port (no hub). Hubs, split transactions and USB Ethernet come later.

## Status at a glance

| Milestone | State |
|---|---|
| M0 OS design has USB host + HID | Done (Debug configuration sysgen'd, modules in image) |
| M1 Skeleton DLL, register dump | Done, verified on a Pi Zero |
| M2 Core bring-up, root port detect | Done, verified on a Pi Zero (FS, LS, HS devices) |
| M3 hcd2lib integration, control pipes, enumeration | Done, verified on a Pi Zero (mouse 045e:00cb, keyboard 17ef:60ba enumerated) |
| M4 Interrupt and bulk pipes (HID works) | Verified: single + composite HID work on the root port |
| M6 Hubs / split transactions | **Verified**: control+bulk+interrupt splits all work behind a hub (HID input, FS/LS enumerate, HS storage + file transfers), and behind-hub disconnect is handled. NAK-throttle refinement still optional |
| M5 Clean-up | Not started |

## Next step: build + test split Step 3 (interrupt splits behind a hub)

Build `src\drivers\dwchci` once (Debug; **stop KITL first** so the release-dir DLL is not
locked), makeimg, copy `kernel.img`, boot, and save the Output window to
`C:\Dev\ceonpi\bootlog.txt`. `dwctt.cpp` is a new source in `sources`.

### Test: a HID device behind a hub
1. Plug a **USB 2.0 hub** into the OTG port, then a **keyboard and/or mouse** into the hub (a FS
   keyboard such as 17ef:60ba and a LS mouse such as 045e:00cb exercise both speeds).
2. Expect, per HID interrupt endpoint, one `DWCHCI: split interrupt pipe ep 0x8x alive: first
   packet N bytes`, and then **typing / pointer movement works in the CE UI** - no more
   `interrupt split pipe ... not scheduled yet`.
3. Regression: a keyboard/mouse on the **root port** and an HS device behind the hub must still
   work. Watch for `split failed ... HCINT=...` (handshake/scheduler bug), repeated
   `transfer failed ... ep 0x8x`, or a wedge where an idle endpoint keeps a channel busy.
4. Robustness (preview of Step 4): unplug/replug on the hub; confirm clean teardown and that TT
   bandwidth is released (a replugged device still opens its interrupt pipes).

Expected log (root-port HID, unchanged):
- At load: the register dump as before, now ending `DeviceInitialize done (milestone 4: control,
  bulk and interrupt pipes)`, `HcdPdd_Init done`.
- On plug-in: the same enumeration lines as M3 (`device descriptor ... VID xxxx PID xxxx`). The
  M3 "interrupt pipe ... not implemented yet" line is gone.
- The first time a report arrives on each HID endpoint: one line
  `DWCHCI: interrupt pipe ep 0x81 alive: first packet N bytes` (logged once per pipe, not per
  report). After that, typing and mouse movement should work in the CE UI with no further log
  spam.
- On unplug: `device disconnected`.

Expected behaviour: keystrokes reach CE windows; the mouse moves the pointer; unplug/replug
re-enumerates and HID works again; the system stays responsive while a HID device is idle.

If it fails:
- No `... alive: first packet` line and HID does nothing: the interrupt poll is not completing.
  Look for `transfer failed, addr N ep 0x81 ...` (transaction errors) in the log.
- The boot hangs or crawls while a HID device is attached: the SOF scheduler is mis-firing.
  Suspect the frame arithmetic (`CurFrame`/`FrameReached` in `dwccore.h`) or a leaked SOF
  waiter (the `m_dwSofWaiters` count in `cdwchcd.cpp` never returning to 0).
- After removing a composite device the next one still will not enumerate even though the log
  shows `resetting the controller` + `host init done`: the core reset recovery itself failed.
  Check for `controller re-init after disconnect failed`, and whether `host init done` reports
  sane FIFO/HPRT values. If the reset runs but re-enumeration still fails, the wedge is deeper
  than the channels (suspect the port/PHY state) and may need a longer power-off of HPRT0.
- `AHB (DMA) error`: wrong DMA bus address (but M3 already validated `DmaBusOffset=0x40000000`).
- Link errors (unresolved externals from `hcd2lib`): read `Z:\platform\raspi2\Build.log`.
- The M4 code already builds and runs; only the disconnect fix in `dwccore.cpp` is new.

## How we work

- Claude edits `C:\Dev\ceonpi` and cannot build. The VM's `C:\WINCE700` is visible read-only
  as `Z:\` (`Z:\platform\raspi2` is the VM's copy of the BSP, `Z:\projects\RaspiCE\RaspiCE`
  the OS design, `Z:\platform\raspi2\Build.log` the last BSP build log).
- The user builds, boots and saves the KITL output to `C:\Dev\ceonpi\bootlog.txt`.
- Use the **Debug** configuration. Release has `IMGNOKITL=1`, so no debug output.
- KITL serial output costs about 30 ms per line. Log events and errors, never per attempt.
- New source files need CRLF: write them, run `unix2dos`, check `git ls-files --eol`.
  `.hpp` is not covered by `.gitattributes`, so use `.h`.
- Reference sources on the host: `C:\Dev\csud` (MIT) and
  `C:\Dev\linux-dwc2\drivers\usb\dwc2` (host files GPL-2.0+ OR BSD-3-Clause). Linux is the
  behavioural model; CSUD is wrong for a device directly on the root port.

## Design

`dwchci.dll` links the CE USB 2.0 host library `hcd2lib`
(`Z:\public\common\oak\drivers\usb\hcd\usb20\usb2com`). The library provides the `HCD_*`
stream entry points, the USBD interface, the root hub and hub logic, enumeration and
`CPhysMem`. We provide a `CHcd` subclass, the pipe factories and the `HcdPdd_*` functions.
The CE EHCI driver (`...\usb20\ehci`, `...\usb20\ehcipdd`) is the template for conventions.

Files in `raspi2\src\drivers\dwchci`:

| File | Content |
|---|---|
| `dwc2reg.h` | Register offsets and masks (from Linux `hw.h`). No bitfield structs. |
| `dwccore.cpp/.h` | `CDwcCore`: mapping, register dump, core reset, host init, port control, channel primitives. No CE USB stack dependency. |
| `cdwchcd.cpp/.h` | `CDwcHcd : CHcd`: root hub virtuals, interrupt thread, channel table, pipe list, `CreateHCDObject`. |
| `cpipe.cpp/.h` | `CDwcPipe : CPipeAbs`: transfer queue, control transfer state machine, abort, completion, the four `Create*Pipe` factories. |
| `system.cpp` | PDD: creates the `CPhysMem` pool (320 KB, allocated by the library) and the HCD object. |
| `sources`, `makefile`, `dwchci.def` | Build files. |

Also touched: `src\drivers\dirs`, `files\platform.bib` (`dwchci.dll`, `NK SHK`),
`files\platform.reg` (`Drivers\BuiltIn\DWCUsbHost`, values `Prefix`, `Dll`, `Order`,
`DmaBusOffset`).

Key design decisions:
- One lock (`CDwcHcd::LockHc`) protects the channel table, pipe list, all transfer queues and
  channel registers. Client callbacks and client buffers are touched only in
  `CDwcPipe::Complete`, which runs without the lock.
- One transfer per pipe is active at a time, on one channel, one attempt at a time. The
  channel is released after every attempt.
- Buffer DMA mode. `HCINTMSK` = channel halted + AHB error; the reason for a halt is decoded
  from the other `HCINT` bits (order: transfer complete, stall, babble, NAK, then errors with
  three retries).
- All client data is copied through `CPhysMem` (uncached, contiguous, 32-byte aligned), so
  there are no cache or alignment issues. A client-supplied physical address is ignored.
  IN buffers are rounded up to a whole packet.
- Bus address = physical address + `DmaBusOffset` (registry, default `0x40000000`).
- Port change bits are kept in software (`m_dwPortChange`); the library clears them through
  `RootHubFeature`. The interrupt thread acknowledges the hardware bits.
- `HCD_SUSPEND_ON_REQUEST` is not accepted and no `IClass` is registered, so there is no
  power management yet.

## Facts verified on hardware (Pi Zero)

- `MmMapIoSpace(0x20980000, 0x1000)` works from a kernel-mode driver.
- `GSNPSID=0x4F54280A` (core 2.80a: reset completes when `CSFTRST` self-clears; no `HFIR`
  reload control).
- `GHWCFG2=0x228DDD50`: internal DMA, UTMI+ HS PHY, dedicated FS PHY, 8 channels, dynamic FIFO.
- `GHWCFG3=0x0FF000E8`: FIFO depth 4080 words. `GHWCFG4=0x1FF00020`: 8-bit UTMI (60 MHz PHY
  clock), no descriptor DMA.
- Firmware leaves: `GUSBCFG=0x20402700` (force-host set), host mode, `HCFG=1`, `HFIR=48000`,
  `HPRT0=0`, FIFO sizes RX 4096 / NP TX 256 / periodic TX 512.
- After our init: `GUSBCFG=0x20001400`, `GAHBCFG=0x30` (+ bit 0 when interrupts are on),
  `HCFG=0`, FIFOs RX 774 / NP 256 at 774 / periodic 512 at 1030, `HPRT0=0x1000`.
- Interrupt path works: `SYSINTR_USBHOST` = 17, handled by the existing OAL code.
- Speed detection works for FS, LS and HS. `HFIR` = 59999 (FS/LS) or 7499 (HS).
- Quirks: `DISCONNINT` fires around a port reset while the device is still connected, and
  `CONNDET` can re-latch from contact bounce. Check `HPRT0.CONNSTS` before acting.
- Halting a channel on a *present* device needs `CHENA|CHDIS` written together (and an IN
  channel turned to OUT, `EPDIR=0`), as `HaltAllChannels` does at init.
- BUT once the device is physically gone, the core cannot drain the periodic request queue, so
  a channel that was polling an interrupt-IN endpoint **will not halt at all**: `CHENA` never
  clears and `CHHLTD` never fires. Verified on hardware with two composite wireless receivers
  (Logitech Bolt 046d:c548 and Microsoft 045e:0800): the disconnect log showed
  `disconnect halted 2 active channel(s), 2 stuck`. Two stuck channels wedge the whole host -
  the port will not re-enable (`port did not enable after reset`) and no device re-enumerates
  (hub logs `forcing descr type 0x0 to 0x1`, i.e. the control-IN returned zeros).
- Flushing the FIFOs does not clear a stuck `CHENA`. The only reliable recovery is a **core
  reset**. `HandleDisconnect` now re-runs `HostInit` + `EnableInterrupts` whenever
  `DisableAllChannels` reports any stuck channel; this mirrors cold boot and leaves the port
  powered, so a still-attached device is re-detected. Expect a
  `... would not halt; resetting the controller` line followed by `host init done` on such a
  disconnect, and then normal re-enumeration.

## Facts about hcd2lib that shaped the code

- `HcdMdd_CreateHcdObject` must be called inside `HcdPdd_Init`. It calls `CreateHCDObject`
  then `DeviceInitialize`, and allocates the pipe cache from `m_dwOpenedPipeCacheSize`.
- The hub thread calls `ResetAndEnablePort` twice per attach (once itself, once in
  `AttachDevice`), reads the speed from `GetPortStatus` after the reset, and clears each
  reported change bit with `RootHubFeature(CLEAR, bit | 0x10)`.
- The control pipe is created with address 0 and max packet 8. The device address is passed
  with every `IssueTransfer`. `ChangeMaxPacketSize` is called after the first descriptor.
- Because the root hub is declared high speed, the library calls `AddedTt(0, port)` for a
  full/low speed device on the root port and passes hub address 0 to the pipe. Rule used:
  split iff `!fIsHighSpeed && bHubAddress != 0` (full/low speed behind a real external hub).
  `ttContext` is deliberately not used for this decision - the library leaves it NULL during
  the addr-0 enumeration of a device behind a hub, so keying off it deadlocked enumeration.
- Completion convention (from EHCI `CQTransfer::DoneTransfer`): copy IN data, set
  `*lpfComplete`, `*lpdwError`, `*lpdwBytesTransferred`, then call the callback, all in
  `__try`. Abort reports `USB_CANCELED_ERROR` and then calls the cancel callback.
- A control stall must not set the pipe's halted flag: after a failed transfer the library
  calls `IsPipeHalted` and gives up on the device if it is set.

## Unverified assumptions

1. DMA bus address offset `0x40000000`: **verified by M3** (descriptors read correctly, no AHB
   errors).
2. In buffer DMA mode the core retries NAKs on control and bulk endpoints by itself without
   halting the channel (taken from Linux behaviour). If control transfers to a slow device
   hang or spin, look here.
3. `HCTSIZ` after a halt gives bytes left (IN) and packets left (OUT), and its PID field is
   the next data toggle (as Linux uses them).

## M4 plan: interrupt and bulk pipes (implemented, awaiting test)

Implemented as described below. Summary of what landed in the code:
- `CreateBulkPipe` / `CreateInterruptPipe` now construct a `CDwcPipe` (the constructor already
  handles any endpoint type via `m_bEpType`).
- Bulk reuses the control data-stage machinery. A per-pipe running toggle `m_dwNextPid` is
  seeded into each transfer once (`DWC_TRANSFER.fDataPidInit`) and advanced from `HCTSIZ` on
  completion; NAK retries immediately; STALL sets the halted flag and resets the toggle.
- Interrupt pipes add a start-of-frame scheduler: on NAK or frame overrun the head transfer is
  held and re-polled one interval later (`ScheduleNextInterval`); the controller enables the SOF
  interrupt only while `m_dwSofWaiters > 0` (`CDwcHcd::SofWaiterAdd/Remove`,
  `CDwcCore::EnableSof/DisableSof`). The SOF handler (`CDwcHcd::HandleSof` ->
  `CDwcPipe::OnSofFrame`) starts pipes whose due frame has arrived, using the wrap-safe
  `CDwcCore::FrameReached`. One packet per attempt; `fOddFrame` set from `HFNUM + 1`.
- Frame-wait bookkeeping is released on disconnect, abort and close (`ClearFrameWait`) so the
  SOF waiter count cannot leak.

Original design notes (for reference):

Add to `CDwcPipe` (the machinery is shared; only the data stage is used):
- `CreateBulkPipe` and `CreateInterruptPipe` return a `CDwcPipe`. Initial PID is `m_dwNextPid`
  (DATA0 after open and after `ClearHaltedFlag`); set `pTransfer->dwPid = m_dwNextPid` when a
  non-control transfer starts, and save the toggle from `HCTSIZ` on completion (already done).
- Interrupt endpoints: one packet per attempt. Set `fOddFrame` in `DWC_CHANNEL_SETUP` from the
  parity of the frame the transfer will go out in (`HFNUM` + 1), as Linux
  `dwc2_hc_set_even_odd_frame` does.
- Interrupt NAK: the channel halts with NAK. Keep the transfer at the head of the queue and
  retry `bInterval` frames later (frames for FS/LS, 2^(bInterval-1) microframes for HS). Do
  not restart immediately as control pipes do.
- Scheduling: unmask `GINTSTS_SOF` only while some interrupt pipe is waiting for its slot; in
  the SOF handler start the pipes whose due frame has arrived; mask SOF again when none wait.
  Frame arithmetic wraps at `0x3FFF`. This costs up to 1000 interrupts per second while a
  keyboard is idle; a timer-based wake-up is the later optimisation.
- Frame overrun on an interrupt endpoint: retry at the next interval without counting it as
  an error.
- Stall on bulk or interrupt: set the halted flag and reset the toggle (already in place).
- Keep `HCINTMSK` as is.

Expected result: typing in a CE window and moving the mouse work; unplug and replug works;
the boot stays responsive.

## M5 and later

- M5: move logging behind debug zones, remove the register dump from every boot, update
  `CLAUDE.md`, consider registering the power-management `IClass`.
- Later: hubs with split transactions (`AddedTt`, `HCSPLT`, start-split / complete-split
  sequencing as in Linux), isochronous pipes, USB Ethernet.

## Open side issues

- The boot log shows `OEMInterruptEnable: ERROR enabling unknown SysIntr=11` just before GWES
  loads. It predates the USB driver's use of interrupts; source not found.
- `src\INC\dwcRegs.h` (the original author's bitfield header) is unused and unverified.
