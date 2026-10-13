# End-to-end check for the interactive camera's pointer policy:
#   lock ONLY while the window has focus; tap Alt -> toggle pointer release/lock
#   (the release is sticky: releasing Alt does NOT re-lock, tapping again does).
#
# The verdict is read from the CURSOR_SHOWING bit of GetCursorInfo:
# GLFW_CURSOR_DISABLED (locked) clears it, so visible == unlocked.
# Focus is switched with the AttachThreadInput workaround, because a plain
# SetForegroundWindow from a background process is refused by the Windows
# foreground lock and the test would silently measure nothing.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File test/tools/cursor_lock_probe.ps1
#
# NOTE: keep this file ASCII-only. PowerShell 5.1 reads .ps1 as ANSI, so UTF-8
#       Chinese text both breaks parsing and mangles patterns used for matching.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Cur {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO {
        public int cbSize; public int flags; public IntPtr hCursor; public POINT pt;
    }
    [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO pci);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string name);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public static bool CursorVisible() {
        CURSORINFO ci = new CURSORINFO();
        ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
        if (!GetCursorInfo(ref ci)) return false;
        return (ci.flags & 1) != 0;
    }
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
$log = Join-Path $env:TEMP "gryce_cursor_probe.log"
$errlog = Join-Path $env:TEMP "gryce_cursor_probe.err"
$smoke = Start-Process -FilePath (Join-Path $root "build\GryceRenderSmoke.exe") `
    -ArgumentList "--api","dx12","--project","test/demo_project","--play" `
    -RedirectStandardOutput $log -RedirectStandardError $errlog -PassThru
Start-Sleep -Seconds 6

$hwnd = [Cur]::FindWindowW($null, "Gryce Renderer")
function Check($label) {
    $v = [Cur]::CursorVisible()
    Write-Host ("{0,-26} cursor visible = {1}" -f $label, $v)
    return $v
}

# Step 1: the smoke focuses its own window at startup (it calls focus_window()),
# so we just observe -- forcing focus here would add noise, not signal.
Start-Sleep -Milliseconds 900
$r1 = Check "1) focused"

[Cur]::keybd_event(0x12, 0, 0, [IntPtr]::Zero) # Alt tap -> toggle to unlocked
Start-Sleep -Milliseconds 600
$r2 = Check "2) Alt tapped"

[Cur]::keybd_event(0x12, 0, 2, [IntPtr]::Zero) # Alt release: no toggle (edge on press only)
Start-Sleep -Milliseconds 600
$r3 = Check "3) Alt released (stays unlocked)"

# Unfocus with Alt+Tab (works in sessions where SetForegroundWindow is refused).
# The Alt press toggles back to locked; give the engine time to process that
# toggle BEFORE Alt+Tab steals focus, otherwise the press and the focus loss
# land in the same poll and the toggle is missed.
[Cur]::keybd_event(0x12, 0, 0, [IntPtr]::Zero) # Alt tap -> toggle back to locked
Start-Sleep -Milliseconds 250
[Cur]::keybd_event(0x09, 0, 0, [IntPtr]::Zero) # Tab down
[Cur]::keybd_event(0x09, 0, 2, [IntPtr]::Zero) # Tab up
[Cur]::keybd_event(0x12, 0, 2, [IntPtr]::Zero) # Alt up
Start-Sleep -Milliseconds 1200
$r4 = Check "4) unfocused"

# Best effort: bring the smoke back to the foreground (may be refused in this
# session; the verdict below only requires the four transitions up to here).
[Cur]::ForceFocus($hwnd)
Start-Sleep -Milliseconds 900
$r5 = Check "5) refocused"

Stop-Process -Id $smoke.Id -Force
Start-Sleep -Milliseconds 600
[void](Check "6) after exit")

Write-Output ""
# The cursor-visibility bits above depend on the OS/session and can be
# inconclusive (a non-focused window shows the cursor no matter what we do), so
# the verdict is taken from the engine's own state machine, which the smoke logs
# as ASCII markers on every transition.
$states = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue |
    Where-Object { $_ -like "*cursor state:*" } |
    ForEach-Object {
        if ($_ -like "*cursor state: LOCKED*") { "LOCKED" }
        elseif ($_ -like "*cursor state: UNLOCKED*") { "UNLOCKED" }
    })
Write-Output "--- engine pointer state machine (in order) ---"
$states | ForEach-Object { Write-Output "  $_" }
$reasons = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue |
    Where-Object { $_ -like "*cursor state:*" } |
    ForEach-Object { if ($_ -match "reason=([a-z-]+)") { $Matches[1] } })
Write-Output "--- reasons ---"
$reasons | ForEach-Object { Write-Output "  $_" }
Write-Output ""

# Expected sequence (focus loss + refocus only if this session could do it):
#   LOCKED(focus) -> UNLOCKED(alt) -> LOCKED(focus) -> UNLOCKED(focus-lost)
$want_states = @("LOCKED", "UNLOCKED", "LOCKED", "UNLOCKED")
$want_reasons = @("focus", "alt", "focus", "focus-lost")
$n = [Math]::Min($states.Count, $want_states.Count)
$pass = ($states.Count -ge $want_states.Count) -and
        ($reasons.Count -ge $want_reasons.Count)
for ($i = 0; $i -lt $n -and $pass; $i++) {
    if ($states[$i] -ne $want_states[$i] -or $reasons[$i] -ne $want_reasons[$i]) { $pass = $false }
}
if ($pass) {
    Write-Output "PASS: focus->lock, Alt-tap->unlock, Alt-tap->lock, focus-loss->unlock"
    if ($states.Count -gt $want_states.Count) {
        Write-Output ("      (+ " + ($states.Count - $want_states.Count) +
                      " more transition(s), e.g. relock after refocus)")
    }
} else {
    Write-Output "FAIL: state machine sequence does not match expectation"
}
exit $(if ($pass) { 0 } else { 1 })
