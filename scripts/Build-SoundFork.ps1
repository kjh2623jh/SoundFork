[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$Version,
    [switch]$WithObsPlugin,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue

if ($null -eq $cmakeCommand) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Visual Studio Installer vswhere.exe was not found.'
    }
    $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($visualStudio)) {
        throw 'A Visual Studio installation with the x64 C++ toolchain was not found.'
    }
    $bundledCmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (-not (Test-Path -LiteralPath $bundledCmake)) {
        throw 'Visual Studio bundled CMake was not found.'
    }
    $cmake = $bundledCmake
} else {
    $cmake = $cmakeCommand.Source
}

Push-Location $projectRoot
try {
    $configureArguments = @('--preset', 'windows-msvc-x64')
    if ($WithObsPlugin) {
        & (Join-Path $PSScriptRoot 'Prepare-ObsSdk.ps1')
        $configureArguments += '-DSOUNDFORK_BUILD_OBS_PLUGIN=ON'
        $configureArguments += "-DSOUNDFORK_OBS_SDK_ROOT=$(Join-Path $projectRoot 'build\obs-sdk')"
    } else {
        $configureArguments += '-DSOUNDFORK_BUILD_OBS_PLUGIN=OFF'
    }
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $configureArguments += '-USOUNDFORK_VERSION'
    } else {
        if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-(?:alpha|beta|rc)\.[0-9]+)?$') {
            throw "Version '$Version' is not a supported SoundFork release version."
        }
        $configureArguments += "-DSOUNDFORK_VERSION=$Version"
    }
    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    & $cmake --build --preset $Configuration.ToLowerInvariant()
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }

    & $cmake --build build/windows-msvc-x64 --config $Configuration --target soundfork_tests soundfork_host soundfork_driver
    if ($LASTEXITCODE -ne 0) { throw 'Target build failed.' }

    if ($SkipTests) {
        Write-Host 'Compilation completed; CTest execution skipped (-SkipTests).'
    } else {
        $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
        if (-not (Test-Path -LiteralPath $ctest -PathType Leaf)) {
            throw "CTest was not found beside CMake: $ctest"
        }
        & $ctest --test-dir build/windows-msvc-x64 -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'CTest failed. See build/windows-msvc-x64/Testing/Temporary/LastTest.log.' }
    }
} finally {
    Pop-Location
}
