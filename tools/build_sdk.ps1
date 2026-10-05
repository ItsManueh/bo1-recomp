# Rebuilds the modified ReXGlue SDK (the bo1 branch of the rexglue-sdk fork, D3D12 only), installs
# it from scratch and copies its DLLs next to bo1.exe/bo1mp.exe.
# (The game build only copies the DLLs when it relinks the .exe.)
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_sdk.ps1 [-ToolsDir <folder>]
#   -ToolsDir  folder with the SDK build tree (rexglue-build), the install folder
#              (rexglue-sdk-custom) and the official SDK release (rexglue-sdk) whose rexglue.exe
#              recompiler is reused. Default: %USERPROFILE%\Tools
param(
    [string]$ToolsDir = (Join-Path $env:USERPROFILE 'Tools')
)
$ErrorActionPreference = 'Stop'
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$root = Split-Path $PSScriptRoot -Parent
$sdkBuild = Join-Path $ToolsDir 'rexglue-build'
$sdkInstall = Join-Path $ToolsDir 'rexglue-sdk-custom\win-amd64'
$officialSdk = Join-Path $ToolsDir 'rexglue-sdk\win-amd64'
$logDir = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null

cmd /c "cmake --build `"$sdkBuild`" --config RelWithDebInfo > `"$logDir\sdk_build.txt`" 2>&1"
if ($LASTEXITCODE -ne 0) {
    Select-String -Path "$logDir\sdk_build.txt" -Pattern 'error:|FAILED' | Select-Object -First 15
    throw "SDK build failed (see logs\sdk_build.txt)"
}
if (Test-Path $sdkInstall) { Remove-Item -Recurse -Force $sdkInstall }
cmd /c "cmake --install `"$sdkBuild`" --config RelWithDebInfo > `"$logDir\sdk_install.txt`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "SDK install failed (see logs\sdk_install.txt)" }

# The recompiler (rexglue.exe) is only installed in Release: the official one of the same version is used.
Copy-Item "$officialSdk\bin\rexglue.exe" "$sdkInstall\bin\" -Force
@'
# Only the recompiler (official rexglue.exe v0.10.0); the rest of the custom SDK is RelWithDebInfo.
set(CMAKE_IMPORT_FILE_VERSION 1)
set_property(TARGET rex::rexglue APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/rexglue.exe")
list(APPEND _cmake_import_check_targets rex::rexglue)
list(APPEND _cmake_import_check_files_for_rex::rexglue "${_IMPORT_PREFIX}/bin/rexglue.exe")
set(CMAKE_IMPORT_FILE_VERSION)
'@ | Set-Content -Encoding ascii "$sdkInstall\lib\cmake\rexglue\rexglueTargets-release.cmake"

$gameDir = Join-Path $root 'out\build\local-relwithdebinfo'
if (Test-Path $gameDir) {
    Get-ChildItem $gameDir -Filter '*.dll' | Where-Object { -not (Test-Path (Join-Path "$sdkInstall\bin" $_.Name)) } |
        ForEach-Object { Remove-Item $_.FullName -Force; Write-Output "  removed obsolete DLL: $($_.Name)" }
    Copy-Item "$sdkInstall\bin\*.dll" $gameDir -Force
    Write-Output "SDK built, installed and copied to $gameDir"
} else {
    Write-Output "SDK built and installed (no game folder yet)"
}
