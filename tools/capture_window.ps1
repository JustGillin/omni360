# Saves a PNG of a window, found by title, even when other windows cover it.
# Used with the OBS Windowed Projector to see the console during UI audits.
#   powershell -File tools\capture_window.ps1 -Out frame.png [-Title "Windowed Projector"]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Title = "Windowed Projector",
    [int]$Width = 1280
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WinCapture {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    // The first visible top-level window whose title contains part.
    public static IntPtr Find(string part) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            if (!IsWindowVisible(h)) return true;
            var text = new System.Text.StringBuilder(256);
            GetWindowText(h, text, 256);
            if (text.ToString().IndexOf(part, StringComparison.OrdinalIgnoreCase) < 0) return true;
            found = h;
            return false;
        }, IntPtr.Zero);
        return found;
    }
}
"@
# Without this, sizes come back in scaled units on a high-DPI display while
# PrintWindow draws at full resolution, so the capture loses its right and bottom.
[void][WinCapture]::SetProcessDPIAware()

# Every top-level window, not just each process's main one: OBS's main window
# and its projector belong to one process, and which of them counts as "main"
# changes with whichever was used last.
$handle = [WinCapture]::Find($Title)
if ($handle -eq [IntPtr]::Zero) { Write-Error "No window titled like '$Title'"; exit 1 }

$rect = New-Object WinCapture+RECT
[void][WinCapture]::GetClientRect($handle, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { Write-Error "Window is minimised or has no size"; exit 1 }

$bitmap = New-Object System.Drawing.Bitmap $w, $h
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
# 3 = client area only + full content (needed for GPU-drawn windows like OBS)
[void][WinCapture]::PrintWindow($handle, $hdc, 3)
$graphics.ReleaseHdc($hdc)
$graphics.Dispose()

if ($w -gt $Width) {
    $scaledH = [int]($h * $Width / $w)
    $scaled = New-Object System.Drawing.Bitmap $Width, $scaledH
    $g = [System.Drawing.Graphics]::FromImage($scaled)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($bitmap, 0, 0, $Width, $scaledH)
    $g.Dispose()
    $bitmap.Dispose()
    $bitmap = $scaled
}

$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Output "$Out ${w}x${h}"
