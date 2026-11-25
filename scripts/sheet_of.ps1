# Contact sheet of chosen dump images: scripts\sheet_of.ps1 -Files a.bmp,b.bmp -Out x.png [-Columns 3]
param([string[]]$Files, [string]$Out, [int]$Columns = 3, [int]$Width = 426, [int]$Height = 240)
Add-Type -AssemblyName System.Drawing
$rows = [Math]::Ceiling($Files.Count / $Columns)
$sheet = New-Object System.Drawing.Bitmap ($Columns * $Width), ($rows * ($Height + 16))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font "Arial", 9
for ($i = 0; $i -lt $Files.Count; $i++) {
    $b = [System.Drawing.Bitmap]::FromFile($Files[$i])
    $x = ($i % $Columns) * $Width; $y = [Math]::Floor($i / $Columns) * ($Height + 16)
    $g.DrawImage($b, $x, $y, $Width, $Height)
    $g.DrawString([System.IO.Path]::GetFileNameWithoutExtension($Files[$i]), $font, [System.Drawing.Brushes]::Red, $x, $y + $Height)
    $b.Dispose()
}
$g.Dispose(); $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
