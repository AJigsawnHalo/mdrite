param([switch]$Clean, [switch]$Debug)

$ErrorActionPreference = 'Stop'
$root = if ($env:WATCOM_ROOT) { $env:WATCOM_ROOT } else { 'C:\WATCOM' }
$out  = 'mdrite.exe'

Push-Location $PSScriptRoot
$oldPath = $env:PATH; $oldInc = $env:INCLUDE; $oldWat = $env:WATCOM

try {
    if ($Clean) {
        Remove-Item build -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item $out, *.err, *.map, *.obj -Force -ErrorAction SilentlyContinue
        Write-Host 'Cleaned'
        return
    }

    $env:WATCOM  = $root
    $env:INCLUDE = "$root\h"
    $env:PATH    = "$root\BINNT64;$root\BINNT;$env:PATH"

	$flags = if ($Debug) { @('-d2') } else { @('-d0', '-os') }
    $srcs  = @(Get-ChildItem *.c | ForEach-Object Name)
    New-Item build -ItemType Directory -Force | Out-Null

    wcl -bt=dos -ml -0 -wx @flags "-fe=$out" '-fo=build\' @srcs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
    Write-Host "OK -> $out"
}
finally {
    $env:PATH = $oldPath; $env:INCLUDE = $oldInc; $env:WATCOM = $oldWat
    Pop-Location
}
