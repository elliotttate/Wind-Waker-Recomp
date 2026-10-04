param([string[]]$Apps = @('W-final', 'W-fuse'), [string]$Tag = 'sd',
      [string[]]$States = @('gohma-lava', 'drc-lava-room', 'drc-lava-bridge'))
# Booting into each save state: Smooth Motion dumps of game frames 300-304 for each app, compared.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = "$PSScriptRoot\..\..\..\build\windows"
foreach ($state in $States) {
    foreach ($app in $Apps) {
        $dump = "$root\test-$Tag-$state-$app-dump"
        Remove-Item -Recurse -Force $dump -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $dump | Out-Null
        $null = & "$s\state_run.ps1" -Tag "$Tag-$state-$app" -App "$root\$app" -State "$root\test-saves\$state.bwstate" -First 99999 -MaxRetraces 2300 `
            -Settings @('smooth_motion=1', 'smooth_motion_fps=60', 'window=960x720', 'mouse_camera=0') `
            -Env @{ DOL_AURORA_FRAME_INTERP_DUMP = $dump; DOL_AURORA_FRAME_INTERP_DUMP_FROM = '300'; DOL_AURORA_FRAME_INTERP_DUMP_TO = '304' }
    }
    "== $state"
    & "python" "$s\dump_maxdiff.py" "$root\test-$Tag-$state-$($Apps[0])-dump" "$root\test-$Tag-$state-$($Apps[1])-dump"
}
