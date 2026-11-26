#
# trimnb0.ps1 - write kernel.img from nk.nb0 without romimage's padding.
#
# Called by mksdcard.js (PostMakeimg.bat) after romimage.
#
# romimage pads nk.nb0 to ROMSIZE (= NKLEN in config.bib, the whole NK
# region). The real image ends at ROMHDR.physlast, found through the ROM
# signature at offset 0x40 ('CECE', followed by the ROMHDR virtual address).
# Only that part is written, so the Pi firmware does not read megabytes of
# padding from the card. The memory past the image becomes RAM (AUTOSIZE) and
# the kernel does not rely on its contents. Same method as the CE-on-iPod BSP
# (ipod6g\files\mkrbipod.ps1).
#
# If no valid ROMHDR is found, the whole file is written (with a warning).
#
# Usage:
#   trimnb0.ps1 -InFile nk.nb0 -OutFile sdcard\kernel.img [-NoTrim]
#
# Exit code 0 = ok, 1 = error. Must run on Windows PowerShell 2.0 (Windows 7
# build VM).
#

param(
    [Parameter(Mandatory=$true)] [string] $InFile,
    [Parameter(Mandatory=$true)] [string] $OutFile,
    [switch] $NoTrim
)

$ErrorActionPreference = "Stop"

# 0x80008000 = ROMSTART in config.bib (IMGFLASH). A hex literal would parse as
# a negative Int32, so the decimal value is used.
$ImageBase = [uint32]2147516416

# Bit manipulation lives in C#: PowerShell 2.0 has no -shl/-shr.
$code = @"
public static class TrimNb0
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
Add-Type -TypeDefinition $code -Language CSharp

function Get-FullPath([string] $p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::Combine((Get-Location).Path, $p)
}

$inPath = Get-FullPath $InFile
if (-not (Test-Path -LiteralPath $inPath)) {
    Write-Host "trimnb0: input file not found: $inPath"
    [Environment]::Exit(1)
}
$data = [System.IO.File]::ReadAllBytes($inPath)

$len = $data.Length
if (-not $NoTrim) {
    $imgLen = [TrimNb0]::ImageLength($data, $ImageBase)
    if ($imgLen -gt 0) {
        Write-Host ("trimnb0: CE image is {0} bytes of {1} in {2} (ROMHDR physlast), trimming padding" -f $imgLen, $data.Length, $InFile)
        $len = [int]$imgLen
    }
    else {
        Write-Host ("trimnb0: warning: no valid ROMHDR found (code {0}), writing the whole file" -f $imgLen)
    }
}

$outPath = Get-FullPath $OutFile
$fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
try {
    $fs.Write($data, 0, $len)
}
finally {
    $fs.Close()
}

Write-Host ("trimnb0: wrote {0} ({1} bytes, first word 0x{2:X8})" -f $OutFile, $len, [TrimNb0]::ReadLE32($data, 0))
[Environment]::Exit(0)
