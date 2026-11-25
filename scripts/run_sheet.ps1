# Runs the game for a while with dumps every N frames and builds a contact sheet of the final
# images (the last full-screen resolve of each dumped frame): scripts\run_sheet.ps1 -Seconds 120 -Every 150 -Env @{ DR_AUTO_INPUT = "700:start" }
param([hashtable]$Env = @{}, [int]$Seconds = 120, [int]$Every = 150, [string]$Out = "$env:TEMP\dr_sheet.png", [string]$Run = "F:\downloads\Dead Rising\_recomp\run")
$dump = Join-Path $Run "dump"
New-Item -ItemType Directory -Force $dump | Out-Null
Get-ChildItem $dump -Filter *.bmp | Remove-Item
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$env:DR_DUMP = "$Every"
$exe = "F:\downloads\Dead Rising\_recomp\runtime\build\DRRecomp.exe"
$p = Start-Process -FilePath $exe -ArgumentList "`"F:\downloads\Dead Rising`"" -WorkingDirectory $Run -PassThru -NoNewWindow -RedirectStandardOutput "$Run\stdout.txt" -RedirectStandardError "$Run\stderr.txt"
if (-not $p.WaitForExit($Seconds * 1000)) { Stop-Process -Id $p.Id -Force }
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
Remove-Item Env:DR_DUMP -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
$frames = Get-ChildItem $dump -Filter "frame*_resolve*_1280x720_*.bmp" | Where-Object { $_.Name -notmatch "source" } |
    Group-Object { $_.Name.Substring(5, 5) } | Sort-Object Name
$cols = 4; $w = 320; $h = 180
$rows = [Math]::Max(1, [Math]::Ceiling($frames.Count / $cols))
$sheet = New-Object System.Drawing.Bitmap ($cols * $w), ($rows * ($h + 16))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font "Arial", 9
$i = 0
foreach ($group in $frames) {
    $last = $group.Group | Sort-Object Name | Select-Object -Last 1
    $b = [System.Drawing.Bitmap]::FromFile($last.FullName)
    $x = ($i % $cols) * $w; $y = [Math]::Floor($i / $cols) * ($h + 16)
    $g.DrawImage($b, $x, $y, $w, $h)
    $g.DrawString("frame $($group.Name)", $font, [System.Drawing.Brushes]::Red, $x, $y + $h)
    $b.Dispose(); $i++
}
$g.Dispose(); $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
Write-Output "$($frames.Count) frames -> $Out"
Get-Content "$Run\DRRecomp.log" | Select-String "\[input\]|fps" | Select-Object -Last 6 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(160, $_.Line.Length)) }
