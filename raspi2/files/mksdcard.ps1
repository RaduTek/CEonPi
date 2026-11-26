#
# mksdcard.ps1 - builds the SD card folder in the flat release directory.
#
# Called by PostMakeimg.bat after romimage. Runs from %_TARGETPLATROOT%\files
# (not from a release-dir copy), so an edit takes effect without Copy Files to
# Release Directory. Creates %_FLATRELEASEDIR%\sdcard from scratch, a complete
# set of files for a blank FAT32 SD card:
#   - every file in sdboot (raspi2\files\sdboot, copied to the release dir by
#     Copy Files to Release Directory): the Raspberry Pi firmware
#     (bootcode.bin, start.elf, fixup.dat, the cut-down start_cd.elf /
#     fixup_cd.dat), LICENCE.broadcom and our config.txt.
#   - sdcard\kernel.img : nk.nb0 cut at the real image end (see below).
#     nk.nb0 itself stays in place.
#   - with BSP_SDROOT=1 : the folders and files that initobj.dat lists for
#     the root file system (desktop shortcuts, \Windows\Desktop,
#     \Windows\Programs, \Program Files, \My Documents, \Windows\Keys, ...).
#     With the SD card as root, filesys applies initobj.dat to the RAM object
#     store before the card is mounted, so these never reach the card by
#     themselves. Files named in initobj.dat (File("name","\Windows\src")) are
#     copied from the release dir by their source file name.
#   - with BSP_SDOFFLOAD=1 : sdcard\Windows gets the modules that
#     sdoffload.ps1 removed from the image (listed in sdoffload.txt).
# Copy the CONTENTS of sdcard to the root of the SD card. See docs\sdroot.md.
#
# kernel.img trimming: romimage pads nk.nb0 to ROMSIZE (= NKLEN in
# config.bib, the whole NK region). The real image ends at ROMHDR.physlast,
# found through the ROM signature at offset 0x40 ('CECE', followed by the
# ROMHDR virtual address). Only that part is written, so the Pi firmware does
# not read megabytes of padding from the card. The memory past the image
# becomes RAM (AUTOSIZE) and the kernel does not rely on its contents. Same
# method as the CE-on-iPod BSP (ipod6g\files\mkrbipod.ps1). Set BSP_NOTRIM=1
# to copy the whole nk.nb0. If no valid ROMHDR is found, the whole file is
# written (with a warning).
#
# Exit code 0 = ok, 1 = error (makeimg then reports a failure). Must run on
# Windows PowerShell 2.0 (Windows 7 build VM).
#

$ErrorActionPreference = "Stop"

# 0x80008000 = ROMSTART in config.bib (IMGFLASH). A hex literal would parse as
# a negative Int32, so the decimal value is used.
$ImageBase = [uint32]2147516416

# Bit manipulation lives in C#: PowerShell 2.0 has no -shl/-shr.
$code = @"
public static class MkSdCard
{
    public static uint ReadLE32(byte[] buf, long offset)
    {
        if (offset < 0 || buf.Length < offset + 4) return 0;
        return System.BitConverter.ToUInt32(buf, (int)offset);
    }

    // Length of the CE image (ROMHDR.physlast - physfirst, rounded up to 4)
    // in a flat .nb0 that starts at 'imageBase'. Negative value = not found:
    //   -1 file too short, -2 no 'CECE' signature at 0x40,
    //   -3 ROMHDR pointer outside the file, -4 physfirst/physlast invalid,
    //   -5 image longer than the file
    public static long ImageLength(byte[] buf, uint imageBase)
    {
        const int SIG_OFFSET = 0x40;
        const uint ROM_SIGNATURE = 0x43454345;

        if (buf.Length < SIG_OFFSET + 8) return -1;
        if (ReadLE32(buf, SIG_OFFSET) != ROM_SIGNATURE) return -2;

        long hdrOffset = (long)ReadLE32(buf, SIG_OFFSET + 4) - imageBase;
        if (hdrOffset < 0 || hdrOffset + 16 > buf.Length) return -3;

        uint physfirst = ReadLE32(buf, hdrOffset + 8);
        uint physlast  = ReadLE32(buf, hdrOffset + 12);
        if (physfirst != imageBase || physlast <= physfirst) return -4;

        long len = ((long)physlast - physfirst + 3) & ~3L;
        if (len > buf.Length) return -5;
        return len;
    }
}
"@

function Write-Log([string] $s) { Write-Host ("mksdcard: " + $s) }
function Stop-Script([string] $s) {
    Write-Host ("mksdcard: ERROR: " + $s)
    [Environment]::Exit(1)
}

# Creates a folder and any missing parents.
function New-Folder([string] $path) {
    [void] [System.IO.Directory]::CreateDirectory($path)
}

# Maps a CE path ("\Windows\Desktop", "Windows", "") to a folder under sdcard.
function Get-SdPath([string] $root, [string] $cePath) {
    $p = $cePath -replace '^\\+', '' -replace '\\+$', ''
    if ($p) { return [System.IO.Path]::Combine($root, $p) }
    return $root
}

function Copy-OneFile([string] $src, [string] $dst) {
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

# Writes kernel.img from nk.nb0, cut at the real image end unless $noTrim.
function Write-KernelImg([string] $nb0, [string] $kimg, [bool] $noTrim) {
    $data = [System.IO.File]::ReadAllBytes($nb0)
    $len = $data.Length
    if (-not $noTrim) {
        $imgLen = [MkSdCard]::ImageLength($data, $ImageBase)
        if ($imgLen -gt 0) {
            Write-Log ("CE image is {0} bytes of {1} in nk.nb0 (ROMHDR physlast), trimming padding" -f $imgLen, $data.Length)
            $len = [int]$imgLen
        }
        else {
            Write-Log ("warning: no valid ROMHDR found in nk.nb0 (code {0}), writing the whole file" -f $imgLen)
        }
    }
    else {
        Write-Log "BSP_NOTRIM=1, writing the whole nk.nb0"
    }

    $fs = [System.IO.File]::Open($kimg, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $fs.Write($data, 0, $len)
    }
    finally {
        $fs.Close()
    }
    Write-Log ("kernel.img written from nk.nb0 ({0} of {1} bytes, first word 0x{2:X8})" -f $len, $data.Length, [MkSdCard]::ReadLE32($data, 0))
}

function Invoke-MkSdCard {
    $frd = $env:_FLATRELEASEDIR
    if (-not $frd -or -not [System.IO.Directory]::Exists($frd)) {
        Stop-Script "_FLATRELEASEDIR is not set or does not exist"
    }

    # Modules moved out of the image (BSP_SDOFFLOAD). sdoffload.ps1 wrote
    # sdoffload.txt when ce.bib was last rebuilt: ON/OFF, then the removed
    # modules. makeimg only rebuilds ce.bib when a .bib file changes, not when a
    # catalog item (environment variable) changes, so a mismatch means the image
    # was built with the old setting.
    $offloadOn = ($env:BSP_SDOFFLOAD -eq "1")
    $offloadList = New-Object System.Collections.ArrayList
    $offloadState = "OFF"
    $listFile = [System.IO.Path]::Combine($frd, "sdoffload.txt")
    if ([System.IO.File]::Exists($listFile)) {
        $lst = [System.IO.File]::ReadAllLines($listFile, [System.Text.Encoding]::Default)
        if ($lst.Length -gt 0) { $offloadState = $lst[0] }
        for ($i = 1; $i -lt $lst.Length; $i++) {
            if ($lst[$i]) { [void] $offloadList.Add($lst[$i]) }
        }
    }
    $expected = "OFF"
    if ($offloadOn) { $expected = "ON" }
    if ($offloadState -cne $expected) {
        Stop-Script ('BSP_SDOFFLOAD ("Load optional modules from the SD card") changed since ce.bib was ' +
                     "built. Delete ce.bib in " + $frd + " and run Make Run-Time Image again.")
    }

    $sd = [System.IO.Path]::Combine($frd, "sdcard")
    if ([System.IO.Directory]::Exists($sd)) {
        try { Remove-Item -LiteralPath $sd -Recurse -Force }
        catch { Stop-Script ("cannot delete " + $sd + " (is a file in it open?)") }
    }
    New-Folder $sd

    # Firmware, licence and config.txt from sdboot. The first four are needed to
    # boot at all; the rest of the folder is copied as it is.
    $boot = [System.IO.Path]::Combine($frd, "sdboot")
    $required = @("bootcode.bin", "start.elf", "fixup.dat", "config.txt")
    if (-not [System.IO.Directory]::Exists($boot)) {
        Stop-Script ($boot + " not found (run Copy Files to Release Directory)")
    }
    foreach ($r in $required) {
        if (-not [System.IO.File]::Exists([System.IO.Path]::Combine($boot, $r))) {
            Stop-Script ($r + " missing from " + $boot)
        }
    }
    $nBoot = 0
    foreach ($f in [System.IO.Directory]::GetFiles($boot)) {
        Copy-OneFile $f ([System.IO.Path]::Combine($sd, [System.IO.Path]::GetFileName($f)))
        $nBoot++
    }
    Write-Log ([string]$nBoot + " boot files copied from sdboot")

    # kernel.img
    $nb0 = [System.IO.Path]::Combine($frd, "nk.nb0")
    if (-not [System.IO.File]::Exists($nb0)) { Stop-Script ("nk.nb0 not found in " + $frd) }
    Write-KernelImg $nb0 ([System.IO.Path]::Combine($sd, "kernel.img")) ($env:BSP_NOTRIM -eq "1")

    # initobj.dat items, only when the SD card is the root file system.
    if ($env:BSP_SDROOT -eq "1") {
        $initobj = [System.IO.Path]::Combine($frd, "initobj.dat")
        if (-not [System.IO.File]::Exists($initobj)) { Stop-Script ("initobj.dat not found in " + $frd) }

        # initobj.dat is UTF-16LE (no BOM).
        $lines = [System.IO.File]::ReadAllLines($initobj, [System.Text.Encoding]::Unicode)
        $re = '^\s*(root|Directory\("([^"]*)"\))\s*:-\s*(Directory|File)\("([^"]*)"(\s*,\s*"([^"]*)")?\)'
        $nDirs = 0
        $nFiles = 0
        $nSkipped = 0

        foreach ($line in $lines) {
            if ([regex]::IsMatch($line, '^\s*(;|$)')) { continue }

            $m = [regex]::Match($line, $re, 'IgnoreCase')
            if (-not $m.Success) {
                Write-Log ("skipped unrecognised line: " + $line)
                $nSkipped++
                continue
            }

            $parent = $m.Groups[2].Value
            if ($m.Groups[1].Value -eq "root") { $parent = "" }
            $parentDir = Get-SdPath $sd $parent
            New-Folder $parentDir

            if ($m.Groups[3].Value -eq "directory") {
                New-Folder ([System.IO.Path]::Combine($parentDir, $m.Groups[4].Value))
                $nDirs++
            }
            else {
                $srcName = $m.Groups[4].Value
                if ($m.Groups[6].Value) { $srcName = $m.Groups[6].Value }
                $srcName = [System.IO.Path]::GetFileName($srcName)
                $src = [System.IO.Path]::Combine($frd, $srcName)
                if (-not [System.IO.File]::Exists($src)) {
                    Write-Log ("WARNING: " + $srcName + " not in release dir, skipped")
                    $nSkipped++
                    continue
                }
                Copy-OneFile $src ([System.IO.Path]::Combine($parentDir, $m.Groups[4].Value))
                $nFiles++
            }
        }
        Write-Log ("initobj.dat: " + $nDirs + " folders, " + $nFiles + " files, " + $nSkipped + " skipped")
    }

    # Modules removed from the image by sdoffload.ps1 go to the card's Windows
    # folder; with the SD card as root they appear in \Windows.
    if ($offloadOn) {
        $winDir = [System.IO.Path]::Combine($sd, "Windows")
        New-Folder $winDir
        foreach ($mod in $offloadList) {
            $msrc = [System.IO.Path]::Combine($frd, $mod)
            if (-not [System.IO.File]::Exists($msrc)) {
                Stop-Script ($mod + " (moved out of the image) not found in " + $frd)
            }
            Copy-OneFile $msrc ([System.IO.Path]::Combine($winDir, $mod))
        }
        Write-Log ([string]$offloadList.Count + " modules moved out of the image copied to Windows")
    }

    # ROM-only file system: the registry hives persist on the card and win over
    # the registry in a new kernel.img (docs\sdroot.md).
    if ($env:BSP_SDROOT -eq "1" -and $env:SYSGEN_FSROMONLY -eq "1") {
        Write-Log "ROM-only file system: the registry is kept in \Windows\Registry on the card;"
        Write-Log "delete that folder on the card for registry changes in this image to apply"
    }

    Write-Log ("done: copy the contents of " + $sd + " to the root of the SD card")
}

try {
    Add-Type -TypeDefinition $code -Language CSharp
    Invoke-MkSdCard
}
catch {
    Stop-Script ($_.Exception.Message)
}
[Environment]::Exit(0)
