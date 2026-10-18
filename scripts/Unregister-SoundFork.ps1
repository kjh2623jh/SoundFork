[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated 64-bit PowerShell session. It will not self-elevate.'
}
if (-not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit PowerShell to unregister the x64 SoundFork driver.'
}

$resolvedDll = (Resolve-Path -LiteralPath $DllPath).Path
$regsvr32 = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
if ($PSCmdlet.ShouldProcess($resolvedDll, 'Unregister SoundFork')) {
    $unregisterArguments = "/u /s `"$resolvedDll`""
    $process = Start-Process -FilePath $regsvr32 -ArgumentList $unregisterArguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "regsvr32 /u failed with exit code $($process.ExitCode)."
    }
    Write-Host 'SoundFork was unregistered. Existing ASIO drivers were not modified.'
}
