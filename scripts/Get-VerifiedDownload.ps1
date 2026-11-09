# Build-time downloads only. Never used by the audio runtime.
function Get-VerifiedDownload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][uri]$Uri,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$Sha256,
        [ValidateRange(1, 5)][int]$Attempts = 4
    )
    $ErrorActionPreference = 'Stop'
    if (Test-Path -LiteralPath $DestinationPath -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $DestinationPath -Algorithm SHA256).Hash -eq $Sha256) {
            return $DestinationPath
        }
        Write-Warning "Discarding corrupt download cache: $DestinationPath"
        Remove-Item -LiteralPath $DestinationPath -Force
    }
    # Never publish a partial file under the verified cache name.
    $partial = $DestinationPath + '.' + [Guid]::NewGuid().ToString('N') + '.partial'
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Write-Host "Downloading $Uri (attempt $attempt/$Attempts)"
            Invoke-WebRequest -Uri $Uri -OutFile $partial -UseBasicParsing -TimeoutSec 300 -ErrorAction Stop
            if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Sha256) {
                throw "Download checksum mismatch: $Uri"
            }
            Move-Item -LiteralPath $partial -Destination $DestinationPath -Force
            return $DestinationPath
        } catch {
            if ($attempt -eq $Attempts) {
                throw "Download failed after $Attempts attempts: $Uri. $($_.Exception.Message)"
            }
            Write-Warning "Download attempt $attempt failed: $($_.Exception.Message)"
        } finally {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        }
        Start-Sleep -Seconds ([int][Math]::Pow(2, $attempt))
    }
}
