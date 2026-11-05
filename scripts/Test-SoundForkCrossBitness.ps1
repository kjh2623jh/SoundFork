[CmdletBinding()]
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release', [switch]$AllFormats)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root "build\interop-$Configuration"
New-Item -ItemType Directory -Path (Join-Path $stage 'x86') -Force | Out-Null
foreach ($name in @('soundfork_cross_bitness_tests.exe', 'soundfork_capture_hub.exe')) {
    Copy-Item -LiteralPath (Join-Path $root "build\windows-msvc-x64\$Configuration\$name") -Destination $stage -Force
}
Copy-Item -LiteralPath (Join-Path $root "build\windows-msvc-x86\$Configuration\soundfork_cross_bitness_tests.exe") -Destination (Join-Path $stage 'x86') -Force
function Start-TestWorker([string]$Executable, [string]$Arguments) {
    $worker = [Diagnostics.Process]::new()
    $worker.StartInfo.FileName = $Executable
    $worker.StartInfo.Arguments = $Arguments
    $worker.StartInfo.UseShellExecute = $false
    $worker.StartInfo.CreateNoWindow = $true
    $worker.StartInfo.RedirectStandardOutput = $true
    $worker.StartInfo.RedirectStandardError = $true
    if (-not $worker.Start()) { $worker.Dispose(); throw "Could not start $Executable" }
    return $worker
}
foreach ($reverse in @($false, $true)) {
    foreach ($format in $(if ($AllFormats) { 0..17 } else { @(0,1,2,3,4,9,11,17) })) {
        foreach ($frames in @(64, 256)) {
            $scope = 'interop-' + [Guid]::NewGuid().ToString('N')
            $x64 = Join-Path $stage 'soundfork_cross_bitness_tests.exe'
            $x86 = Join-Path $stage 'x86\soundfork_cross_bitness_tests.exe'
            $producerPath = if ($reverse) { $x64 } else { $x86 }
            $consumerPath = if ($reverse) { $x86 } else { $x64 }
            Write-Host "Interop: reverse=$reverse, format=$format, frames=$frames"
            $producer = $null; $consumer = $null
            try {
                $producer = Start-TestWorker $producerPath "producer $scope $format $frames"
                $consumer = Start-TestWorker $consumerPath "consumer $scope $format $frames"
                if (-not $producer.WaitForExit(20000) -or -not $consumer.WaitForExit(20000)) { throw 'Cross-bitness test timed out.' }
                if ($producer.ExitCode -ne 0 -or $consumer.ExitCode -ne 0) {
                    throw "Cross-bitness test failed: producer=$($producer.ExitCode), consumer=$($consumer.ExitCode), format=$format, frames=$frames"
                }
            } finally {
                $role = 'producer'
                foreach ($process in @($producer, $consumer)) {
                    if ($null -ne $process) {
                        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
                        if ($process.HasExited) {
                            $process.StandardOutput.ReadToEnd() | Set-Content -LiteralPath (Join-Path $stage "$scope-$role.log")
                            $process.StandardError.ReadToEnd() | Set-Content -LiteralPath (Join-Path $stage "$scope-$role.err")
                        }
                        $process.Dispose()
                    }
                    $role = 'consumer'
                }
            }
        }
    }
}
Write-Host 'Cross-bitness proxy -> x64 hub -> dual consumers passed in both directions, selected PCM formats, 64/256 frames.'
