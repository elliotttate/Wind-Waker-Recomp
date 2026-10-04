param([string[]]$Apps, [int]$Reps = 2, [string[]]$Places = @('sea:41:0', 'sea:13:0', 'Hyroom:0:0'), [long]$Mask = 0,
      [string]$Tag = 'aa', [string[]]$Settings = @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0'), [hashtable]$Env = @{})
# Apps A/B uncapped on $Mask: game FPS, game thread and GX worker CPU per game frame, drain per frame.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = "$PSScriptRoot\..\..\..\build\windows"
foreach ($rep in 1..$Reps) {
    foreach ($app in $Apps) {
        $runTag = "$Tag-$app-$rep"
        $envs = @{ BLUEWAKE_PLAYER_PROBE = '1' }
        foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
        $null = & "$s\tour_run.ps1" -Tag $runTag -App "$root\$app" -Places $Places -Stop 1200 -Settings $Settings -Env $envs -Unpaced -Mask $Mask -MovesKind survey
        "== $app rep $rep"
        python "$s\survey_drain.py" 1200 "$root\test-$runTag\stderr.txt" ($Places -join ',')
    }
}
