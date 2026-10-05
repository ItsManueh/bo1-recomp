# Starts bo1.exe for a test, keeps its log and (optionally) takes screenshots.
# Usage: powershell -ExecutionPolicy Bypass -File tools\run.ps1 [-Seconds 120] [-Level debug]
#        [-Name run2] [-Shots "20,45,80"] [-Exec "map zombie_theater"] [-ExecDelay 20]
#   -Seconds 0    leaves the game running until you close it.
#   -Level        forces the log level (bo1.toml rules if omitted).
#   -Extra        extra options for bo1.exe separated by spaces, e.g. -Extra "--vsync=true"
#   -Exec         developer console lines run once after -ExecDelay seconds, separated by ';'
#                 (port commands like "dvar x" or "console engine", or engine commands)
#   -PrintWindow  screenshots of the whole window (with the developer UI) instead of the game image
#   -RealProfile  use the player's real profile and saves (Saved Games) instead of the separate
#                 test profile in <project>	est_profile (tests that start a campaign overwrite
#                 the save and every start rotates the backups)
param(
    [int]$Seconds = 120,
    [string]$Level = '',
    [string]$Name = ('run_' + (Get-Date -Format 'yyyyMMdd_HHmmss')),
    [string]$Build = 'local-relwithdebinfo',
    [string]$Shots = '',
    [string]$Extra = '',
    [string]$Exe = 'bo1.exe',  # bo1mp.exe for multiplayer
    [string]$Exec = '',
    [int]$ExecDelay = 15,
    [switch]$PrintWindow,
    [switch]$RealProfile
)

$root = Split-Path $PSScriptRoot -Parent
$exeDir = Join-Path $root "out\build\$Build"
$log = Join-Path $root "logs\$Name.log"
$runLogDir = Join-Path $exeDir 'logs'
New-Item -ItemType Directory -Force (Join-Path $root 'logs') | Out-Null

# No --log_file: the runtime writes its logs to <exe>\logs and the newest one is copied at the end.
$argList = @()
if ($Level) { $argList += "--log_level=$Level" }
# -File passes everything as a single string: split on spaces here.
$argList += @($Extra -split '\s+' | Where-Object { $_ -ne '' })
# Test windows get no controller/keyboard/mouse input: they do not interfere with whoever is using
# the PC and every test sees the same screens.
$argList += '--bo1_test_ignore_input=true'
# Separate profile and saves for tests; the shader cache is shared so performance is measured with
# the same warm pipeline cache the player has.
if (-not $RealProfile) {
    $testProfile = Join-Path $root 'test_profile'
    New-Item -ItemType Directory -Force $testProfile | Out-Null
    $sharedCache = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Saved Games\Call of Duty Black Ops (recompiled)\cache'
    $argList += "`"--user_data_root=$testProfile`""
    if (Test-Path $sharedCache) { $argList += "`"--cache_root=$sharedCache`"" }
}
# Start-Process joins the arguments with spaces: the ones with spaces go in quotes.
if ($Exec) { $argList += "`"--bo1_test_exec=$Exec`""; $argList += "--bo1_test_exec_delay=$ExecDelay" }
# Screenshots: by default the game saves them itself (internal image, any graphics backend).
$shotSpec = (@($Shots -split '[,; ]+' | Where-Object { $_ -ne '' }) -join ',')
if ($shotSpec -and -not $PrintWindow) {
    $argList += "--bo1_capture_at=$shotSpec"
    $argList += "--bo1_capture_prefix=$Name"
}
$launchTime = Get-Date
$startArgs = @{ FilePath = (Join-Path $exeDir $Exe); WorkingDirectory = $exeDir; PassThru = $true }
if ($argList.Count -gt 0) { $startArgs.ArgumentList = $argList }
$p = Start-Process @startArgs

Add-Type -AssemblyName System.Drawing
if (-not ('Bo1Win' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Bo1Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
}
[Bo1Win]::SetProcessDPIAware() | Out-Null

# Captures ONLY the content of the game window (PrintWindow), even if it is covered or in the
# background. It never copies the desktop or other windows, and never changes the focus.
function Save-Shot([int]$t) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Output "  (no window for the $t s screenshot)"; return }
    $r = New-Object Bo1Win+RECT
    [Bo1Win]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    if ($w -le 0 -or $hgt -le 0) { return }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    # 1 = PW_CLIENTONLY, 2 = PW_RENDERFULLCONTENT (needed for DirectX windows)
    $ok = [Bo1Win]::PrintWindow($h, $hdc, 3)
    $g.ReleaseHdc($hdc)
    $file = Join-Path $root "logs\${Name}_${t}s.png"
    if ($ok) {
        $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Output "  screenshot: $file"
    } else {
        Write-Output "  (PrintWindow failed for the $t s screenshot)"
    }
    $g.Dispose(); $bmp.Dispose()
}

$shotList = @($Shots -split '[,; ]+' | Where-Object { $_ -ne '' } | ForEach-Object { [int]$_ } | Sort-Object)
if ($PrintWindow) {
    foreach ($t in $shotList) {
        $wait = $t - ((Get-Date) - $launchTime).TotalSeconds
        if ($wait -gt 0 -and $p.WaitForExit([int]($wait * 1000))) { break }
        if ($p.HasExited) { break }
        Save-Shot $t
    }
}

if ($Seconds -gt 0) {
    $left = $Seconds - ((Get-Date) - $launchTime).TotalSeconds
    if ($left -lt 0) { $left = 0 }
    if (-not $p.WaitForExit([int]($left * 1000))) {
        Write-Output "Still running after $Seconds s; closing it."
        Stop-Process -Id $p.Id -Force
    }
}
$p.WaitForExit() | Out-Null
if ($p.ExitCode -ne $null -and $p.ExitCode -ne -1) {
    Write-Output ("Exit code: 0x{0:X8}" -f $p.ExitCode)
}

if ($shotSpec -and -not $PrintWindow) {
    Get-ChildItem $runLogDir -Filter "${Name}_*s.png" -ErrorAction SilentlyContinue | ForEach-Object {
        Move-Item $_.FullName (Join-Path $root "logs\$($_.Name)") -Force
        Write-Output "  screenshot: $(Join-Path $root "logs\$($_.Name)")"
    }
}

$runLog = Get-ChildItem $runLogDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $launchTime } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($runLog) {
    Copy-Item $runLog.FullName $log -Force
    Write-Output "Log: $log"
} else {
    Write-Output "No new log found in $runLogDir"
}
