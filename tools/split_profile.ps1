# Split screen test with a profile of the emulated GPU thread: starts tools\split_test.ps1 and, once
# both players are in the match, samples the "GPU Commands" thread with tools\sampler.
# Usage: powershell -ExecutionPolicy Bypass -File tools\split_profile.ps1 -Name t_prof [-At 150]
#        [-Seconds 15] [-Extra "..."] [-Threads "GPU Commands"]
#   -Threads  thread names for the sampler, separated by commas ("-" = every thread above 5%)
param(
    [string]$Name = 'split_profile',
    [int]$At = 150,
    [int]$Seconds = 15,
    [string]$Extra = '',
    [string]$Threads = 'GPU Commands'
)
$root = Split-Path $PSScriptRoot -Parent
$job = Start-Job -ScriptBlock {
    param($script, $name, $extra)
    & $script -Name $name -Seconds 200 -Shots '190' -Extra $extra
} -ArgumentList (Join-Path $PSScriptRoot 'split_test.ps1'), $Name, $Extra
Start-Sleep -Seconds $At
$game = Get-Process bo1mp -ErrorAction SilentlyContinue | Select-Object -First 1
if ($game) {
    & (Join-Path $PSScriptRoot 'sampler\sampler.exe') $game.Id $Seconds 1 5 $Threads `
        (Join-Path $root "logs\$Name.samples.tsv") > (Join-Path $root "logs\${Name}_sampler.txt")
} else {
    Write-Output 'bo1mp.exe is not running'
}
Receive-Job $job -Wait | Select-Object -Last 1
