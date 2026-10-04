# Overnight recording of the patch with no phone: keeps the PC from going to sleep (the screen may
# still turn off; the lid must stay open) and runs patch_capture.mjs in drive mode, which keeps the
# bridge connected and saves everything to %USERPROFILE%\BruxAI-data\patch-capture\night-*.jsonl.
# In the morning close this window first, and only then press Disconnect in the app if at all.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File night_recording.ps1

$Host.UI.RawUI.WindowTitle = 'BroxMon night recording - keep this window open'
# int, not uint: PowerShell 5.1 reads 0x80000001 as a negative Int32 (same bits).
Add-Type -Namespace Win32 -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(int flags);'
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED: no sleep while this window runs; the display may still turn off.
[Win32.Power]::SetThreadExecutionState(0x80000001) | Out-Null

Write-Host ''
Write-Host ' BroxMon night recording'
Write-Host ' The computer will not sleep while this window is open (the screen may turn off).'
Write-Host ' Keep the charger connected and the lid open. Close this window in the morning.'
Write-Host ''
try {
  node "$PSScriptRoot\patch_capture.mjs" --drive --prefix night @args
} finally {
  [Win32.Power]::SetThreadExecutionState(0x80000000) | Out-Null
  Write-Host ''
  Write-Host ' Night recording stopped.'
}
