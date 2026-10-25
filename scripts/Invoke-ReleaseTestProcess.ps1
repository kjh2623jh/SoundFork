# Shared only by release tests. Never used by the real-time audio path.
function Invoke-ReleaseTestProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$StepName,

        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [ValidateRange(1, 300)]
        [int]$TimeoutSeconds = 120,

        [string]$LogPath
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "[$([DateTime]::UtcNow.ToString('o'))] START $StepName (timeout ${TimeoutSeconds}s)"
    $process = $null
    try {
        # Do not use Start-Process -Wait: a hidden dialog must not block CI forever.
        $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList `
            -PassThru -WindowStyle Hidden -ErrorAction Stop
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            # Inno Setup's loader can own a child setup process. Kill only this
            # test's process tree; never kill processes by image name.
            $taskkill = Join-Path $env:SystemRoot 'System32\taskkill.exe'
            & $taskkill /PID $process.Id /T /F | Out-Host
            if (-not $process.WaitForExit(10000)) {
                throw "$StepName timed out and its process did not terminate (PID $($process.Id))."
            }
            throw "$StepName timed out after ${TimeoutSeconds}s (PID $($process.Id))."
        }
        $exitCode = $process.ExitCode
        Write-Host "[$([DateTime]::UtcNow.ToString('o'))] END $StepName (exit $exitCode, $([Math]::Round($timer.Elapsed.TotalSeconds, 2))s)"
        return $exitCode
    } catch {
        if ($LogPath -and (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
            Write-Host "Last 80 lines of ${LogPath}:"
            Get-Content -LiteralPath $LogPath -Tail 80 | Out-Host
        }
        throw
    } finally {
        if ($null -ne $process) { $process.Dispose() }
    }
}
