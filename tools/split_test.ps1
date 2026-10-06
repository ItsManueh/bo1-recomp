# Starts a two player split screen match in multiplayer (Array, test profile) and keeps its log and
# screenshots of the window: the menu path a player would take, driven with `press` commands.
# Usage: powershell -ExecutionPolicy Bypass -File tools\split_test.ps1 -Name t_split [-Seconds 200]
#        [-Shots "135,160,185"] [-Extra "--bo1_split_screen_view=full"] [-Game]
#   -Game  screenshots of the game image instead of the whole window
param(
    [string]$Name = 'split_test',
    [int]$Seconds = 200,
    [string]$Shots = '135,160,185',
    [string]$Extra = '',
    [switch]$Game
)
$sequence = 'press 1 start;wait 8;press 1 a;wait 5;press 1 down;wait 5;press 1 a;wait 6;' +
    'press 1 a;wait 7;press 2 a;wait 5;press 1 a;wait 30;press 1 a;wait 3;press 2 a;wait 3;' +
    'press 1 a;wait 3;press 2 a;wait 3;press 1 a;wait 3;press 2 a'
# Two virtual controllers: player 2 always has one, with or without a real controller connected.
$runArgs = @{ Exe = 'bo1mp.exe'; Seconds = $Seconds; Name = $Name; Exec = $sequence;
              ExecDelay = 25; Shots = $Shots; Extra = ("--bo1_test_pads=2 " + $Extra).Trim() }
if (-not $Game) { $runArgs.PrintWindow = $true }
& (Join-Path $PSScriptRoot 'run.ps1') @runArgs
