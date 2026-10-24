# Validates a built VST3 with the pinned pluginval at strictness 5, as a hidden
# process. GUI tests are skipped so no editor window can ever open on screen;
# the editor is covered by the offscreen CTest suite instead.
# Runs on Windows PowerShell 5.1 and PowerShell 7.
[CmdletBinding()]
param([string]$PluginPath = '')

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-toolchain.json') -Raw | ConvertFrom-Json
$validator = Join-Path $repoRoot "build\tools\pluginval-$($pins.pluginval.version)\pluginval.exe"
if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) { throw 'Run tools/setup-windows.ps1 to fetch pluginval first.' }
if (-not $PluginPath) {
    $PluginPath = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'release-*' |
        ForEach-Object { Join-Path $_.FullName 'Dropforge_artefacts\Release\VST3\Dropforge.vst3' } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $PluginPath -or -not (Test-Path -LiteralPath $PluginPath -PathType Container)) { throw 'Build the Release VST3 first or pass -PluginPath.' }
$plugin = (Resolve-Path -LiteralPath $PluginPath).Path
if ($plugin.Contains('"')) { throw 'Unsupported quote in plugin path.' }

$logs = Join-Path $repoRoot 'build\pluginval'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$arguments = @('--strictness-level', '5', '--random-seed', '12345', '--skip-gui-tests',
    '--output-dir', ('"{0}"' -f $logs), '--validate', ('"{0}"' -f $plugin))
$process = Start-Process -FilePath $validator -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru `
    -RedirectStandardOutput (Join-Path $logs 'stdout.txt') -RedirectStandardError (Join-Path $logs 'stderr.txt')
$output = Get-Content -LiteralPath (Join-Path $logs 'stdout.txt')
$output | Select-Object -Last 15
if ($process.ExitCode -ne 0) {
    Get-Content -LiteralPath (Join-Path $logs 'stderr.txt')
    throw "pluginval failed (exit $($process.ExitCode)); logs in $logs"
}
Write-Output "pluginval strictness 5 passed (GUI tests skipped): $plugin"
