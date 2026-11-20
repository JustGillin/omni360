# Development deploy: build Release with a numbered tag shown after the
# clock ("b7"), copy it to the console over FTP, then put the tag back.
#   powershell -File tools\dev_deploy.ps1 [-NoDeploy]
param([switch]$NoDeploy)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$header = Join-Path $root "src\DevBuild.h"
$counter = Join-Path $root "Release\dev-build-number.txt"
$msbuild = "C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"

$number = 0
if (Test-Path $counter) { $number = [int](Get-Content $counter -TotalCount 1) }
$number++
$stamp = "b$number"

$original = Get-Content $header -Raw
try {
    ($original -replace '#define DEV_BUILD_STAMP ".*"', "#define DEV_BUILD_STAMP `"$stamp`"") |
        Set-Content $header -Encoding ascii -NoNewline
    $started = Get-Date
    # The project's own deploy step targets the old console address and fails;
    # the XEX is written before it, so judge the build by the XEX's time.
    & $msbuild (Join-Path $root "Omni360.sln") /p:Configuration=Release "/p:Platform=Xbox 360" /v:minimal /nologo |
        Where-Object { $_ -match "error|warning C" -and $_ -notmatch "xbecopy" }
    $xex = Join-Path $root "Release\Omni360.xex"
    if (-not (Test-Path $xex) -or (Get-Item $xex).LastWriteTime -lt $started) {
        Write-Error "Build failed: Release\Omni360.xex was not rewritten"
    }
}
finally {
    Set-Content $header $original -Encoding ascii -NoNewline
}

New-Item -ItemType Directory -Force (Split-Path $counter) | Out-Null
Set-Content $counter $number -Encoding ascii
Write-Output "Built $stamp"

if (-not $NoDeploy) {
    python (Join-Path $root "tools\ftp_deploy.py") --local $xex
    if ($LASTEXITCODE -ne 0) { exit 1 }
    Write-Output "Restart Omni360 to load $stamp (B on Home, then Exit)."
}
