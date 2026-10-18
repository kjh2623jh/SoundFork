[CmdletBinding()]
param()

$relay = @(Get-Process -Name 'soundfork_discord_relay' -ErrorAction SilentlyContinue)
if (-not $relay.Count) {
    Write-Host 'SoundFork Discord relay is already stopped.'
    return
}

$installedExe = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'SoundFork\soundfork_discord_relay.exe'
$developmentExe = Join-Path $PSScriptRoot '..\build\windows-msvc-x64\Release\soundfork_discord_relay.exe'
$relayExe = if (Test-Path -LiteralPath $installedExe -PathType Leaf) {
    $installedExe
} elseif (Test-Path -LiteralPath $developmentExe -PathType Leaf) {
    (Resolve-Path -LiteralPath $developmentExe).Path
} else {
    $null
}

if ($relayExe) {
    & $relayExe --stop
}
$relay | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
if (@(Get-Process -Name 'soundfork_discord_relay' -ErrorAction SilentlyContinue).Count) {
    throw 'The SoundFork Discord relay did not stop within five seconds.'
}
Write-Host "SoundFork Discord relay stopped (PID $($relay.Id -join ', '))."
