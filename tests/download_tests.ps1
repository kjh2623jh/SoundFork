$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\Get-VerifiedDownload.ps1')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('SoundForkDownloadTest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$destination = Join-Path $testRoot 'archive.zip'
$fixture = [Text.Encoding]::UTF8.GetBytes('verified archive fixture')
$hasher = [Security.Cryptography.SHA256]::Create()
try { $hash = [BitConverter]::ToString($hasher.ComputeHash($fixture)).Replace('-', '') }
finally { $hasher.Dispose() }
$script:calls = 0
$script:sleeps = @()
$script:mode = 'transient'
function Start-Sleep { param([int]$Seconds) $script:sleeps += $Seconds }
function Invoke-WebRequest {
    param($Uri, $OutFile, [switch]$UseBasicParsing, $TimeoutSec, $ErrorAction)
    $script:calls++
    if ($TimeoutSec -ne 300) { throw 'Missing download timeout' }
    if ($script:mode -eq 'transient' -and $script:calls -lt 3) {
        [IO.File]::WriteAllBytes($OutFile, [byte[]](1, 2, 3))
        throw 'Simulated connection reset after a partial write'
    }
    if ($script:mode -eq 'reset') { throw 'Simulated permanent connection reset' }
    if ($script:mode -eq 'bad-hash') { [IO.File]::WriteAllBytes($OutFile, [byte[]](9, 9)) }
    else { [IO.File]::WriteAllBytes($OutFile, $fixture) }
}
function Require([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Download { Get-VerifiedDownload -Uri 'https://example.invalid/archive.zip' -DestinationPath $destination -Sha256 $hash }
try {
    Require ((Download) -ceq $destination) 'Wrong verified cache path'
    Require ($script:calls -eq 3) 'Transient connection reset was not retried'
    Require (($script:sleeps -join ',') -eq '2,4') 'Unexpected retry backoff'
    Require ((Get-FileHash -LiteralPath $destination).Hash -eq $hash) 'Bad file published'
    Download | Out-Null
    Require ($script:calls -eq 3) 'Valid cache was downloaded again'
    [IO.File]::WriteAllBytes($destination, [byte[]](0))
    $script:mode = 'valid'
    Download | Out-Null
    Require ($script:calls -eq 4) 'Corrupt cache was not replaced'
    Require ((Get-FileHash -LiteralPath $destination).Hash -eq $hash) 'Corrupt cache accepted'
    Remove-Item -LiteralPath $destination
    foreach ($mode in @('bad-hash', 'reset')) {
        $script:mode = $mode; $script:calls = 0
        $failed = $false
        try { Download | Out-Null }
        catch { if ($_.Exception.Message -notlike 'Download failed after 4 attempts:*') { throw }; $failed = $true }
        Require $failed 'Permanent failure did not fail the build'
        Require ($script:calls -eq 4) 'Retry count was not bounded'
        Require (-not (Test-Path -LiteralPath $destination)) 'Failed download polluted cache'
        Require (@(Get-ChildItem -LiteralPath $testRoot -Filter '*.partial').Count -eq 0) 'Partial file leaked'
    }
    Write-Host 'Download retry, cache integrity, timeout and partial-file cleanup tests passed (offline).'
} finally {
    # Exact private fixture directory created above; never remove a shared temp root.
    $resolvedFixture = (Resolve-Path -LiteralPath $testRoot).Path
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedFixture.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedFixture -Leaf) -notlike 'SoundForkDownloadTest-*') { throw 'Unsafe fixture cleanup' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
