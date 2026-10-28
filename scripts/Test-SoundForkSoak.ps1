[CmdletBinding()]
param(
    [ValidateRange(0.01, 24.0)]
    [double]$Hours = 4.0,
    [ValidateSet('int16', 'int24', 'int32', 'float32', 'all')]
    [string]$Format = 'all',
    [ValidateRange(1, 8192)]
    [int]$BufferSize = 64,
    [switch]$WithObsPlugin,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Format = $Format.ToLowerInvariant()
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'Build-SoundFork.ps1'
$soakExe = Join-Path $repositoryRoot 'build\windows-msvc-x64\Release\soundfork_soak_tests.exe'
$relayTestExe = Join-Path $repositoryRoot 'build\windows-msvc-x64\Release\soundfork_relay_tests.exe'

if (-not $SkipBuild) {
    & $buildScript -Configuration Release -WithObsPlugin:$WithObsPlugin
}
if (-not (Test-Path -LiteralPath $soakExe -PathType Leaf) -or
    -not (Test-Path -LiteralPath $relayTestExe -PathType Leaf)) {
    throw 'Release soak test executables were not found. Build the Release configuration first.'
}

& $soakExe --hours $Hours.ToString([Globalization.CultureInfo]::InvariantCulture) --format $Format --buffer-size $BufferSize
if ($LASTEXITCODE -ne 0) {
    throw "Accelerated ASIO soak failed with exit code $LASTEXITCODE."
}
if ($SkipBuild) {
    # Without a build, CTest has not already run the deterministic drift tests.
    & $relayTestExe
    if ($LASTEXITCODE -ne 0) { throw "Relay drift soak failed with exit code $LASTEXITCODE." }
}

Write-Host 'Accelerated soak completed. This does not replace a real-time FiiO, Discord, and OBS hardware soak.'
