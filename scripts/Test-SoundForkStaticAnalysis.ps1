[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'x86')]
    [string]$Architecture = 'x64',
    [switch]$WithObsPlugin
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repositoryRoot 'build\windows-msvc-analyze'
if ($Architecture -eq 'x86') { $buildDirectory += '-x86' }
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -ne $cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer vswhere.exe was not found.'
    }
    $visualStudio = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    $cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if ([string]::IsNullOrWhiteSpace($visualStudio) -or
        -not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        throw 'Visual Studio bundled CMake was not found.'
    }
}

$obsArguments = @('-DSOUNDFORK_BUILD_OBS_PLUGIN=OFF')
if ($Architecture -eq 'x86' -and $WithObsPlugin) { throw 'OBS plugin must remain x64.' }
if ($WithObsPlugin) {
    & (Join-Path $PSScriptRoot 'Prepare-ObsSdk.ps1')
    $obsArguments = @('-DSOUNDFORK_BUILD_OBS_PLUGIN=ON', "-DSOUNDFORK_OBS_SDK_ROOT=$(Join-Path $repositoryRoot 'build\obs-sdk')")
}
$platform = if ($Architecture -eq 'x86') { 'Win32' } else { 'x64' }
& $cmake -S $repositoryRoot -B $buildDirectory -A $platform @obsArguments `
    -DBUILD_TESTING=ON `
    -DSOUNDFORK_ENABLE_STATIC_ANALYSIS=ON
if ($LASTEXITCODE -ne 0) {
    throw 'Static-analysis CMake configuration failed.'
}
& $cmake --build $buildDirectory --config $Configuration --clean-first -- /m:1
if ($LASTEXITCODE -ne 0) {
    throw 'MSVC static analysis failed.'
}
Write-Host 'MSVC static analysis completed with warnings treated as errors.'
