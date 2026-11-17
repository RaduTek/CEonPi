# SD host controller driver (`sdhcbcm`)

A platform driver for the BCM2835 **Arasan eMMC (SDHCI)** controller, letting Windows Embedded
Compact 7 mount the SD card as a FAT/exFAT volume on the Raspberry Pi 1 family (notably the Pi
Zero). The driver is `sdhcbcm.dll`, a thin **BCM2835 PDD over the CE standard SD host MDD**
(`SDHCBase.lib` - `CSDHCBase`/`CSDHCSlotBase`, from the public `SDHCDrivers\SDIOStdHC\base` tree).
The MDD does the SD protocol, clock, interrupt and data-transfer work; the SD bus driver (`sdbus`)
and memory client (`sdmemory`) load above it, so the card shows up as a normal **Storage Card**
volume.

> Build/boot mechanics for the whole BSP live in `CLAUDE.md`, which also has the detailed
> engineering notes. This file describes the driver as it stands and the work still ahead.

## Current state

Verified on a Pi Zero with both an SDSC (1 GB) and an SDHC (4 GB) card: the card enumerates, the
FAT/exFAT partition mounts, and files read and write through Explorer. **All transfers are PIO**
(programmed I/O) - see [DMA](#dma-not-used) for why DMA is not used.

| Area | State |
|---|---|
| Register window (`0x20300000`), SYSINTR wiring (GPU IRQ 62 -> `SYSINTR_SDHC` = 18) | Done |
| Synthesized capabilities (the Arasan `CAPABILITIES` register reads 0) | Done |
| Base clock from the VideoCore mailbox (200 MHz) | Done |
| Data-timeout clock (SDCLK-based, 25 MHz) | Done |
| Card identification - SDSC and SDHC (high-capacity) | Done |
| FAT/exFAT mount, file read and write | Done (PIO) |
| DMA (built-in SDHCI SDMA / ADMA2) | **Not possible on this silicon** |
| Card-detect / hot-plug insert-remove | Not implemented / untested |
| High-speed mode, 4-bit bus width | Not specifically verified |

## Hardware

- **Controller:** Arasan eMMC / SDHCI at `0x20300000`, SDHCI spec version 3.00
  (`HOST_VERSION = 0x9902`). GPU IRQ 62, wired in the OAL as `SYSINTR_SDHC` (= 18).
- **Pin routing:** the SD card is on GPIO 48-53 in **ALT3**, which the firmware selects by default,
  so no pin re-route or `config.txt` change is needed. (The other SD controller, the Broadcom
  SDHOST at `0x20202000`, is *not* used - it has no datasheet and no CE MDD.)
- **`CAPABILITIES` register reads 0** on the BCM2835 Arasan (the SDHCI "missing caps" quirk), so the
  PDD must synthesize it.

## Architecture

The design reuses the Microsoft-supplied SDHCI MDD and keeps the board-specific code small. The PDD
(`src/drivers/sdhcbcm`) provides only:

- **`CSDHCSlotBcm2835::GetCapabilities()`** - the Arasan `CAPABILITIES` register reads 0, so this
  returns a synthesized value advertising SDMA, high speed, 3.3 V and a 512-byte maximum block
  length. The base clock and timeout clock are taken from the registry instead of the (zero) caps
  fields.
- **The controller/slot factory** - `CreateSDHCBcmObject` (named by the registry
  `ObjectCreationProc`) builds a `CSDHCBcm2835`, which hands out one `CSDHCSlotBcm2835` slot.

Everything else - command/response, clocking, the interrupt service thread, and PIO data movement -
is the stock MDD.

### Registry (`files/platform.reg`, `HKLM\Drivers\BuiltIn\SDHCBcm`)

| Value | Setting | Notes |
|---|---|---|
| `MemBase` / `MemLen` | `0x20300000` / `0x100` | Arasan register window (Internal bus) |
| `SysIntr` | `0x12` (18) | Static `SYSINTR_SDHC`, wired in the OAL for GPU IRQ 62 |
| `BaseClockFrequency` | `200000000` | VideoCore EMMC clock (see below) |
| `TimeoutClockFrequency` | `25000000` | SDCLK, drives the data-timeout counter (see below) |
| `DisableDMA` | `1` | PIO only; DMA does not work on this controller |

### OAL support added for the SD host

- **`IOCTL_HAL_MY_GET_EMMC_CLOCK`** (`src/oal/oallib/ioctl.c`) - returns the VideoCore EMMC base
  clock (200 MHz) read via the mailbox "get clock rate" property tag. The Arasan `CAPABILITIES`
  register carries no base-clock field, so the PDD feeds this to the MDD as `BaseClockFrequency`.
- **`IOCTL_HAL_GET_CACHE_INFO`** - the stock handler (`OALIoCtlHalGetCacheInfo`) was commented out
  of this BSP's OAL IOCTL table; it is now enabled. Without it `CeGetCacheInfo()` returns FALSE,
  which the MDD's DMA path asserts on. It is not exercised by the PIO path but is correct to have
  and is required by any future DMA path.

## Two hardware traps worth remembering

### Data-timeout clock must be SDCLK, not the base clock

The Arasan's data-timeout counter is clocked by **SDCLK**, not a separate timeout clock, and the
timeout-value register field is only 4 bits. If `TimeoutClockFrequency` is set to the 200 MHz base
clock, the MDD computes a timeout-control value of `0x10` to reach its ~2.7 s target, which
**overflows the 4-bit field to 0** - the minimum (~41 us) window. A fast card still returns data
inside 41 us, but a slower card fails **every** data transfer with a data-timeout error, so the MBR
cannot be read and the volume will not mount. Setting `TimeoutClockFrequency` to the 25 MHz SD data
clock keeps the computed value in range (`0xD`) with a real window near 2.7 s.

This is the same quirk FreeBSD/Linux express with `SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK` +
`SDHCI_QUIRK_BROKEN_TIMEOUT_VAL`.

### Initial clock is slightly out of spec

SD card identification wants a clock of 400 kHz or less, but the SDHCI clock divider here is 8-bit,
so the slowest it reaches from the 200 MHz base is `200 MHz / 256 = 781250 Hz`. Both test cards
tolerate it. A fully correct fix would use the SDHCI 3.00 10-bit divider; this is deferred.

## DMA (not used)

**The BCM2835 Arasan's built-in SDHCI DMA does not work**, so the driver runs in PIO. This was
established the hard way:

- A PDD DMA path was written (translate the MDD's CE physical addresses to VideoCore bus addresses
  before the SDMA System Address register write). The translation was confirmed correct on
  hardware (`bus = physical + offset`), but the **first multi-block transfer arms and then hangs** -
  no data moves and no SDMA-boundary or transfer-complete interrupt ever fires. Both the
  `0x40000000` (L2-coherent) and `0xC0000000` (uncached) bus aliases behaved identically, ruling out
  addressing and cache coherency.
- Root cause: the Arasan's EMMC FIFO needs **DREQ flow control**, which the controller's built-in
  SDMA (and ADMA2) bus master does not provide. FreeBSD (`bcm2835_sdhci.c`) and the Linux drivers
  therefore **never use SDHCI SDMA/ADMA2** - they move SD data through the **external BCM2835 DMA
  engine**, DREQ-paced (`BCM_DMA_DREQ_EMMC`), with PIO for sub-block transfers.

The CE standard SD host MDD only knows the controller's built-in SDMA/ADMA2, so it cannot drive DMA
on this controller at all. `DisableDMA=1` keeps the stack on the working PIO path.

### Future: external DMA engine

Real DMA would mean driving the **external BCM2835 DMA controller** (the DREQ-paced engine behind
`src/INC/DMA.h`) and feeding the SD data path directly, bypassing the MDD's transfer machinery -
the CE equivalent of FreeBSD's `SDHCI_PLATFORM_TRANSFER` hooks, which the CE MDD does not expose.
That is a milestone in its own right. The `IOCTL_HAL_GET_CACHE_INFO` groundwork is already in place.

## Other future work

- Card-detect and hot-plug (insert/remove) handling.
- Verify and, if needed, enable high-speed mode and 4-bit bus width.
- The 10-bit clock divider for an in-spec identification clock.
- The boot log shows `InitializeHardware: IRQ 0xFFFFFFFF mapped to SYS_INTR 0x12`; the `0xFFFFFFFF`
  is cosmetic (the static `SysIntr` is used and interrupts work), but could be tidied.

## Reference code

FreeBSD `bcm2835_sdhci.c` / `bcm2835_sdhost.c` and NetBSD `sdhc.c` (all BSD-licensed) are the
permissive references; the Linux `bcm2835.c` / `sdhci-bcm2835.c` and U-Boot are GPL and were used
for documentation only. Check each licence before copying code into this BSP.
