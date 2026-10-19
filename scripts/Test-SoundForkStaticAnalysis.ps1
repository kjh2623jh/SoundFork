[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$WithObsPlugin
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $repositoryRoot 'build\windows-msvc-analyze'
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
if ($WithObsPlugin) {
    & (Join-Path $PSScriptRoot 'Prepare-ObsSdk.ps1')
    $obsArguments = @('-DSOUNDFORK_BUILD_OBS_PLUGIN=ON', "-DSOUNDFORK_OBS_SDK_ROOT=$(Join-Path $repositoryRoot 'build\obs-sdk')")
}
& $cmake -S $repositoryRoot -B $buildDirectory -A x64 @obsArguments `
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
