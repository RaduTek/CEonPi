# USB Ethernet driver (`smsc95xx`)

A driver for the **SMSC LAN9512** USB hub + 10/100 Ethernet chip on the **Raspberry Pi 1 Model B**
(the LAN9514 on the Model B+ is the same Ethernet function and should work too, untested). The chip
is a high-speed USB hub with the Ethernet controller hard-wired behind it as USB device
**0424:ec00**. The driver is `smsc95xx.dll`: a USB client driver loaded by USBD for that device, and
an **NDIS 5.1 deserialized miniport** in the same DLL, so the CE TCP/IP stack sees a normal wired
network adapter (DHCP, IPv4/IPv6, the network tray icon, FTP/SMB servers and clients).

> Build/boot mechanics for the whole BSP live in `CLAUDE.md`. The USB host controller underneath is
> described in [usb.md](usb.md).

## Current state

Verified on a Pi 1 Model B.

| Area | State |
|---|---|
| USB attach (VID/PID match), endpoint discovery, register/MII access | Done |
| Chip init: lite reset, PHY reset, autonegotiation (10/100, half/full) | Done |
| MAC address from the VideoCore firmware (same as Linux, `b8:27:eb:...`) | Done |
| NDIS registration without a boot-time `Linkage\Route` | Done |
| DHCP, IPv4 + IPv6, ping both ways, tray icon | Done |
| Link up/down (cable unplug/replug) | Done (polled, 1 s) |
| Receive filter: unicast, broadcast, multicast hash, all-multicast, promiscuous | Done |
| Multi-frame RX (several frames per bulk-IN transfer) | Done |
| Multi-frame TX (up to 8 frames per bulk-OUT transfer) | Done |
| Zero-copy DMA (no copy through the HCD's uncached buffers) | Done |
| Checksum offload, VLAN, Wake-on-LAN, power management | Not implemented |
| Hot-unplug | Code path present; the chip is on-board, so untested |

### Throughput

FTP to/from the RAM disk (`\Temp`), files checked by checksum in both directions:

| Build | Pi receives | Pi sends |
|---|---|---|
| Checked (Debug) | ~3.2 MB/s | ~2.5 MB/s |
| Release | **~5.6 MB/s** | **~3.8 MB/s** |

The ARM1176 at 700 MHz is the limit, not the wire or USB: during a transfer the CPU is at
90-100%. Release is about twice as fast as Checked because the debug code paths (notably the
`hcd2lib` `CPhysMem` allocator filling every allocated and freed block with a pattern) disappear.

## Hardware facts

- **Registers** are accessed with vendor control requests: write `bRequest=0xA0` (type 0x40), read
  `0xA1` (type 0xC0), `wIndex` = register, 4 bytes little-endian. MII (PHY) registers go through
  `MII_ADDR`/`MII_DATA`; the internal PHY is at MII address 1.
- **Endpoints:** bulk IN, bulk OUT, interrupt IN (discovered from the descriptor, not hard-coded).
  The interrupt endpoint is not used (see link polling below).
- **RX framing:** with `HW_CFG.MEF` + `BCE` the chip packs several frames into one bulk-IN transfer,
  up to `BURST_CAP` 512-byte packets (32, so 16 KB buffers). Each frame has a 4-byte status word
  (length in bits 29:16, includes the 4-byte FCS; error-summary bit) and is padded to 4 bytes.
  `BULK_IN_DLY` = 0x2000 (the Linux/FreeBSD default).
- **TX framing:** every frame carries an 8-byte header, `TX_CTRL_0` = length | FIRST_SEG | LAST_SEG,
  `TX_CTRL_1` = length. Several frames can be concatenated in one bulk-OUT transfer, each header
  on a 4-byte boundary; the chip skips the pad bytes. (Verified on hardware; the LAN9512 databook
  was not available, the layout follows the sibling SMSC MACs' TX buffer format.)
- **MAC address:** the board has no EEPROM; the firmware derives the MAC from the SoC serial. The OAL
  answers `IOCTL_HAL_MY_GET_MAC_ADDRESS` with the VideoCore property tag `0x00010003`.
- **Multicast hash:** top 6 bits of the big-endian CRC-32 of the address; bit 5 selects
  `HASHH`/`HASHL`.

## Architecture

Source in `raspi2\src\drivers\smsc95xx`:

| File | Content |
|---|---|
| `smscreg.h` | Register and bit definitions (adapted from FreeBSD `if_smscreg.h`, BSD-2-Clause, attributed). |
| `smsc.h` | Adapter context, tuning constants, debug zones, shared prototypes. |
| `smscusb.c` | `DllEntry`, USB entry points (`USBDeviceAttach`, `USBInstallDriver`, `USBUnInstallDriver`), close notification, synchronous register and MII access. |
| `smschip.c` | Chip init, MAC, receive filter, link polling, RX thread and frame parsing, statistics, start/stop. |
| `smscndis.c` | NDIS miniport: `DriverEntry`, Initialize/Halt/Reset, Query/SetInformation, SendPackets, TX thread, RX indication, adapter registration. |
| `sources`, `makefile`, `smsc95xx.def` | Build files. |

### How a USB device becomes an NDIS adapter

There is no USB host NIC sample in WEC7; the pattern is the one the Atheros SDIO and RNDIS drivers
use for a bus client that turns into a network adapter:

1. USBD matches `Drivers\USB\LoadClients\1060_60416\Default\Default\SMSC95XX` and calls
   `USBDeviceAttach`. The driver brings the chip up and starts its threads.
2. A worker thread calls `NdisRegisterAdapter(L"SMSC95XX", L"SMSC95XX1")`, retrying while NDIS is
   not up yet.
3. NDIS reads `HKLM\Comm\SMSC95XX` (`ImagePath` = `smsc95xx.dll`), loads the DLL - already loaded by
   USBD in the same `device.exe` process, so its globals are shared - calls `DriverEntry`, then
   `MiniportInitialize`, which picks up the attached adapter.
4. On `USB_CLOSE_DEVICE` the driver calls `NdisDeregisterAdapter` (which halts the miniport) before
   stopping the USB side.

There is deliberately **no `Comm\SMSC95XX\Linkage\Route`**: with it NDIS would try to bring the
adapter up at boot, before the USB device exists. TCP/IP binds without it.

### Data path

- **RX:** four 16 KB bulk-IN transfers stay queued. The RX thread parses each completed buffer and
  indicates up to 32 frames at a time with `NdisMIndicateReceivePacket`, in place, with status
  `NDIS_STATUS_RESOURCES` (NDIS copies during the call, so there is no packet return path).
- **TX:** `MiniportSendPackets` only queues. The TX thread copies queued packets into one of eight
  bulk-OUT slots - up to eight frames per slot when several are waiting - completes them to NDIS
  right away (the data is copied), and issues the transfer. A lone packet (a ping) still goes out
  immediately on its own.
- **Zero copy below the driver:** RX buffers and TX slots are physically contiguous cached memory
  (`AllocPhysMem`), and each bulk transfer passes the physical address. `dwchci` then DMAs straight
  to/from them instead of copying through its uncached pool, doing the cache maintenance itself
  (write back before the transfer, discard the received bytes after an IN). See
  [usb.md](usb.md#key-design-decisions).
- **Link:** polled through the PHY `BMSR` once a second by the link thread, which also applies
  receive-filter changes (register writes are synchronous control transfers and must not run in
  NDIS's context) and prints statistics. The interrupt endpoint is not polled: with `bInterval` 1 ms
  it would cost `dwchci` a channel halt and reschedule every millisecond for nothing.
- **Threads:** RX and TX run at CE priority 130 (above applications, below the USB IST at 101); the
  link thread at normal priority.

### Registry (`files/platform.reg`, under `IF BSP_NOUSB !`)

| Key | Values |
|---|---|
| `Drivers\USB\LoadClients\1060_60416\Default\Default\SMSC95XX` | `DLL` = `smsc95xx.dll` |
| `Drivers\USB\ClientDrivers\SMSC95XX` | `DLL` = `smsc95xx.dll` |
| `Comm\SMSC95XX`, `Comm\SMSC95XX1` | `DisplayName`, `Group` = `NDIS`, `ImagePath` = `smsc95xx.dll` |
| `Comm\SMSC95XX1\Parms` | `BusNumber` = 0, `BusType` = 0; optional `NetworkAddress` (12 hex digits) overrides the firmware MAC |
| `Comm\SMSC95XX1\Parms\TcpIp` | `EnableDHCP` = 1 (or a static address) |

The OS design needs `SYSGEN_USB`, NDIS/TCP-IP, the DHCP client and Winsock; `SYSGEN_ETHERNET` gives
the network tray icon.

### Logging and statistics

Debug zones: Error, Warning and Init print by default (Init is a handful of one-off lines: attach,
MAC, NDIS registration, link up/down). **Info** (off by default; turn it on with the kernel debug-zone
editor) adds the register dump, endpoint list, filter changes and a statistics line every 5 s while
traffic flows:

    SMSC95XX: rx 3172 kB/s 10952 frm 1213 xfer (9.0 frm/xfer) | tx 16 kB/s 1505 frm 407 xfer (3.6 frm/xfer) | cpu ~100% busy

In Debug builds the Info zone also starts a priority-255 spin thread that estimates CPU load
(`OEMIdle` is a stub, so the kernel's idle time cannot be used). Nothing is logged per frame:
KITL serial output costs ~30 ms a line.

## Future work

- **Fewer RX transfers while sending.** During a Pi-to-PC transfer the PC's ACKs arrive as about
  3000 bulk-IN transfers/s of ~1.3 frames each; each costs an interrupt, a callback and a thread
  wake-up. A larger `BULK_IN_DLY` would batch them at the cost of some receive latency - to be
  measured.
- **Checksum offload** (`COE_CR`): small gain, the checksum is a minor part of the per-byte cost.
- **Idle power:** `OEMIdle` is a stub (no `WFI`), so the CPU spins when idle. OAL work, separate.
- Power management / Wake-on-LAN, VLAN.

## Reference sources

FreeBSD `sys/dev/usb/net/if_smsc.c` + `if_smscreg.h` (BSD-2-Clause) is the reference and the source
of the register definitions. Linux `smsc95xx.c`, U-Boot and USPi/Circle are GPL and were used as
documentation only. The Microchip WinCE LAN95xx driver was **not** used: its licence makes the
source confidential.
