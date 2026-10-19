[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'Build-SoundFork.ps1') -Configuration Release -WithObsPlugin
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else {
    Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$build = Join-Path $projectRoot 'build\windows-msvc-x64'
$cache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') -Raw
if ($cache -notmatch 'SOUNDFORK_BUILD_OBS_PLUGIN:BOOL=ON') {
    throw 'Build with -WithObsPlugin before packaging.'
}
$stage = Join-Path $projectRoot ('build\obs-package-' + [Guid]::NewGuid().ToString('N'))
& $cmake --install $build --config Release --component ObsPlugin --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'OBS plugin staging failed.' }
$artifactRoot = Join-Path $projectRoot 'artifacts'
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
$zip = Join-Path $artifactRoot 'SoundFork-OBS-preview-windows-x64.zip'
Compress-Archive -LiteralPath (Join-Path $stage 'obs-plugin\soundfork-obs') -DestinationPath $zip -Force
Write-Host "OBS preview package: $zip"
Write-Host 'With OBS closed, copy soundfork-obs to %ProgramData%\obs-studio\plugins\.'
Write-Host 'Update SoundFork and its relay from the same source build for simultaneous Discord use.'
