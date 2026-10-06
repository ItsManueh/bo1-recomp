# Interleaved A/B split screen runs, separated by ';', each "name=extra options" (options may be
# empty). Prints the frame rate of the last minute of every run.
# Usage: powershell -ExecutionPolicy Bypass -File tools\split_ab.ps1 -Runs "a1=--x=true;b1=--x=false"
param([string]$Runs)
$root = Split-Path $PSScriptRoot -Parent
foreach ($run in ($Runs -split ';' | Where-Object { $_.Trim() })) {
    $name, $extra = $run -split '=', 2
    & (Join-Path $PSScriptRoot 'split_test.ps1') -Name $name -Seconds 200 -Shots '190' -Extra $extra | Out-Null
    $log = Join-Path $root "logs\$name.log"
    $fps = Select-String -Path $log -Pattern 'average FPS over the last 10 s = ([\d.]+)' |
        Select-Object -Last 6 | ForEach-Object { $_.Matches[0].Groups[1].Value }
    $views = (Select-String -Path $log -Pattern 'split screen with 2 views' -Quiet)
    $errors = @(Select-String -Path $log -Pattern '\[error\]|engine error').Count
    "{0,-14} fps {1} | split {2} | errors {3}" -f $name, ($fps -join ' '), $views, $errors
}
