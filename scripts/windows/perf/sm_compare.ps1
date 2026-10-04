param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$App = "$PSScriptRoot\..\..\..\build\windows\BlueWake-dev",
    [string]$Reference = "$PSScriptRoot\..\..\..\build\windows\test-cap-sm-rel-dump",
    [hashtable]$Env = @{}
)
# Smooth Motion at 60, paced: dump game frames 880-910 (real and in-between,
# Link running and turning) and compare every image with the release's.
$scratch = Split-Path -Parent $MyInvocation.MyCommand.Path
$dump = "$PSScriptRoot\..\..\..\build\windows\test-$Tag-dump"
New-Item -ItemType Directory -Force $dump | Out-Null
Get-ChildItem $dump -File | Remove-Item -Force
$envs = @{ DOL_AURORA_FRAME_INTERP_DUMP = $dump; DOL_AURORA_FRAME_INTERP_DUMP_FROM = '880'; DOL_AURORA_FRAME_INTERP_DUMP_TO = '910' }
foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
& "$scratch\capture_run.ps1" -Tag $Tag -App $App -Settings @('smooth_motion=1', 'smooth_motion_fps=60', 'window=960x720', 'mouse_camera=0') `
    -First 5000 -MaxRetraces 2000 -Paced -Env $envs | Select-Object -First 1
python "$scratch\dump_compare.py" $Reference $dump
