param([string[]]$Apps = @('W-final', 'W-fuse'), [string]$Tag = 'pd',
      [string[]]$Places = @('sea:1:100', 'sea:41:0', 'sea:13:0', 'Hyrule:0:0', 'Hyroom:0:0', 'M_DaiB:0:0', 'kindan:0:0', 'Siren:0:0'),
      [hashtable]$Env = @{})
# Per place (warp at retrace 1200, Link running from 1600): Smooth Motion dumps of game frames 900-904 for each
# app, then real frames compared (must be identical) and in-between frames' largest difference.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = "$PSScriptRoot\..\..\..\build\windows"
$set = @('smooth_motion=1', 'smooth_motion_fps=60', 'window=960x720', 'mouse_camera=0')
foreach ($place in $Places) {
    $name = $place -replace ':', '-'
    foreach ($app in $Apps) {
        $dump = "$root\test-$Tag-$name-$app-dump"
        Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $dump | Out-Null
        $envs = @{ DOL_AURORA_FRAME_INTERP_DUMP = $dump; DOL_AURORA_FRAME_INTERP_DUMP_FROM = '900'; DOL_AURORA_FRAME_INTERP_DUMP_TO = '904' }
        foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
        $null = & "$s\tour_run.ps1" -Tag "$Tag-$name-$app" -App "$root\$app" -Places @($place) -Stop 700 -Settings $set -MovesKind survey -Env $envs
    }
    $a = "$root\test-$Tag-$name-$($Apps[0])-dump"; $b = "$root\test-$Tag-$name-$($Apps[1])-dump"
    "== $place"
    & "python" "$s\dump_maxdiff.py" $a $b
}
