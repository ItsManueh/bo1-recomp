# Profile-guided optimization (PGO) of the ReXGlue SDK's GPU plugin (rexgpu-xenos: the emulated
# GPU's command processor, shared memory, caches), the busiest code of the port. Builds an
# instrumented copy of the SDK, plays a training session with it (a split screen match, a campaign
# mission and Zombies, with the game's own executables), merges the profiles and rebuilds the SDK
# with them, then installs it like tools\build_sdk.ps1.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_sdk_pgo.ps1 [-ToolsDir <folder>]
#        [-UseExisting] [-Off]
#   -UseExisting  skip the instrumented build and the training, reuse pgo\sdk.profdata
#   -Off          rebuild the SDK without profile data
# The profile only fits the source it was recorded with: after changing the SDK, run it again
# (functions changed since are just optimized without profile).
param(
    [string]$ToolsDir = (Join-Path $env:USERPROFILE 'Tools'),
    [switch]$UseExisting,
    [switch]$Off
)
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = Split-Path $PSScriptRoot -Parent
$sdkSource = Join-Path $ToolsDir 'rexglue-src'
$sdkBuild = Join-Path $ToolsDir 'rexglue-build'
$genBuild = Join-Path $ToolsDir 'rexglue-build-pgogen'
$pgoDir = Join-Path $root 'pgo'
$rawDir = Join-Path $pgoDir 'raw'
$profdata = Join-Path $pgoDir 'sdk.profdata'
$gameDir = Join-Path $root 'out\build\local-relwithdebinfo'
$logDir = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logDir, $pgoDir | Out-Null

function Invoke-Logged([string]$what, [string]$log, [string[]]$command) {
    # Windows PowerShell turns every stderr line of a native program (CMake warnings) into an
    # error under 'Stop': the exit code decides instead.
    $ErrorActionPreference = 'Continue'
    & $command[0] $command[1..($command.Count - 1)] *> (Join-Path $logDir $log)
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($exitCode -ne 0) {
        Select-String -Path (Join-Path $logDir $log) -Pattern 'error|FAILED' | Select-Object -First 10
        throw "$what failed (see logs\$log)"
    }
}

if (-not $Off -and -not $UseExisting) {
    # 1. Instrumented SDK, in a build tree (and output folder) of its own.
    Invoke-Logged 'Instrumented SDK configure' 'pgo_configure.txt' @(
        'cmake', '-S', $sdkSource, '-B', $genBuild, '-G', 'Ninja Multi-Config',
        '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
        '-DCMAKE_C_FLAGS=-march=x86-64-v3', '-DCMAKE_CXX_FLAGS=-march=x86-64-v3',
        '-DCMAKE_SHARED_LINKER_FLAGS=', '-DCMAKE_EXE_LINKER_FLAGS=',
        '-DREXGLUE_USE_VULKAN=OFF', '-DREXGLUE_ENABLE_FIDELITYFX=ON', '-DREXGLUE_BUILD_TESTS=OFF',
        '-DREXGLUE_PGO_GENERATE=ON', "-DREXGLUE_OUTPUT_DIR=$($genBuild -replace '\\', '/')/out")
    Invoke-Logged 'Instrumented SDK build' 'pgo_build.txt' @('cmake', '--build', $genBuild, '--config', 'RelWithDebInfo')

    # 2. Training session with the instrumented DLLs next to the game (the normal ones are put back
    #    afterwards).
    if (Test-Path $rawDir) { Remove-Item -Recurse -Force $rawDir }
    New-Item -ItemType Directory -Force $rawDir | Out-Null
    $backup = Join-Path $pgoDir 'dll_backup'
    if (Test-Path $backup) { Remove-Item -Recurse -Force $backup }
    New-Item -ItemType Directory -Force $backup | Out-Null
    Copy-Item (Join-Path $gameDir '*.dll') $backup
    try {
        Get-ChildItem (Join-Path $genBuild 'out\RelWithDebInfo') -Filter '*.dll' |
            Where-Object { Test-Path (Join-Path $gameDir $_.Name) } |
            ForEach-Object { Copy-Item $_.FullName $gameDir -Force }
        # One file per process and module.
        $env:LLVM_PROFILE_FILE = Join-Path $rawDir '%p-%m.profraw'
        & (Join-Path $PSScriptRoot 'split_test.ps1') -Name 'pgo_split' -Seconds 200 -Shots '190' | Out-Null
        & (Join-Path $PSScriptRoot 'run.ps1') -Name 'pgo_campaign' -Seconds 150 -Exec 'press 1 start;wait 10;map cuba' -ExecDelay 20 | Out-Null
        & (Join-Path $PSScriptRoot 'run.ps1') -Name 'pgo_zombies' -Seconds 120 -Exec 'press 1 start;wait 10;map zombie_theater' -ExecDelay 20 | Out-Null
    } finally {
        Remove-Item Env:LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
        Copy-Item (Join-Path $backup '*.dll') $gameDir -Force
    }
    $raw = @(Get-ChildItem $rawDir -Filter '*.profraw')
    if ($raw.Count -eq 0) { throw 'The training session wrote no profiles' }
    Invoke-Logged 'Profile merge' 'pgo_merge.txt' (@('llvm-profdata', 'merge', '-o', $profdata) + ($raw | ForEach-Object { $_.FullName }))
    Write-Output "Merged $($raw.Count) profiles into $profdata"
}

# 3. The SDK with (or without) the profile.
$use = ''
if (-not $Off) {
    if (-not (Test-Path $profdata)) { throw "No profile at $profdata" }
    $use = $profdata -replace '\\', '/'
}
Invoke-Logged 'SDK configure' 'pgo_configure_use.txt' @('cmake', '-S', $sdkSource, '-B', $sdkBuild, "-DREXGLUE_PGO_USE=$use")
& (Join-Path $PSScriptRoot 'build_sdk.ps1') -ToolsDir $ToolsDir
