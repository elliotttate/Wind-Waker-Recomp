param([int]$Reps = 2, [long]$Mask = 0x00FF0000, [string]$Tag = 'm', [string[]]$Apps = @('W-ef3', 'W-mac'),
      [int]$Fps = 60, [string[]]$Places = @('sea:44:0', 'sea:41:0', 'sea:13:0', 'sea:1:100', 'Hyrule:0:0', 'Hyroom:0:0'),
      [hashtable]$Env = @{})
# The Mac's water and HUD (RecompCore 201e909) against the renderer before it (ef3e17f), the same game module:
# Smooth Motion at $Fps (60 by default), paced, at places with and without the sea, on eight E-cores.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = "$PSScriptRoot\..\..\..\build\windows"
$set = @('smooth_motion=1', "smooth_motion_fps=$Fps", 'window=1280x960', 'mouse_camera=0')
$places = $Places
foreach ($rep in 1..$Reps) {
    foreach ($app in $Apps) {
        $runTag = "$Tag-$app-$rep"
        $null = & "$s\tour_run.ps1" -Tag $runTag -App "$root\$app" -Places $places -Stop 1200 -Settings $set `
            -Env ($Env + @{ BLUEWAKE_PLAYER_PROBE = '1' }) -Mask $Mask -MovesKind survey
        "== $app rep $rep"
        python "$s\survey_sm.py" 1200 "$root\test-$runTag\stderr.txt" ($places -join ',')
    }
}
