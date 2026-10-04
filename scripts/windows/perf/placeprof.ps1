param(
    [Parameter(Mandatory)] [string]$Place,
    [string]$Tag = 'pp',
    [string]$App = "$PSScriptRoot\..\..\..\build\windows\F-new",
    [long]$Mask = 0x000F0000,
    [int]$Seconds = 20,
    [hashtable]$Env = @{},
    [switch]$Stacks
)
# One place by warp (at retrace 1200), Link running and turning, uncapped on four E-cores with Smooth Motion
# off; from retrace 1800 the game thread's guest pc is sampled (guest_sampler.py: which game functions) and,
# at the same time, the busiest host threads' instruction pointers (thread_sampler_w32.py: the GX worker,
# the render worker). Writes $Tag-guest.txt and $Tag-<tid>.txt here.
$scratch = Split-Path -Parent $MyInvocation.MyCommand.Path
$old = $PSScriptRoot
$envs = @{ BLUEWAKE_PLAYER_PROBE = '1' }
foreach ($k in $Env.Keys) { $envs[$k] = $Env[$k] }
$job = Start-Job -ScriptBlock {
    param($s, $tag, $app, $place, $mask, $envs)
    & "$s\tour_run.ps1" -Tag $tag -App $app -Places @($place) -Stop 5000 -Settings @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0') `
        -Env $envs -Unpaced -Mask $mask -MovesKind long
} -ArgumentList $scratch, $Tag, $App, $Place, $Mask, $envs
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
$deadline = (Get-Date).AddSeconds(240)
$proc = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if (-not $proc) { $proc = Get-Process BlueWake -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$((Resolve-Path $App).Path)*" } | Select-Object -First 1 }
    $last = Select-String -Path "$data\stderr.txt" -Pattern '\[perf\] retrace=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($last -and [int]$last.Matches[0].Groups[1].Value -ge 1800) { break }
}
$line = Select-String -Path "$data\stderr.txt" -Pattern 'guest cpu state ([0-9A-Fa-f]+) .*pc at \+(\d+)' | Select-Object -First 1
$state = $line.Matches[0].Groups[1].Value; $pcoff = $line.Matches[0].Groups[2].Value
$tid = python -c "import sys; sys.path.insert(0, r'$old'); import thread_sampler as ts; print(ts.threads_of($($proc.Id))[0])"
"pid=$($proc.Id) main-thread=$tid"
$host_job = Start-Job -ScriptBlock { param($old, $procid, $sec, $out) python "$old\thread_sampler_w32.py" $procid $sec $out 3 } `
    -ArgumentList $old, $proc.Id, $Seconds, "$scratch\$Tag"
if ($Stacks) {
    python "$scratch\guest_stack_sampler.py" $proc.Id $state $Seconds "$scratch\$Tag-stack" $tid
} else {
    python "$old\guest_sampler.py" $proc.Id $state $pcoff $Seconds "$scratch\$Tag-guest" $tid
}
Receive-Job $host_job -Wait | Out-Null
Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
Receive-Job $job -Wait | Out-Null
Get-ChildItem "$scratch\$Tag-*.txt" | ForEach-Object Name
