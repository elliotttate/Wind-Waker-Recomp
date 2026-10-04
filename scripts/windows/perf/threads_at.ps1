param([Parameter(Mandatory)] [string]$App, [string]$Place = 'sea:41:0', [string]$Tag = 'thr', [long]$Mask = 0x000F0000,
      [string[]]$Settings = @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0'), [switch]$Paced, [hashtable]$Env = @{})
# One place by warp, Link running and turning; from retrace 1800, every thread's CPU over 15 s (thread_cpu.py).
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$job = Start-Job -ScriptBlock {
    param($s, $tag, $app, $place, $mask, $settings, $paced, $envs)
    if ($paced) { & "$s\tour_run.ps1" -Tag $tag -App $app -Places @($place) -Stop 3000 -Settings $settings -Env $envs -Mask $mask -MovesKind long }
    else { & "$s\tour_run.ps1" -Tag $tag -App $app -Places @($place) -Stop 3000 -Settings $settings -Env $envs -Unpaced -Mask $mask -MovesKind long }
} -ArgumentList $s, $Tag, $App, $Place, $Mask, $Settings, [bool]$Paced, $Env
$data = "$PSScriptRoot\..\..\..\build\windows\test-$Tag"
$deadline = (Get-Date).AddSeconds(240); $proc = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if (-not $proc) { $proc = Get-Process BlueWake -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$((Resolve-Path $App).Path)*" } | Select-Object -First 1 }
    $last = Select-String -Path "$data\stderr.txt" -Pattern '\[perf\] retrace=(\d+)' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($last -and [int]$last.Matches[0].Groups[1].Value -ge 1800) { break }
}
python "$s\thread_cpu.py" $proc.Id 15 14
Select-String -Path "$data\stderr.txt" -Pattern '^\[fps\]' | Select-Object -Last 2 | ForEach-Object Line
Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
Receive-Job $job -Wait | Out-Null
