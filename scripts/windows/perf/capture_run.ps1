param(
    [string]$Tag = 'cap',
    [string]$App = "$PSScriptRoot\..\..\..\build\windows\BlueWake-dev",
    [int]$First = 800,
    [int]$Interval = 100,
    [int]$MaxRetraces = 2700,
    [string[]]$Settings = @('smooth_motion=0'),
    [hashtable]$Env = @{},
    [switch]$Paced
)
# bench.ps1's route (load the Outset save, stand, run and turn from 1500),
# unpaced by default, capturing the frame every $Interval retraces from $First.
$saves = "$PSScriptRoot\..\..\..\build\windows\test-saves"
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$data\frames" | Out-Null
if ($Settings.Count -gt 0) { Set-Content -Path "$data\settings.ini" -Value $Settings -Encoding ascii }
foreach ($name in @(Get-ChildItem env: | Where-Object { $_.Name -like 'BLUEWAKE_*' -or $_.Name -like 'DOL_*' -or $_.Name -like 'AURORA_*' } | ForEach-Object Name)) {
    Remove-Item "env:$name"
}
Copy-Item "$saves\outset-start.card" "$data\GZLE01.card"
$env:BLUEWAKE_DATA_DIR = $data
$env:BLUEWAKE_NO_DIALOG = '1'
$env:BLUEWAKE_MAX_RETRACES = "$MaxRetraces"
$env:BLUEWAKE_PLAYER_PROBE = '1'
$env:BLUEWAKE_PAD_BUTTONS = '0x0100'
$env:BLUEWAKE_PAD_PULSE_ON_TITLE_READY = '1'
$env:BLUEWAKE_PAD_PULSE_LENGTH = '2'
$env:BLUEWAKE_PAD_CONFIRM_EVENT = 'any'
$presses = (340..760 | Where-Object { ($_ - 340) % 60 -eq 0 }) | ForEach-Object { "${_}:0x0100:2" }
$moves = @(
    '1500:0:300:0:127', '1800:0:120:90:90', '1920:0:300:0:127', '2220:0:120:-90:90',
    '2340:0:250:0:127', '2590:0:150:100:60', '2740:0:160:0:127'
)
$env:BLUEWAKE_PAD_SCRIPT = (@($presses) + $moves) -join ','
$env:BLUEWAKE_CAPTURE_OPENING_FRAME = "$data\frames\frame.ppm"
$env:BLUEWAKE_CAPTURE_RETRACE = "$First"
$env:BLUEWAKE_CAPTURE_INTERVAL = "$Interval"
if (-not $Paced) { $env:BLUEWAKE_WALL_PACE = '0'; $env:DOL_AUDIO_NO_THROTTLE = '1' }
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
$p = Start-Process -FilePath "$App\BlueWake.exe" -PassThru -Wait `
    -RedirectStandardError "$data\stderr.txt" -RedirectStandardOutput "$data\stdout.txt"
"$Tag exit=$($p.ExitCode)"
Select-String -Path "$data\stderr.txt" -Pattern '\[frame-capture\] sample=\d+ retrace=(\d+) .*hash=(0x[0-9A-F]+)' |
    ForEach-Object { "{0} {1}" -f $_.Matches[0].Groups[1].Value, $_.Matches[0].Groups[2].Value }
