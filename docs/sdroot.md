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
  longer the root.

Planned next (M2): a second catalog item that takes selected modules out of `kernel.img` and stages
them for the card's `Windows` folder.

## How it works (`raspi2\files\platform.reg`, `IF BSP_SDROOT`)
The card must be mounted before the shell starts, so the SD drivers load in **boot phase 1**:
- `init\BootVars` `"Flags"=dword:3`: storage manager (1) and device manager (2) start in boot phase
  1. The device manager loads drivers from the boot hive, signals `SYSTEM/BootPhase1`, waits for
  phase 2, then loads the rest (`private\winceos\COREOS\device\devcore\devcore.c`).
- The SD host (`SDHCBcm`), SD bus (`SDBusDriver`) and SD memory class keys are in the HIVE BOOT
  SECTION. The bus/memory values are copied verbatim from `common.reg`.
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
