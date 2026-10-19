[CmdletBinding()]
param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build\windows-msvc-x64\Release')
)

$ErrorActionPreference = 'Stop'
$resolvedBin = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio vswhere.exe was not found.'
}
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'Visual Studio with the x64 C++ toolchain was not found.'
}
$dumpbin = Get-ChildItem (Join-Path $visualStudio 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe |
    Where-Object { $_.FullName -match 'Hostx64\\x64\\dumpbin\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) {
    throw 'The x64 dumpbin.exe tool was not found.'
}

$binaries = @(
    'soundfork_driver.dll',
    'soundfork_control.exe',
    'soundfork_discord_relay.exe',
    'soundfork_capture_hub.exe',
    'soundfork_capture_monitor.exe',
    'soundfork_host.exe'
)
if (Test-Path -LiteralPath (Join-Path $resolvedBin 'soundfork-obs.dll')) {
    $binaries += 'soundfork-obs.dll'
}
foreach ($name in $binaries) {
    $path = Join-Path $resolvedBin $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Release binary was not found: $path"
    }
    $headers = (& $dumpbin /headers $path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $headers -notmatch '8664 machine \(x64\)') {
        throw "$name is not a valid x64 PE image."
    }
    if ($headers -notmatch 'Control Flow Guard' -or
        $headers -notmatch 'NX compatible' -or
        $headers -notmatch 'Dynamic base') {
        throw "$name is missing CFG, NX, or ASLR protection."
    }
    $dependencies = (& $dumpbin /dependents $path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not inspect dependencies for $name."
    }
    if ($dependencies -match '(?im)^\s+(?:VCRUNTIME|MSVCP|ucrtbase)[^\r\n]*\.dll\s*$') {
        throw "$name has an unexpected dynamic MSVC runtime dependency: $($Matches[0].Trim())"
    }
}
Write-Host 'Release binaries are x64, CFG/NX/ASLR enabled, and use no dynamic MSVC runtime DLLs.'
