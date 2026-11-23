param([Parameter(Mandatory = $true)][string]$Drive)

$letter = $Drive.Substring(0, 1).ToUpper()

Add-Type @"
using System;
using System.Runtime.InteropServices;

public class VolEject {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    public static string Eject(string letter) {
        IntPtr h = CreateFile(@"\\.\" + letter + ":", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) return "could not open volume, error " + Marshal.GetLastWin32Error();
        try {
            uint r;
            bool locked = false;
            for (int i = 0; i < 10 && !locked; i++) {
                locked = DeviceIoControl(h, 0x90018, null, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
                if (!locked) System.Threading.Thread.Sleep(500);
            }
            if (!locked) return "could not lock volume, it is in use, error " + Marshal.GetLastWin32Error();
            DeviceIoControl(h, 0x90020, null, 0, IntPtr.Zero, 0, out r, IntPtr.Zero);
            DeviceIoControl(h, 0x2D4804, new byte[1], 1, IntPtr.Zero, 0, out r, IntPtr.Zero);
            if (!DeviceIoControl(h, 0x2D4808, null, 0, IntPtr.Zero, 0, out r, IntPtr.Zero))
                return "eject command failed, error " + Marshal.GetLastWin32Error();
            return "";
        } finally {
            CloseHandle(h);
        }
    }
}
"@

$err = [VolEject]::Eject($letter)
if ($err) {
    Write-Host "Direct eject failed: $err"
    Write-Host "Trying the Explorer eject verb ..."
    try {
        (New-Object -ComObject Shell.Application).Namespace(17).ParseName("${letter}:").InvokeVerb("Eject")
    } catch { }
}

Start-Sleep -Seconds 2
if (Test-Path "${letter}:\") { exit 1 } else { exit 0 }
