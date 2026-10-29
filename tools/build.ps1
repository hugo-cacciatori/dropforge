# Configures and builds Dropforge inside an isolated MSVC developer environment,
# then runs CTest. Nothing is installed and the parent shell's PATH is unchanged.
# Runs on Windows PowerShell 5.1 and PowerShell 7.
#
#   powershell -NoProfile -File tools/build.ps1                 # Release + all tests
#   powershell -NoProfile -File tools/build.ps1 -SkipTests      # build only
#   powershell -NoProfile -File tools/build.ps1 -TestRegex routing
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$JuceSource = '',
    [string]$Target = '',
    [switch]$SkipTests,
    [string]$TestRegex = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'windows-toolchain.json') -Raw | ConvertFrom-Json
$cmakeBin = Join-Path $repoRoot "build\tools\cmake-$($pins.cmake.version)-windows-x86_64\bin"
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'
$developerShell = Join-Path $pins.visualStudio.installPath 'Common7\Tools\VsDevCmd.bat'
foreach ($tool in @($cmake, $ctest, $developerShell)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Missing tool: $tool. Run tools/setup-windows.ps1 first."
    }
}

# Ninja builds on every core; it is used only once tools/setup-windows.ps1
# -WithNinja has fetched the pinned copy. Each generator gets its own tree.
$ninja = Join-Path $repoRoot "build\tools\ninja-$($pins.ninja.version)\ninja.exe"
$useNinja = Test-Path -LiteralPath $ninja -PathType Leaf
$generator = if ($useNinja) { 'Ninja' } else { 'NMake Makefiles' }
$buildRoot = Join-Path $repoRoot ("build\{0}-{1}" -f $Configuration.ToLowerInvariant(), $(if ($useNinja) { 'ninja' } else { 'nmake' }))

# Prefer a local copy of the exact pinned JUCE commit (build/vendor/JUCE-<commit>),
# otherwise CMake downloads it and checks its SHA-256.
$cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
$commit = [regex]::Match($cmakeText, 'juce-framework/JUCE/zip/([0-9a-f]{40})').Groups[1].Value
if (-not $commit) { throw 'Could not read the pinned JUCE commit from CMakeLists.txt.' }
if (-not $JuceSource) {
    $vendored = Join-Path $repoRoot "build\vendor\JUCE-$commit"
    if (Test-Path -LiteralPath (Join-Path $vendored 'CMakeLists.txt')) { $JuceSource = $vendored }
}
$sourceOption = ' -DFETCHCONTENT_SOURCE_DIR_JUCE='
if ($JuceSource) {
    $JuceSource = (Resolve-Path -LiteralPath $JuceSource).Path
    if (-not (Test-Path -LiteralPath (Join-Path $JuceSource 'CMakeLists.txt'))) { throw 'JuceSource must be the JUCE source root.' }
    $sourceOption = ' "-DFETCHCONTENT_SOURCE_DIR_JUCE={0}"' -f $JuceSource
}

# These strings become cmd.exe input: refuse metacharacters instead of letting an
# unusual path be interpreted as another command.
foreach ($value in @($repoRoot, $JuceSource, $developerShell, $pins.visualStudio.toolset, $Target, $TestRegex, $ninja)) {
    if ($value -match '["%&|<>^!\r\n]') { throw "Unsupported cmd.exe character in: $value" }
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$targetOption = if ($Target) { " --target $Target" } else { '' }
$jobs = if ($useNinja) { " --parallel $([Environment]::ProcessorCount)" } else { '' }
# JUCE configures its VST3 manifest helper in a nested CMake run during the
# build, which finds Ninja only through PATH (this child process's PATH only).
$ninjaPath = if ($useNinja) { 'set "PATH={0};%PATH%"' -f (Split-Path -Parent $ninja) } else { 'rem NMake' }
$lines = @(
    '@echo off'
    'setlocal DisableDelayedExpansion'
    ('call "{0}" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver={1}' -f $developerShell, $pins.visualStudio.toolset)
    'if errorlevel 1 exit /b %errorlevel%'
    $ninjaPath
    ('"{0}" -S "{1}" -B "{2}" -G "{3}" -DCMAKE_BUILD_TYPE={4} -DBUILD_TESTING=ON{5}' -f $cmake, $repoRoot, $buildRoot, $generator, $Configuration, $sourceOption)
    'if errorlevel 1 exit /b %errorlevel%'
    ('"{0}" --build "{1}"{2}{3}' -f $cmake, $buildRoot, $targetOption, $jobs)
    'if errorlevel 1 exit /b %errorlevel%'
)
if (-not $SkipTests) {
    $regex = if ($TestRegex) { " -R $TestRegex" } else { '' }
    $lines += ('"{0}" --test-dir "{1}" --output-on-failure --timeout 1500 -j {2}{3}' -f $ctest, $buildRoot, [Environment]::ProcessorCount, $regex)
}
$lines += 'exit /b %errorlevel%'
$batch = Join-Path $buildRoot 'run-build.cmd'
[IO.File]::WriteAllLines($batch, $lines, (New-Object Text.UTF8Encoding $false))

$started = Get-Date
& $env:ComSpec /d /c $batch
$exit = $LASTEXITCODE
Write-Output ("Build directory: {0} ({1}), {2:N0} s" -f $buildRoot, $generator, ((Get-Date) - $started).TotalSeconds)
if ($exit -ne 0) { throw "Configure, build or CTest failed (exit $exit)." }
