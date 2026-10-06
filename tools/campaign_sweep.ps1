# Loads every campaign mission in turn and keeps a log and screenshots of each (test profile).
# Usage: powershell -ExecutionPolicy Bypass -File tools\campaign_sweep.ps1 [-Maps "cuba,vorkuta"]
#        [-Seconds 160] [-Shots "90,120,150"]
# Then: python tools\campaign_report.py  (summary of errors, frame rate and hitches per mission)
param(
    [string]$Maps = 'int_escape,cuba,vorkuta,pentagon,flashpoint,khe_sanh,hue_city,kowloon,fullahead,creek_1,river,wmd_sr71,wmd,pow,rebirth,underwaterbase,terminal,outro',
    [int]$Seconds = 160,
    [string]$Shots = '90,120,150'
)
$root = Split-Path $PSScriptRoot -Parent
foreach ($map in ($Maps -split ',')) {
    $map = $map.Trim()
    if (-not $map) { continue }
    Write-Output "== $map"
    # START on the title screen picks the profile and the storage device, like a player would.
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Seconds $Seconds `
        -Name "sweep_$map" -Exec "press 1 start;wait 10;map $map" -ExecDelay 20 -Shots $Shots |
        Select-Object -Last 1
}
