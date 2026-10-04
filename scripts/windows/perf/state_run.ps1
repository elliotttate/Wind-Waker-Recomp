param(
    [string]$Tag = 'st-lava',
    [string]$App = "$PSScriptRoot\..\..\..\build\windows\BlueWake-lava",
    [string]$State = "$PSScriptRoot\..\..\..\build\windows\test-saves\gohma-lava.bwstate",
    [int]$First = 1340,
    [int]$Interval = 60,
    [int]$MaxRetraces = 1700,
    [string[]]$Moves = @(),
    [string[]]$Settings = @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0'),
    [hashtable]$Env = @{},
    [string[]]$CopyIn = @(),
    [long]$Mask = 0,
    [switch]$Unpaced
)
# Boots straight into a save state (gohma-lava: Gohma's room, Link by the lava, retrace 1300)
# and captures frames; $Moves are pad-script entries (retrace:buttons:length[:x:y]).
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$data\frames" | Out-Null
Set-Content -Path "$data\settings.ini" -Value $Settings -Encoding ascii
foreach ($name in @(Get-ChildItem env: | Where-Object { $_.Name -like 'BLUEWAKE_*' -or $_.Name -like 'DOL_*' -or $_.Name -like 'AURORA_*' } | ForEach-Object Name)) {
    Remove-Item "env:$name"
}
Copy-Item "$PSScriptRoot\..\..\..\build\windows\test-saves\outset-start.card" "$data\GZLE01.card"
foreach ($f in $CopyIn) { Copy-Item $f $data }
$env:BLUEWAKE_DATA_DIR = $data
$env:BLUEWAKE_NO_DIALOG = '1'
$env:BLUEWAKE_MAX_RETRACES = "$MaxRetraces"
$env:BLUEWAKE_LOAD_STATE = $State
if ($Moves.Count -gt 0) { $env:BLUEWAKE_PAD_SCRIPT = $Moves -join ',' }
$env:BLUEWAKE_CAPTURE_OPENING_FRAME = "$data\frames\frame.ppm"
$env:BLUEWAKE_CAPTURE_RETRACE = "$First"
$env:BLUEWAKE_CAPTURE_INTERVAL = "$Interval"
if ($Unpaced) { $env:BLUEWAKE_WALL_PACE = '0'; $env:DOL_AUDIO_NO_THROTTLE = '1' }
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
$p = Start-Process -FilePath "$App\BlueWake.exe" -PassThru `
    -RedirectStandardError "$data\stderr.txt" -RedirectStandardOutput "$data\stdout.txt"
if ($Mask -ne 0) { $p.ProcessorAffinity = [IntPtr]$Mask }
$p.WaitForExit()
"$Tag exit=$($p.ExitCode)"
Select-String -Path "$data\stderr.txt" -Pattern '\[state\]' | ForEach-Object { $_.Line }
