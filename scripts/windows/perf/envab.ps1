param([Parameter(Mandatory)] [string]$App, [Parameter(Mandatory)] [string]$Var, [string[]]$Values, [int]$Reps = 2,
      [string[]]$Places = @('sea:41:0', 'sea:13:0', 'Hyroom:0:0'), [long]$Mask = 0x000F0000, [string]$Tag = 'ab',
      [string[]]$Settings = @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0'))
# One exe, an environment variable's values A/B: uncapped on $Mask, game FPS, the game thread's and the GX worker's
# CPU per game frame and the draw-done drain per frame, at each place (Stop 1200, survey moves).
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = "$PSScriptRoot\..\..\..\build\windows"
foreach ($rep in 1..$Reps) {
    foreach ($v in $Values) {
        $runTag = "$Tag-$($v -replace '[^A-Za-z0-9]', '_')-$rep"
        $envs = @{ BLUEWAKE_PLAYER_PROBE = '1' }
        if ($v -ne 'unset') { $envs[$Var] = $v }
        $null = & "$s\tour_run.ps1" -Tag $runTag -App $App -Places $Places -Stop 1200 -Settings $Settings -Env $envs -Unpaced -Mask $Mask -MovesKind survey
        "== $Var=$v rep $rep"
        python "$s\survey_drain.py" 1200 "$root\test-$runTag\stderr.txt" ($Places -join ',')
    }
}
