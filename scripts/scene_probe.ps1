# Runs the game with the given debug environment until the frame-600 scene dump exists, then prints
# its average brightness. Example: scripts\scene_probe.ps1 -Env @{ DR_TEX_WHITE_ADDR = "B4A4A000-C0000000" }
param([hashtable]$Env = @{}, [int]$Frame = 600, [int]$Seconds = 60, [string]$Run = "F:\downloads\Dead Rising\_recomp\run")
$dump = Join-Path $Run "dump"
New-Item -ItemType Directory -Force $dump | Out-Null
Get-ChildItem $dump -Filter *.bmp | Remove-Item
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$env:DR_DUMP = "$Frame"
$exe = "F:\downloads\Dead Rising\_recomp\runtime\build\DRRecomp.exe"
$p = Start-Process -FilePath $exe -ArgumentList "`"F:\downloads\Dead Rising`"" -WorkingDirectory $Run -PassThru -NoNewWindow -RedirectStandardOutput "$Run\stdout.txt" -RedirectStandardError "$Run\stderr.txt"
$scene = Join-Path $dump ("frame{0:D5}_scene_1280x720.bmp" -f $Frame)
$deadline = (Get-Date).AddSeconds($Seconds)
while (-not (Test-Path $scene) -and (Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Milliseconds 500 }
Start-Sleep -Seconds 2 # let the remaining dumps of that frame finish
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Remove-Item Env:DR_DUMP -ErrorAction SilentlyContinue
if (Test-Path $scene) { & "C:\Program Files\Git\usr\bin\perl.exe" "F:\downloads\Dead Rising\_recomp\scripts\bmpavg.pl" $scene } else { Write-Output "no scene dump" }
