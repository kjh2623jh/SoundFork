[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
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
    & $cmake --preset windows-msvc-x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    & $cmake --build --preset $Configuration.ToLowerInvariant()
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }

    & $cmake --build build/windows-msvc-x64 --config $Configuration --target soundfork_tests soundfork_host soundfork_driver
    if ($LASTEXITCODE -ne 0) { throw 'Target build failed.' }

    & $cmake --build build/windows-msvc-x64 --config $Configuration --target RUN_TESTS
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
} finally {
    Pop-Location
}

