[CmdletBinding()]
param(
    [ValidateRange(0.01, 24.0)]
    [double]$Hours = 4.0,
    [ValidateRange(1, 300)]
    [int]$PollSeconds = 10,
    [string]$LogPath = (Join-Path $PWD 'soundfork-hardware-soak.csv')
)

$ErrorActionPreference = 'Stop'
$relay = @(Get-Process -Name 'soundfork_discord_relay' -ErrorAction SilentlyContinue)
if ($relay.Count -ne 1) {
    throw 'Start exactly one SoundFork Discord relay before running the hardware soak watcher.'
}

$resolvedLogPath = [IO.Path]::GetFullPath($LogPath)
$deadline = [DateTimeOffset]::Now.AddHours($Hours)
$rows = [Collections.Generic.List[object]]::new()
Write-Host "Watching relay PID $($relay[0].Id) until $deadline."
Write-Host 'Keep DJMAX playing, Discord entire-screen sharing, and OBS capture active.'

while ([DateTimeOffset]::Now -lt $deadline) {
    $current = Get-Process -Id $relay[0].Id -ErrorAction SilentlyContinue
    if ($null -eq $current) {
        throw "Relay exited before the soak completed. Partial log: $resolvedLogPath"
    }
    $rows.Add([PSCustomObject]@{
        Timestamp = [DateTimeOffset]::Now.ToString('o')
        RelayPid = $current.Id
        WorkingSetBytes = $current.WorkingSet64
        PrivateBytes = $current.PrivateMemorySize64
        CpuSeconds = $current.TotalProcessorTime.TotalSeconds
        Threads = $current.Threads.Count
        Handles = $current.HandleCount
    })
    $rows | Export-Csv -LiteralPath $resolvedLogPath -NoTypeInformation -Encoding utf8
    Start-Sleep -Seconds $PollSeconds
}

Write-Host "Hardware soak watcher completed. Log: $resolvedLogPath"
Write-Host 'Manually record: stereo audio, clicks/pops/dropouts, Discord listener audio, OBS meters/recording, microphone separation, and FiiO latency feel.'
