[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string[]]$Path,
    [Parameter(Mandatory)]
    [string]$CertificateThumbprint,
    [Parameter(Mandatory)]
    [uri]$TimestampUrl
)

$ErrorActionPreference = 'Stop'
$normalizedThumbprint = $CertificateThumbprint.Replace(' ', '').ToUpperInvariant()
if ($normalizedThumbprint -notmatch '^[0-9A-F]{40}$') {
    throw 'CertificateThumbprint must be a 40-character SHA-1 certificate thumbprint.'
}
if ($TimestampUrl.Scheme -notin @('http', 'https')) {
    throw 'TimestampUrl must use HTTP or HTTPS.'
}

$certificate = @(
    Get-ChildItem "Cert:\CurrentUser\My\$normalizedThumbprint" -ErrorAction SilentlyContinue
    Get-ChildItem "Cert:\LocalMachine\My\$normalizedThumbprint" -ErrorAction SilentlyContinue
) | Where-Object { $_.HasPrivateKey } | Select-Object -First 1
if ($null -eq $certificate) {
    throw "A code-signing certificate with a private key was not found for thumbprint $normalizedThumbprint."
}
$codeSigningOid = '1.3.6.1.5.5.7.3.3'
if (-not ($certificate.EnhancedKeyUsageList.ObjectId.Value -contains $codeSigningOid)) {
    throw 'The selected certificate is not valid for code signing.'
}
if ($certificate.NotAfter -le [DateTime]::Now) {
    throw 'The selected code-signing certificate has expired.'
}

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signTool = Get-ChildItem $kitsRoot -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'x64\\signtool\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $signTool) {
    throw 'The Windows SDK x64 signtool.exe was not found.'
}

foreach ($item in $Path) {
    $resolvedPath = (Resolve-Path -LiteralPath $item).Path
    & $signTool sign /sha1 $normalizedThumbprint /fd SHA256 `
        /tr $TimestampUrl.AbsoluteUri /td SHA256 /v $resolvedPath
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode signing failed: $resolvedPath"
    }
    & $signTool verify /pa /all $resolvedPath
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed: $resolvedPath"
    }
}
