[CmdletBinding()]
param(
    [string]$ObsRuntimeDirectory = 'C:\Program Files\obs-studio\bin\64bit'
)
$ErrorActionPreference = 'Stop'
$sdk = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\obs-sdk'
New-Item -ItemType Directory -Path $sdk -Force | Out-Null
$readyMarker = Join-Path $sdk 'soundfork-sdk-32.2.2.ready'
if ((Test-Path -LiteralPath $readyMarker) -and
    (Test-Path -LiteralPath (Join-Path $sdk 'lib\obs.lib')) -and
    (Test-Path -LiteralPath (Join-Path $sdk 'runtime\obs.dll'))) {
    Write-Host "Using the prepared OBS 32.2.2 SDK in '$sdk'."
    return
}

function Copy-RuntimeDlls([string]$Directory) {
    foreach ($dll in Get-ChildItem -LiteralPath $Directory -Filter '*.dll' -File) {
        $destination = Join-Path $runtime $dll.Name
        if ((Test-Path -LiteralPath $destination) -and
            (Get-FileHash -LiteralPath $destination).Hash -eq (Get-FileHash -LiteralPath $dll.FullName).Hash) {
            continue
        }
        Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
    }
}

function Get-CheckedArchive([string]$Name, [string]$Hash) {
    $archive = Join-Path $sdk $Name
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest "https://github.com/obsproject/obs-studio/releases/download/32.2.2/$Name" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) {
        throw "OBS archive checksum mismatch: $Name"
    }
    return $archive
}
$source = Get-CheckedArchive 'OBS-Studio-32.2.2-Sources.tar.gz' 'ec81fb66b03e75ddb3076b576f62679c39262e0e9960cef3e17a40dc5d68e6b4'
& tar -xf $source -C $sdk 'obs-studio-32.2.2-sources/libobs' 'obs-studio-32.2.2-sources/COPYING'
if ($LASTEXITCODE -ne 0) { throw 'Could not extract OBS headers.' }

$runtime = Join-Path $sdk 'runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$installedDll = Join-Path $ObsRuntimeDirectory 'obs.dll'
if ((Test-Path -LiteralPath $installedDll) -and
    (Get-Item -LiteralPath $installedDll).VersionInfo.ProductVersion -eq '32.2.2') {
    # Only for development/tests; these runtime files never enter a release.
    Copy-RuntimeDlls $ObsRuntimeDirectory
} else {
    $binary = Get-CheckedArchive 'OBS-Studio-32.2.2-Windows-x64.zip' '4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1'
    $binaryRoot = Join-Path $sdk 'windows'
    New-Item -ItemType Directory -Path $binaryRoot -Force | Out-Null
    & tar -xf $binary -C $binaryRoot 'bin/64bit'
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract OBS runtime.' }
    Copy-RuntimeDlls (Join-Path $binaryRoot 'bin\64bit')
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudio)) { throw 'Visual Studio x64 C++ tools are required.' }
$tools = Get-ChildItem (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$toolBin = Join-Path $tools.FullName 'bin\Hostx64\x64'
$exports = & (Join-Path $toolBin 'dumpbin.exe') /exports (Join-Path $runtime 'obs.dll')
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect OBS exports.' }
$symbols = @($exports | ForEach-Object {
    if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\w+)\s*$') { $Matches[1] }
})
if ($symbols -notcontains 'obs_source_output_audio') { throw 'Invalid OBS export table.' }
$libraryRoot = Join-Path $sdk 'lib'
New-Item -ItemType Directory -Path $libraryRoot -Force | Out-Null
$definition = Join-Path $libraryRoot 'obs.def'
@('LIBRARY obs.dll', 'EXPORTS') + $symbols | Set-Content -LiteralPath $definition -Encoding ascii
& (Join-Path $toolBin 'lib.exe') /nologo /machine:x64 "/def:$definition" "/out:$(Join-Path $libraryRoot 'obs.lib')"
if ($LASTEXITCODE -ne 0) { throw 'Could not generate OBS import library.' }
Set-Content -LiteralPath $readyMarker -Value '32.2.2' -Encoding ascii
Write-Host "OBS 32.2.2 SDK prepared in '$sdk'."
