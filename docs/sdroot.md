# SD card as root file system (`BSP_SDROOT`)

Status (2026-11-22): **working on a Pi 1 Model B** (first test): the SD drivers load in boot phase
1 (once each), the card is the root, the `\Windows` overlay works (EXEs copied into the card's
`Windows` folder run) and files persist across reboots and Suspend. Two follow-up fixes (drive
icons, startup items) are implemented but not yet retested.

## What it does
With the catalog item **SD card as root file system** (sets `BSP_SDROOT=1`), the FAT partition on
the SD card, the same partition that holds `bootcode.bin` and `kernel.img`, is mounted as the root
of the CE file system (`\`) instead of `\Storage Card`.

- `\Windows` becomes a merged view: the card's `Windows` folder first, then the ROM image
  (`private\winceos\COREOS\storage\fsdmgr\pathapi.cpp`, `virtroot.cpp`). A file in the card's
  `Windows` folder is found by absolute paths (`\Windows\x.dll`), by the loader's normal search and
  by Explorer. It also **overrides** a ROM file with the same name.
- Everything written under `\` (desktop, `\My Documents`, `\Temp`, ...) lands on the card and
  persists across reboots.
- The RAM object store stays mounted as `\Object Store` (it keeps the registry hives) but is no
  longer the root. With the ROM-only file system there is no RAM object store and the hives
  persist on the card (below).

A second catalog item, **Load optional modules from the SD card** (`BSP_SDOFFLOAD`, below), takes
selected modules out of `kernel.img` and puts them in the card's `Windows` folder.

## Modules loaded from the card (`BSP_SDOFFLOAD`)
Status (2026-11-22): **working on a Pi 1 Model B** with the IE7 catalog item selected: the
listed modules are left out of `kernel.img` (no `nk.nb1`), copied to the card's `Windows` folder
and load from there.

`kernel.img` is resident RAM, so every module in it costs RAM permanently. A module in the card's
`Windows` folder is demand-paged from the card only when used. Needs `BSP_SDROOT=1` (otherwise
nothing finds the files; `sdoffload.js` refuses the combination).

- **The list** is the `; SDOFFLOAD <name>` comment lines at the end of the MODULES section in
  `raspi2\files\platform.bib`. Editing it changes a `.bib` input, so makeimg rebuilds `ce.bib`.
  First list (about 4.6 MB of the 24.6 MB Checked image): the IE7/internet stack (`ieframe`,
  `wininet`, `urlmon`, `mlang`, `wininetui`; dependencies of selected catalog items) and the
  network command-line tools (`connmc`, `rnaapp`, `ping`, `ipconfig`, `route`, `netstat`, `net`,
  `tracert`, `ndisconfig`, `ipv6`). Then the IE7 browser (`mshtml` 8.6 MB, `mshtmled`, `msls31`,
  `imgutil`, `pngfilt`, `iesample`, `inetcpl`, about 11 MB): adding the Internet Explorer 7
  catalog item grew the image contents to 32.3 MB even after the first list, past `NKLEN`
  (32 MB for Checked/Release), so romimage split it into `nk.nb0` + `nk.nb1` (unbootable). A PE
  import scan of all 164 remaining image modules found no importer of the browser modules outside
  the browser group itself. Entries for components not in the image are skipped with a warning.
- **Stale `nk.nb1`:** `PostMakeimg.bat` reports "too big" when `nk.nb1` exists. `PreRomImage.proj`
  (makeimg runs it right before romimage) deletes the old `nk.nb0`/`nk.nb1`, so a leftover from an
  oversized build cannot fail a later build that fits.
- **Rules for adding a module:** it must not be needed before the card mounts (early in boot phase
  2) and no boot-phase-1 driver may import it. Check a boot log: if the module loads during boot,
  keep it in the image. Absolute `\Windows\` references are fine with the card as root.
- **Build steps:** makeimg runs `PostFmergeBib.bat` -> `sdoffload.js` right after fmerge writes
  `ce.bib`, before romimage (`makeimg.proj`, `CreateConfigBibFile`). It drops the listed entries
  from the MODULES section of `ce.bib` and writes `sdoffload.txt` (`ON`/`OFF`, then the removed
  modules). `PostMakeimg.bat` -> `mksdcard.js` copies those modules into `sdcard\Windows`.
  `sdoffload.js` is idempotent: if it runs again on an already filtered `ce.bib`, modules from
  the previous list stay on it.
- **Toggle trap:** makeimg only rebuilds `ce.bib` when a `.bib` file changes, not when a catalog
  variable changes. `mksdcard.js` compares `sdoffload.txt` with `BSP_SDOFFLOAD` and stops with
  "changed since ce.bib was built" on a mismatch: delete `ce.bib` in the release dir and make the
  image again.
- **Keep the card in step with the image:** copy the whole `sdcard` folder after every build. A
  module left in the card's `Windows` folder overrides the image's copy, so after removing a
  module from the list (or turning the item off), delete it from the card.

## ROM-only file system: persistent registry on the card (M3)
Status (2026-11-22): **working on a Pi 1 Model B** (Checked). Verified from the boot log:
- First boot on a clean card: `FSREG: Mounting clean system hive`, then
  `FSVOL: Using system database volume "\Windows\Registry\default.vol"`. Second boot:
  `FSREG: Mounted existing system hive`; settings survived Restart and Suspend
  (`FSREG: Flushing registry` before each).
- Boot waits for the card: the card mounts (`Part00`, `CreateCache`) and the system hive loads
  about 0.1 s later on the waiting filesys thread (before M3 the card mounted ~3 s after it).
- `Memory Configuring: Total pages: 51478, Filesystem pages = 32` (was 7338 filesystem pages):
  about 28.5 MB more for programs. Filesystem pages come out of the total, so the total itself only
  changes with the `kernel.img` size.
- The hive clean flag is asked three times per boot (system, users, system); every answer is
  `keep hives`.

Optional, chosen in the OS design: Microsoft catalog item **ROM-only File System**
(`SYSGEN_FSROMONLY`; keep the default Hive-based Registry), together with `BSP_SDROOT=1`. It
drops the RAM file system (`fsysram`), which reserves about 15% of RAM (`FSRAMPERCENT`
`0x25252525`; 7338 "Filesystem pages", about 29 MB, at 224 MB ARM RAM) whether used or not.
- **`platform.reg`** (inside `IF BSP_SDROOT`): under `; @CESYSGEN IF !FILESYS_FSYSRAM` the SD
  profiles get `"MountAsBootable"=dword:1`. The card is then the bootable volume: filesys waits for
  it in boot phase 1 and keeps the hives (`system.hv`, user hives, `default.vol`) in the card's
  `\Windows\Registry`. The `AutoLoad\ObjectStore` override is now under
  `; @CESYSGEN IF FILESYS_FSYSRAM` (that key exists only with `fsysram`).
- **`config.bib`**: `FSRAMPERCENT` is `0x00000010` (32 pages, the CEPC value for persistent
  storage) without `fsysram`, `0x25252525` with it.
- **OAL** (`ioctl.c` `OALIoCtlHalGetHiveCleanFlag`): answers `IOCTL_HAL_GET_HIVE_CLEAN_FLAG`
  with "keep the hives" and logs `[OAL] Hive clean flag 1 (system): keep hives`. The board boots
  with a "clean memory configuration" every time; that is about RAM and must not wipe the card.
- **Registry changes in a new image:** existing hives on the card win over the registry in a new
  `kernel.img`. After a `platform.reg` (or catalog) change, delete `\Windows\Registry` on the card
  so the new defaults apply; `mksdcard.js` reminds you. Deleting it is also the way to reset the
  registry.
- `kernel.img` does not shrink (the RAM FS is not in the image); the gain is RAM and a boot that is
  guaranteed to wait for the card.
- Do not select ROM-only without `BSP_SDROOT=1`: there is then no bootable volume for the hives.
- Build: tick ROM-only File System, **Sysgen** (the `@CESYSGEN` conditions in `platform.reg` and
  `config.bib` are filtered by the BSP build), then Make Run-Time Image. The OAL changed, so check
  `nk.map` for `OALIoCtlHalGetHiveCleanFlag`.
- Still unverified: where `initobj.dat` goes without a RAM FS (`mksdcard.js` already puts its
  items on the card).

## How it works (`raspi2\files\platform.reg`, `IF BSP_SDROOT`)
The card must be mounted before the shell starts, so the SD drivers load in **boot phase 1**:
- `init\BootVars` `"Flags"=dword:3`: storage manager (1) and device manager (2) start in boot phase
  1. The device manager loads drivers from the boot hive, signals `SYSTEM/BootPhase1`, waits for
  phase 2, then loads the rest (`private\winceos\COREOS\device\devcore\devcore.c`).
- The SD host (`SDHCBcm`), SD bus (`SDBusDriver`) and SD memory class keys are in the HIVE BOOT
  SECTION, **only when `BSP_SDROOT=1`**. The bus/memory values are copied verbatim from
  `common.reg`. The device manager loads every `Drivers\BuiltIn` driver in the boot hive in phase 1
  even with `Flags=1` (it does so for `ccfgsvc` too), so the host key must never be in the boot
  hive without the bus key: the host then loads before `SDC_Init` and hits `DEBUGCHK` in
  `sdbus.hpp` line 200 (`g_pSdContainer` NULL). Observed when the item was unticked.
- Trap: the registry compiler treats **any** comment line that starts with the boot-section marker
  text (`; HIVE BOOT SECTION ...`) as a marker, even with more text after it. A wrapped comment
  line like that put `SDHCBcm` back into the boot hive. Check with
  `grep -a -c SDHCBcm` over `boot.hv` with the NUL bytes removed (the hive stores UTF-16 names).
- `SDHCBcm` and `SDBusDriver` get `"Flags"=dword:1000` (`DEVFLAGS_BOOTPHASE_1`, the same pattern as
  `ccfgsvc`): loaded in phase 1 only. Without it the device manager loads a second instance in phase
  2, because its duplicate check (`DeviceContent::IsThisDeviceUnique`) only compares device names.
- `Profiles\SDMemory` / `Profiles\SDHCMemory`: `MountAsRoot=1`, `AutoFormat=0`, `AutoPart=0`.
- SD memory class keys: `"Removable"=dword:0` (read by `sdmemmain.cpp`, default 1). A removable
  volume reports `CE_VOLUME_ATTRIBUTE_REMOVABLE`; the shell (`shcore` `PathIsRemovableDevice`)
  checks the volume of a path's first component, so with a removable root **every folder in `\`
  was drawn as a drive**.
- `AutoLoad\ObjectStore`: `MountAsRoot=0` (only one root volume can exist).
- `CleanBoot\FileExclusionList` lists the boot files, `\overlays` and `\Windows`. What
  `filesys.dll` does to an external root on a clean boot cannot be read from source here (no
  source), and this board always boots with a clean memory configuration, so this is a precaution.
  No files were lost in the first tests.

## Startup items (`initobj.dat`) and the `sdcard` build folder
`filesys.dll` applies `initobj.dat` (desktop shortcuts, `\Windows\Desktop`, `\Windows\Programs`,
`\Program Files`, `\My Documents`, `\Windows\Keys` for DPAPI keys, ...) to the RAM object store at
`FileSystem Starting`, long before the card is mounted. With the card as root those items land in
`\Object Store`, not on the card: the desktop shortcuts are missing and the `\Windows` subfolders
are new and empty.

Fix: makeimg builds them. `PostMakeimg.bat` runs `mksdcard.js` (cscript), which recreates
`<release dir>\sdcard` on every image build:
- `sdcard\kernel.img` - a copy of `nk.nb0` (`nk.nb0` stays in place).
- with `BSP_SDROOT=1`: every folder and file that `initobj.dat` lists, files copied from the
  release dir by their source name (for example `\Windows\Desktop\Shut Down.lnk` from
  `poweroff.lnk`).

Copy the **contents** of `sdcard` to the card root (next to `bootcode.bin`). Existing files on the
card are overwritten; nothing is deleted from the card.

## Boot timing (observed, not guaranteed)
`filesys.dll` does **not** wait for the card: boot phase 2 (`FSREG: Mounting clean system hive`)
starts as soon as the SD host driver finishes `Init`, and the card is detected and mounted
asynchronously on the SD bus thread about 3 s later (`mspart` `Part00`, `diskcache`
`CreateCache`). `explorer.exe` starts about 60 s after that in a Checked build with KITL, so there
is a wide margin, but nothing enforces the order. If something that runs early needs the card,
the options are making the card the bootable volume too (`MountAsBootable=1` on the SD profiles,
`0` on `ObjectStore`; hives then persist in `<card>\Windows\Registry`, and a `platform.reg`
change in a new `kernel.img` must be checked to still apply) or a launch-order helper.

Benign log lines in this mode:
- `HandleErrors - Command line error (0x1)` for CMD5 and CMD1: the SD bus probing for SDIO and MMC
  before it identifies an SD memory card.
- `FSREG: Invalid HKEY` from the SD threads right after `FSREG: Taking down boot registry`: they
  still hold handles from the boot registry. The card enumerates and mounts normally.

## Using it
- **Back up the card first** (for example a full image with Win32 Disk Imager).
- Shut down with Suspend (it flushes the file system before halting); do not just pull the power
  while writing. Never remove the card while CE runs.
- After a `platform.reg` change: Copy Files to Release Directory, then Make Run-Time Image (makeimg
  does not refresh `platform.reg` in the release dir by itself).
- To drop a file that overrides a ROM module, delete it from the card's `Windows` folder.
