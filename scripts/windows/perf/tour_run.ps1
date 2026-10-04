param(
    [string]$Tag = 'tour',
    [string]$App = "$PSScriptRoot\..\..\..\build\windows\BlueWake-dev",
    [string[]]$Places = @('sea:11:1', 'sea:13:0', 'M_NewD2:0:0', 'sea:41:0', 'kindan:0:0', 'Siren:0:0', 'majroom:0:0',
                          'sea:1:100', 'Hyrule:0:0', 'sea:44:0'),
    [int]$Start = 1200,
    [int]$Stop = 1500,
    [string[]]$Settings = @('smooth_motion=1', 'smooth_motion_fps=60', 'window=1280x960', 'mouse_camera=0'),
    [hashtable]$Env = @{},
    [switch]$Unpaced,
    [long]$Mask = 0,
    [string]$MovesKind = 'tour'
)
# Loads the Outset save, then tours the game by test warps (the builder's
# training tour): at each stop Link runs, turns and runs on.
$saves = "$PSScriptRoot\..\..\..\build\windows\test-saves"
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $data | Out-Null
Set-Content -Path "$data\settings.ini" -Value $Settings -Encoding ascii
foreach ($name in @(Get-ChildItem env: | Where-Object { $_.Name -like 'BLUEWAKE_*' -or $_.Name -like 'DOL_*' -or $_.Name -like 'AURORA_*' -or $_.Name -like 'LLVM_PROFILE_*' } | ForEach-Object Name)) {
    Remove-Item "env:$name"
}
Copy-Item "$saves\outset-start.card" "$data\GZLE01.card"
$warps = @(); $moves = @()
for ($i = 0; $i -lt $Places.Count; $i++) {
    $at = $Start + $i * $Stop
    $warps += "${at}:$($Places[$i])"
    if ($MovesKind -eq 'long') {
        for ($m = 400; $m -lt $Stop - 300; $m += 600) {
            $moves += @("$($at + $m):0:250:0:127", "$($at + $m + 250):0:150:110:60", "$($at + $m + 400):0:150:-110:60")
        }
    } elseif ($MovesKind -eq 'survey') {
        $moves += @("$($at + 400):0:250:0:127", "$($at + 650):0:200:110:60", "$($at + 850):0:200:0:127", "$($at + 1050):0:150:-110:60")
    } else {
        $moves += @("$($at + 450):0:300:0:127", "$($at + 780):0:300:110:60", "$($at + 1110):0:300:-110:60")
    }
}
$presses = (340..760 | Where-Object { ($_ - 340) % 60 -eq 0 }) | ForEach-Object { "${_}:0x0100:2" }
$env:BLUEWAKE_DATA_DIR = $data
$env:BLUEWAKE_NO_DIALOG = '1'
$env:BLUEWAKE_MAX_RETRACES = "$($Start + $Places.Count * $Stop + 300)"
$env:BLUEWAKE_PAD_BUTTONS = '0x0100'
$env:BLUEWAKE_PAD_PULSE_ON_TITLE_READY = '1'
$env:BLUEWAKE_PAD_PULSE_LENGTH = '2'
$env:BLUEWAKE_PAD_CONFIRM_EVENT = 'any'
$env:BLUEWAKE_PAD_SCRIPT = (@($presses) + $moves) -join ','
$env:BLUEWAKE_TEST_WARP = $warps -join ','
if ($Unpaced) { $env:BLUEWAKE_WALL_PACE = '0'; $env:DOL_AUDIO_NO_THROTTLE = '1' }
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath "$App\BlueWake.exe" -PassThru `
    -RedirectStandardError "$data\stderr.txt" -RedirectStandardOutput "$data\stdout.txt"
if ($Mask -ne 0) { $p.ProcessorAffinity = [IntPtr]$Mask }
$p.WaitForExit()
"$Tag exit=$($p.ExitCode) seconds=$([math]::Round($sw.Elapsed.TotalSeconds))"
Select-String -Path "$data\stderr.txt" -Pattern '\[load\] test warp' | ForEach-Object { "  " + $_.Line }
