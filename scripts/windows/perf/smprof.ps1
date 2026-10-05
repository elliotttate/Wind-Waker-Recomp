param(
    [Parameter(Mandatory)] [string]$Place,
    [Parameter(Mandatory)] [string]$Tag,
    [Parameter(Mandatory)] [string]$App,
    [int]$Seconds = 20,
    [int]$Fps = 60,
    [long]$Mask = 0,
    [hashtable]$Env = @{},
    [string]$Out = "$PSScriptRoot\..\..\..\build\windows\profiles-out"
)
# placeprof.ps1 with Smooth Motion at -Fps (60), paced, on all cores (or -Mask's): one place by warp, Link running and turning;
# from retrace 1800 the busiest host threads' instruction pointers are sampled (thread_sampler_w32.py).
# Writes $Tag-<tid>.txt to $Out (build\windows\profiles-out); symbolize with symprof.py (SYMPROF_APP = the
# exe whose PDB matches).
$scratch = Split-Path -Parent $MyInvocation.MyCommand.Path
$old = $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
$job = Start-Job -ScriptBlock {
    param($s, $tag, $app, $place, $envs, $fps, $mask)
    & "$s\tour_run.ps1" -Tag $tag -App $app -Places @($place) -Stop 3000 -Settings @('smooth_motion=1', "smooth_motion_fps=$fps", 'window=1280x960', 'mouse_camera=0') `
        -Env $envs -MovesKind long -Mask $mask
} -ArgumentList $scratch, $Tag, $App, $Place, $Env, $Fps, $Mask
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
$deadline = (Get-Date).AddSeconds(240)
$proc = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if (-not $proc) { $proc = Get-Process BlueWake -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$((Resolve-Path $App).Path)*" } | Select-Object -First 1 }
    $last = Select-String -Path "$data\stderr.txt" -Pattern '\[perf\] retrace=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($last -and [int]$last.Matches[0].Groups[1].Value -ge 1800) { break }
}
"pid=$($proc.Id)"
python "$old\thread_sampler_w32.py" $proc.Id $Seconds "$Out\$Tag" 4
Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
Receive-Job $job -Wait | Out-Null
Get-ChildItem "$Out\$Tag-*.txt" | ForEach-Object Name
