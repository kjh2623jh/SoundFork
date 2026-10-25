[CmdletBinding()]
param(
    [string]$Endpoint
)

$ErrorActionPreference = 'Stop'
$existing = @(Get-Process -Name 'soundfork_discord_relay' -ErrorAction SilentlyContinue)
if ($existing.Count) {
    throw "SoundFork audio forwarding is already running (PID $($existing.Id -join ', '))."
}
if (Get-Process -Name 'soundfork_capture_monitor' -ErrorAction SilentlyContinue) {
    throw 'Stop soundfork_capture_monitor first. The shared ring supports one consumer.'
}

$installedExe = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'SoundFork\soundfork_discord_relay.exe'
$developmentExe = Join-Path $PSScriptRoot '..\build\windows-msvc-x64\Release\soundfork_discord_relay.exe'
$relayExe = if (Test-Path -LiteralPath $installedExe -PathType Leaf) {
    $installedExe
} elseif (Test-Path -LiteralPath $developmentExe -PathType Leaf) {
    (Resolve-Path -LiteralPath $developmentExe).Path
} else {
    throw 'soundfork_discord_relay.exe was not found. Build Release or install SoundFork first.'
}

$selectedEndpoint = $Endpoint
if ([string]::IsNullOrWhiteSpace($selectedEndpoint)) {
    $endpointLines = @(& $relayExe --list-endpoints)
    $endpointNames = @($endpointLines | Where-Object { $_ -and -not $_.StartsWith('  ') })
    $selectedEndpoint = $endpointNames | Where-Object {
        $_ -match '(?i)CABLE Input|VB-Audio Virtual Cable|VB-Audio Cable'
    } | Select-Object -First 1
    if (-not $selectedEndpoint) {
        $selectedEndpoint = $endpointNames | Where-Object {
            $_ -match '(?i)VoiceMeeterDesktop|VoiceMeeter Input'
        } | Select-Object -First 1
    }
    if (-not $selectedEndpoint) {
        $selectedEndpoint = $endpointNames | Where-Object {
            $_ -match '(?i)VoiceMeeter'
        } | Select-Object -First 1
    }
    if (-not $selectedEndpoint) {
        throw 'No supported broadcast endpoint was found. Install VB-CABLE or VoiceMeeter, then run this command again.'
    }
}

$quotedEndpoint = '"' + $selectedEndpoint.Replace('"', '\"') + '"'
$arguments = @('--endpoint', $quotedEndpoint, '--system-share', '--persistent')
$process = Start-Process -FilePath $relayExe -ArgumentList $arguments -PassThru -WindowStyle Hidden
Write-Host "SoundFork audio forwarding started on '$selectedEndpoint' (PID $($process.Id))."
Write-Host 'Use Discord entire-screen sharing. Application-window audio is not supported by the validated path.'
