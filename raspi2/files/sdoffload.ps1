#
# sdoffload.ps1 - removes the modules listed for the SD card from ce.bib.
#
# Called by PostFmergeBib.bat, which makeimg runs right after fmerge writes
# ce.bib and before romimage (public\common\oak\misc\makeimg.proj,
# CreateConfigBibFile). That target only runs when a .bib input changed or
# ce.bib is missing. The script runs from %_TARGETPLATROOT%\files (not from a
# release-dir copy), so an edit takes effect without Copy Files to Release
# Directory.
#
# With BSP_SDOFFLOAD=1 (catalog item "Load optional modules from the SD
# card"; needs BSP_SDROOT=1) every MODULES entry named on a
# "; SDOFFLOAD <name>" comment line (platform.bib) is dropped from ce.bib, so
# romimage leaves it out of the image. mksdcard.ps1 later copies those modules
# into sdcard\Windows; with the SD card as root they appear in \Windows.
#
# Always writes %_FLATRELEASEDIR%\sdoffload.txt: first line ON or OFF (the
# BSP_SDOFFLOAD value this ce.bib was built with), then one removed module
# per line. mksdcard.ps1 uses it to copy the modules and to detect a toggled
# catalog item whose ce.bib was not rebuilt.
#
# ce.bib and sdoffload.txt are read and written in the ANSI code page with
# CRLF line ends ([Text.Encoding]::Default, as the old JScript FSO did). Never
# use Out-File or ">" here: Windows PowerShell 2.0 writes UTF-16 with them.
#
# Exit code 0 = ok, 1 = error (makeimg then stops). Must run on Windows
# PowerShell 2.0 (Windows 7 build VM).
#

$ErrorActionPreference = "Stop"

function Write-Log([string] $s) { Write-Host ("sdoffload: " + $s) }
function Stop-Script([string] $s) {
    Write-Host ("sdoffload: ERROR: " + $s)
    [Environment]::Exit(1)
}

function Write-Lines([string] $path, $lines) {
    [System.IO.File]::WriteAllLines($path, [string[]] @($lines), [System.Text.Encoding]::Default)
}

function Invoke-SdOffload {
    $frd = $env:_FLATRELEASEDIR
    if (-not $frd -or -not [System.IO.Directory]::Exists($frd)) {
        Stop-Script "_FLATRELEASEDIR is not set or does not exist"
    }
    $bib = [System.IO.Path]::Combine($frd, "ce.bib")
    $listFile = [System.IO.Path]::Combine($frd, "sdoffload.txt")

    if ($env:BSP_SDOFFLOAD -ne "1") {
        Write-Lines $listFile @("OFF")
        Write-Log "BSP_SDOFFLOAD not set, ce.bib unchanged"
        return
    }
    if ($env:BSP_SDROOT -ne "1") {
        Stop-Script ('BSP_SDOFFLOAD=1 needs BSP_SDROOT=1 ("SD card as root file system"): ' +
                     'without it nothing loads the modules from the card')
    }
    if (-not [System.IO.File]::Exists($bib)) { Stop-Script ("ce.bib not found in " + $frd) }

    # Read ce.bib and collect the list.
    $lines = [System.IO.File]::ReadAllLines($bib, [System.Text.Encoding]::Default)

    $wanted = @{}      # name -> name as listed (PowerShell keys ignore case)
    $order = New-Object System.Collections.ArrayList
    foreach ($line in $lines) {
        $m = [regex]::Match($line, '^\s*;\s*SDOFFLOAD\s+(\S+)', 'IgnoreCase')
        if ($m.Success -and -not $wanted.ContainsKey($m.Groups[1].Value)) {
            $wanted[$m.Groups[1].Value] = $m.Groups[1].Value
            [void] $order.Add($m.Groups[1].Value)
        }
    }

    # Drop the matching entries, only inside the MODULES section.
    $out = New-Object System.Collections.ArrayList
    $removed = @{}
    $section = ""
    foreach ($line in $lines) {
        $sec = [regex]::Match($line, '^\s*(MEMORY|CONFIG|MODULES|FILES)\s*$', 'IgnoreCase')
        if ($sec.Success) { $section = $sec.Groups[1].Value.ToUpper() }

        if ($section -eq "MODULES" -and -not [regex]::IsMatch($line, '^\s*(;|$)')) {
            $name = ($line.TrimStart() -split '\s+')[0]
            if ($wanted.ContainsKey($name)) {
                if ($removed.ContainsKey($name)) {
                    Stop-Script ($name.ToLower() + " is listed twice in the MODULES section of ce.bib")
                }
                $removed[$name] = $true
                continue
            }
        }
        [void] $out.Add($line)
    }

    # Modules removed by an earlier run on this same ce.bib (if fmerge left the
    # filtered file in place) are no longer in it; keep them on the list.
    $previous = @{}
    if ([System.IO.File]::Exists($listFile)) {
        $prev = [System.IO.File]::ReadAllLines($listFile, [System.Text.Encoding]::Default)
        if ($prev.Length -gt 0 -and $prev[0] -ceq "ON") {
            for ($i = 1; $i -lt $prev.Length; $i++) {
                if ($prev[$i]) { $previous[$prev[$i]] = $true }
            }
        }
    }

    $result = New-Object System.Collections.ArrayList
    [void] $result.Add("ON")
    $nRemoved = 0
    $nAlready = 0
    foreach ($n in $order) {
        if ($removed.ContainsKey($n)) {
            [void] $result.Add($wanted[$n])
            $nRemoved++
        }
        elseif ($previous.ContainsKey($n)) {
            [void] $result.Add($wanted[$n])
            $nAlready++
        }
        else {
            Write-Log ("WARNING: " + $wanted[$n] + " is not a module in this image, skipped")
        }
    }

    # Replace ce.bib through a temporary file.
    $tmp = $bib + ".sdoffload.tmp"
    Write-Lines $tmp $out
    [System.IO.File]::Delete($bib)
    [System.IO.File]::Move($tmp, $bib)
    Write-Lines $listFile $result

    $msg = "removed " + $nRemoved + " of " + $order.Count + " listed modules from ce.bib"
    if ($nAlready) { $msg += " (" + $nAlready + " more were already removed)" }
    Write-Log $msg
}

try {
    Invoke-SdOffload
}
catch {
    Stop-Script ($_.Exception.Message)
}
[Environment]::Exit(0)
