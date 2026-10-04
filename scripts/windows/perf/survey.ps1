param([string]$App = "$PSScriptRoot\..\..\..\build\windows\F-new", [string]$Tag = 'sv', [long]$Mask = 0x000F0000,
      [int]$Stop = 1200)
# The game by warp, uncapped on four E-cores with Smooth Motion off: at each place Link runs and turns for
# about 12 s of game time. survey.py turns the logs into a table per place.
$s = Split-Path -Parent $MyInvocation.MyCommand.Path
$a = @('sea:44:0', 'sea:11:1', 'sea:13:0', 'sea:41:0', 'sea:1:100', 'sea:17:0', 'sea:25:0', 'sea:14:0',
       'M_NewD2:0:0', 'M_NewD2:10:0', 'kindan:0:0', 'kindan:11:0', 'kindan:13:0', 'Siren:0:0', 'Siren:10:0',
       'Siren:13:0', 'majroom:0:0', 'ma2room:0:0', 'Hyrule:0:0', 'Hyroom:0:0', 'kenroom:0:0', 'Omori:0:0',
       'Adanmae:0:0', 'Atorizk:0:0')
$b = @('LinkRM:0:0', 'Obshop:1:0', 'Kaisen:0:0', 'Pjavdou:0:0', 'Ojhous:0:0', 'Opub:0:0', 'Comori:0:0',
       'Abship:0:0', 'PShip:0:0', 'GanonJ:0:0', 'kinMB:10:0', 'SirenMB:23:0', 'M_DaiMB:12:0', 'kazeMB:6:0',
       'M_Dra09:9:0', 'M_DragB:0:0', 'kinBOSS:0:0', 'SirenB:0:0', 'Ekaze:0:0', 'Edaichi:0:0', 'M_DaiB:0:0',
       'kaze:0:0', 'M_Dai:0:0')
$envs = @{ BLUEWAKE_PLAYER_PROBE = '1' }
$set = @('smooth_motion=0', 'window=1280x960', 'mouse_camera=0')
& "$s\tour_run.ps1" -Tag "$Tag-a" -App $App -Places $a -Stop $Stop -Settings $set -Env $envs -Unpaced -Mask $Mask -MovesKind survey | Select-Object -First 1
& "$s\tour_run.ps1" -Tag "$Tag-b" -App $App -Places $b -Stop $Stop -Settings $set -Env $envs -Unpaced -Mask $Mask -MovesKind survey | Select-Object -First 1
python "$s\survey.py" $Stop "$PSScriptRoot\..\..\..\build\windows\test-$Tag-a\stderr.txt" ($a -join ',') "$PSScriptRoot\..\..\..\build\windows\test-$Tag-b\stderr.txt" ($b -join ',')
