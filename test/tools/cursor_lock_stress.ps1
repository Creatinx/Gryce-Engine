# Stress the Alt-tap toggle: press/release Alt N times and count the engine
# toggles. Each press must flip the pointer state exactly once (edge on press),
# so N presses produce N "alt toggle:" markers that alternate UNLOCKED/LOCKED.
# A shortfall means a press was missed; a surplus means a press double-fired
# (e.g. long-hold auto-repeat leaking through).
#
#   powershell -ExecutionPolicy Bypass -File test/tools/cursor_lock_stress.ps1 [-Cycles 20]
#
# ASCII-only on purpose (PowerShell 5.1 reads .ps1 as ANSI).
param([int]$Cycles = 20, [int]$HoldMs = 120, [int]$GapMs = 250)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Key {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string name);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static void ForceFocus(IntPtr target) {
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), IntPtr.Zero);
        uint me = GetCurrentThreadId();
        AttachThreadInput(me, fg, true);
        BringWindowToTop(target);
        SetForegroundWindow(target);
        SetFocus(target);
        AttachThreadInput(me, fg, false);
    }
}
"@

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$log = Join-Path $env:TEMP "gryce_cursor_stress.log"
$errlog = Join-Path $env:TEMP "gryce_cursor_stress.err"
$smoke = Start-Process -FilePath (Join-Path $root "build\GryceRenderSmoke.exe") `
    -ArgumentList "--api","dx12","--project","test/demo_project","--play" `
    -RedirectStandardOutput $log -RedirectStandardError $errlog -PassThru
Start-Sleep -Seconds 6

# The Alt behaviour only exists while the window has focus (that is the point of
# the feature), and this session cannot be relied on to keep it focused, so take
# focus explicitly before measuring.
$hwnd = [Key]::FindWindowW($null, "Gryce Renderer")
[Key]::ForceFocus($hwnd)
Start-Sleep -Milliseconds 800

for ($i = 0; $i -lt $Cycles; $i++) {
    [Key]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [Key]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds $GapMs
}
Start-Sleep -Milliseconds 500

Stop-Process -Id $smoke.Id -Force
Start-Sleep -Milliseconds 400

# Each Alt press toggles once, so count the dedicated toggle markers and verify
# they alternate UNLOCKED, LOCKED, UNLOCKED, ... (independent of focus noise).
$toggles = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue |
    Where-Object { $_ -like "*alt toggle:*" } |
    ForEach-Object {
        if ($_ -like "*alt toggle: UNLOCKED*") { "UNLOCKED" }
        elseif ($_ -like "*alt toggle: LOCKED*") { "LOCKED" }
    })
$expected = @()
for ($i = 0; $i -lt $Cycles; $i++) {
    if ($i % 2 -eq 0) { $expected += "UNLOCKED" } else { $expected += "LOCKED" }
}
Write-Output "cycles=$Cycles  toggles detected=$($toggles.Count)"
Write-Output ("toggles: " + ($toggles -join " "))
$pass = ($toggles.Count -eq $expected.Count)
if ($pass) {
    for ($i = 0; $i -lt $expected.Count; $i++) {
        if ($toggles[$i] -ne $expected[$i]) { $pass = $false; break }
    }
}
if ($pass) {
    Write-Output "PASS: every Alt press toggled exactly once, alternating"
    exit 0
} else {
    Write-Output "FAIL: toggle count/sequence does not match expectation"
    exit 1
}
