# Runs DRRecomp.exe against the game folder for a limited time, then prints the log tail.
param([int]$Seconds = 30, [int]$Tail = 80, [switch]$Trace, [switch]$Profile)
if ($Trace) { $env:DR_TRACE = "1" } else { Remove-Item Env:DR_TRACE -ErrorAction SilentlyContinue }
$run = "F:\downloads\Dead Rising\_recomp\run"
New-Item -ItemType Directory -Force $run | Out-Null
$exe = if ($Profile) { "F:\downloads\Dead Rising\_recomp\runtime\build_profile\DRRecomp.exe" } else { "F:\downloads\Dead Rising\_recomp\runtime\build\DRRecomp.exe" }
$p = Start-Process -FilePath $exe -ArgumentList "`"F:\downloads\Dead Rising`"" -WorkingDirectory $run -PassThru -NoNewWindow -RedirectStandardOutput "$run\stdout.txt" -RedirectStandardError "$run\stderr.txt"
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force; Write-Output "== still running after $Seconds s (stopped)" } else { Write-Output "== exited, code $($p.ExitCode)" }
Write-Output "== log lines: $((Get-Content "$run\DRRecomp.log" | Measure-Object -Line).Lines)"
Get-Content "$run\DRRecomp.log" -Tail $Tail
