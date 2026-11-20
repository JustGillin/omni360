# One audit step: send zero or more bridge taps, wait for the screen to settle,
# then capture the OBS projector.
#   powershell -File tools\audit_step.ps1 -Out frame.png -Keys B,B,DPAD_DOWN
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Keys = @(),
    [int]$GapMs = 450,
    [int]$SettleMs = 900
)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# Under "powershell -File", "-Keys B,B" arrives as one string "B,B", which the
# bridge would press as a chord. Split it so each key is its own tap.
$Keys = @($Keys | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
foreach ($key in $Keys) {
    if ($key) {
        python (Join-Path $here "omni360_input_bridge.py") tap $key | Out-Null
        Start-Sleep -Milliseconds $GapMs
    }
}
Start-Sleep -Milliseconds $SettleMs
& (Join-Path $here "capture_window.ps1") -Out $Out
