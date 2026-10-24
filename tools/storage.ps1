# Reports disk use and, with -Apply, reclaims space by fixed rules. It deletes
# only regenerable files: renders, logs, caches, stale build trees and superseded
# local releases. Source, git folders, portable tools, the pinned JUCE copy and
# the newest release are never touched. Runs on Windows PowerShell 5.1 and 7.
#
# Audition audio waits for the owner's answer, so it is deleted only when asked:
#   powershell -NoProfile -File tools/storage.ps1 -Apply -Auditions
[CmdletBinding()]
param([switch]$Apply, [switch]$Auditions)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$projectLimit = 1GB
$cacheLimit = 100MB
$pipLimit = 512MB

function Get-Size([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    $sum = (Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    if ($sum) { return [long]$sum } else { return 0 }
}

function Format-Size([long]$Bytes) { return '{0:N1} MiB' -f ($Bytes / 1MB) }

function Remove-Safely([string]$Path, [string]$Reason) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to delete outside the project: $full" }
    for ($cursor = $full; $cursor -and $cursor.Length -gt $repoRoot.Length; $cursor = Split-Path -Parent $cursor) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to follow a link: $cursor"
        }
    }
    Write-Output ("Remove-Item -LiteralPath '{0}' -Recurse -Force   # {1}, {2}" -f $full, $Reason, (Format-Size (Get-Size $full)))
    Remove-Item -LiteralPath $full -Recurse -Force
}

$build = Join-Path $repoRoot 'build'
$dist = Join-Path $repoRoot 'dist'
$keepInBuild = @('tools', 'vendor', 'installed-backups', 'downloads', 'auditions')
$excluded = @('.git', '.git-docs', 'dist')
$project = 0
foreach ($item in Get-ChildItem -LiteralPath $repoRoot -Force) {
    if ($excluded -notcontains $item.Name) { $project += Get-Size $item.FullName }
}
Write-Output ("Project (without dist and git folders): {0} of {1} limit" -f (Format-Size $project), (Format-Size $projectLimit))
if (Test-Path -LiteralPath $build) {
    foreach ($folder in Get-ChildItem -LiteralPath $build -Directory -Force) {
        Write-Output ("  build\{0}: {1}" -f $folder.Name, (Format-Size (Get-Size $folder.FullName)))
    }
}
$caches = @(Get-ChildItem -LiteralPath $repoRoot -Recurse -Force -Directory -ErrorAction SilentlyContinue |
    Where-Object { @('__pycache__', '.pytest_cache', '.ruff_cache') -contains $_.Name -and $_.FullName -notmatch '\\build\\vendor\\' })
if (Test-Path -LiteralPath (Join-Path $build 'tmp')) { $caches += Get-Item -LiteralPath (Join-Path $build 'tmp') }
$cacheSize = 0
foreach ($cache in $caches) { $cacheSize += Get-Size $cache.FullName }
Write-Output ("In-project caches: {0} of {1} limit" -f (Format-Size $cacheSize), (Format-Size $cacheLimit))
$releases = @()
if (Test-Path -LiteralPath $dist) {
    $releases = @(Get-ChildItem -LiteralPath $dist -Directory -Filter 'v*' | Where-Object { $_.Name -match '^v\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name.Substring(1) })
    Write-Output ("Local releases: {0}" -f (($releases | ForEach-Object { $_.Name }) -join ', '))
}
$pipCache = ''
$pipSize = 0
try { $pipCache = (& python -m pip cache dir 2>$null | Select-Object -Last 1) } catch { $pipCache = '' }
if ($pipCache -and (Test-Path -LiteralPath $pipCache)) {
    $pipSize = Get-Size $pipCache
    Write-Output ("pip cache: {0} of {1} limit ({2})" -f (Format-Size $pipSize), (Format-Size $pipLimit), $pipCache)
}

if (-not $Apply) { Write-Output 'Report only. Run with -Apply to clean by these rules.'; exit 0 }

# Generated renders and logs are always regenerable.
foreach ($name in @('renders', 'pluginval', 'self-test')) {
    $path = Join-Path $build $name
    if (Test-Path -LiteralPath $path) { Remove-Safely $path 'regenerable renders and logs' }
}
$auditionPath = Join-Path $build 'auditions'
if ($Auditions -and (Test-Path -LiteralPath $auditionPath)) { Remove-Safely $auditionPath 'answered listening round' }
elseif (Test-Path -LiteralPath $auditionPath) { Write-Output 'Keeping build\auditions until the owner answers (use -Auditions after).' }
if ($cacheSize -gt $cacheLimit) { foreach ($cache in $caches) { Remove-Safely $cache.FullName 'in-project cache over limit' } }
# Keep only the newest local release.
if ($releases.Count -gt 1) {
    foreach ($old in $releases[0..($releases.Count - 2)]) { Remove-Safely $old.FullName 'superseded local release' }
}
# Over the limit: delete build trees except the most recently used one.
$project = 0
foreach ($item in Get-ChildItem -LiteralPath $repoRoot -Force) {
    if ($excluded -notcontains $item.Name) { $project += Get-Size $item.FullName }
}
if ($project -gt $projectLimit -and (Test-Path -LiteralPath $build)) {
    $trees = @(Get-ChildItem -LiteralPath $build -Directory -Force | Where-Object { $keepInBuild -notcontains $_.Name } |
        Sort-Object LastWriteTime -Descending)
    if ($trees.Count -gt 1) { foreach ($tree in $trees[1..($trees.Count - 1)]) { Remove-Safely $tree.FullName 'stale build tree, project over limit' } }
}
if ($pipSize -gt $pipLimit) {
    Write-Output 'python -m pip cache purge   # pip cache over limit'
    & python -m pip cache purge
}
Write-Output 'Storage maintenance done.'
