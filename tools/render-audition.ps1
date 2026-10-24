# Renders the listening phrases (tools/make_phrases.py) through render_tool for
# one candidate: the full mix plus Anvil-only and Furnace-only stems, which keep
# the full-mix note routing. Output: build/auditions/<Round>/<Label>_<phrase>_<part>.wav
# Runs on Windows PowerShell 5.1 and PowerShell 7.
#
#   powershell -NoProfile -File tools/render-audition.ps1 -Round r1 -Label ironstack
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[a-z0-9-]+$')][string]$Round,
    [Parameter(Mandatory = $true)][ValidatePattern('^[a-z0-9-]+$')][string]$Label,
    [string]$BuildDirectory = '',
    [string[]]$RendererOptions = @()
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) {
    $BuildDirectory = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'release-*' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$pathFile = Join-Path $BuildDirectory 'render-tool-Release.path'
if (-not (Test-Path -LiteralPath $pathFile)) { throw "No Release render_tool in $BuildDirectory; build first." }
$renderer = (Get-Content -LiteralPath $pathFile -Raw).Trim()

$folder = Join-Path $repoRoot "build\auditions\$Round"
$midi = Join-Path $folder 'midi'
New-Item -ItemType Directory -Path $midi -Force | Out-Null
& python -I (Join-Path $PSScriptRoot 'make_phrases.py') $midi | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not write the listening phrases.' }

$parts = [ordered]@{ mix = 'both'; anvil = 'bass'; furnace = 'rhythm' }
foreach ($file in Get-ChildItem -LiteralPath $midi -Filter '*.mid' | Sort-Object Name) {
    foreach ($part in $parts.Keys) {
        $stem = Join-Path $folder ('{0}_{1}_{2}' -f $Label, $file.BaseName, $part)
        foreach ($old in @("$stem.wav", "$stem.json")) { if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old } }
        $arguments = @($file.FullName, "$stem.wav", "$stem.json", '--layer-mask', $parts[$part], '--tail-seconds', '0.5') + $RendererOptions
        & $renderer @arguments | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "render_tool failed for $stem" }
        $metrics = Get-Content -LiteralPath "$stem.json" -Raw | ConvertFrom-Json
        Write-Output ('{0,-36} peak {1,7:N3} dBFS  rms {2,7:N1} dBFS' -f (Split-Path -Leaf "$stem.wav"),
            (20 * [Math]::Log10([Math]::Max($metrics.peak, 1e-9))), (20 * [Math]::Log10([Math]::Max($metrics.rms, 1e-9))))
    }
}
Write-Output "Rendered to $folder"
