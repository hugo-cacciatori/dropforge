# Downloads build-only tools into the ignored build/tools folder after checking
# their pinned SHA-256. Installing Visual Studio is an explicit opt-in. No
# purchases, global PATH edits, automatic restarts or runtime dependencies.
# Runs on Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param(
    [switch]$InstallVisualStudio,
    [switch]$WithNinja
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-toolchain.json') -Raw | ConvertFrom-Json
$downloads = Join-Path $repoRoot 'build\downloads'
$tools = Join-Path $repoRoot 'build\tools'
New-Item -ItemType Directory -Path $downloads, $tools -Force | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Get-PinnedDownload($pin) {
    $destination = Join-Path $downloads $pin.archive
    if (-not (Test-Path -LiteralPath $destination)) {
        Invoke-WebRequest -Uri $pin.url -OutFile $destination -UseBasicParsing
    }
    $hash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($hash -ne $pin.sha256.ToUpperInvariant()) {
        throw "Hash mismatch for $destination. Do not execute or extract this file."
    }
    Write-Output "Verified $($pin.archive)"
    return $destination
}

function Expand-Once([string]$Archive, [string]$Destination, [string]$Probe) {
    if (-not (Test-Path -LiteralPath $Probe)) {
        Expand-Archive -LiteralPath $Archive -DestinationPath $Destination -Force
    }
}

$cmakeRoot = Join-Path $tools "cmake-$($pins.cmake.version)-windows-x86_64"
Expand-Once (Get-PinnedDownload $pins.cmake | Select-Object -Last 1) $tools (Join-Path $cmakeRoot 'bin\cmake.exe')

$validatorRoot = Join-Path $tools "pluginval-$($pins.pluginval.version)"
Expand-Once (Get-PinnedDownload $pins.pluginval | Select-Object -Last 1) $validatorRoot (Join-Path $validatorRoot 'pluginval.exe')

if ($WithNinja) {
    $ninjaRoot = Join-Path $tools "ninja-$($pins.ninja.version)"
    Expand-Once (Get-PinnedDownload $pins.ninja | Select-Object -Last 1) $ninjaRoot (Join-Path $ninjaRoot 'ninja.exe')
}

if ($InstallVisualStudio) {
    $installer = Get-PinnedDownload $pins.visualStudio | Select-Object -Last 1
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw 'Visual Studio installer signature is not valid for Microsoft Corporation.'
    }
    $config = Join-Path $PSScriptRoot $pins.visualStudio.components
    $arguments = '--quiet --wait --norestart --installPath "{0}" --config "{1}" --addProductLang en-US' -f $pins.visualStudio.installPath, $config
    $installation = Start-Process -FilePath $installer -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    if ($installation.ExitCode -ne 0 -and $installation.ExitCode -ne 3010) {
        throw "Visual Studio installation failed (exit $($installation.ExitCode)). See its dd_* setup logs."
    }
    if ($installation.ExitCode -eq 3010) { Write-Warning 'Installer requests a restart. No restart has been performed.' }
}
Write-Output 'Build tools ready under build\tools.'
