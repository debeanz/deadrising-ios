# Copies this project's source into the existing source repository (made by make-source-repo.ps1,
# same file lists) for the next push: changed and new files, and files deleted here are deleted
# there. Nothing made from the game is copied (docs/ios.md).
#   powershell -File scripts\ios\sync-source-repo.ps1 [-Out <source repository>]
param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) "deadrising-ios"))
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $Out ".git"))) { throw "$Out is not the source repository (run make-source-repo.ps1 first)" }

# Keep in step with make-source-repo.ps1.
$include = @(
    ".github", "config", "docs", "patches", "scripts", "xfb2xenos\main.cpp", "xfb2xenos\build.bat",
    "runtime\CMakeLists.txt", "runtime\src", "runtime\ios", "runtime\thirdparty\o1heap",
    "README.md", "build_runtime.bat", "build_tools.bat", "build_xenos.bat", "run_runtime.ps1"
)
$exclude = @("docs\images", "runtime\src\profiler\call_profiler_gen.cpp",
    "config\pdata_dump.txt", "config\imports.txt", "config\imports_status.txt", "config\imports_to_implement.txt")

foreach ($item in $include) {
    $src = Join-Path $root $item
    $dst = Join-Path $Out $item
    if (-not (Test-Path $src)) { continue }
    if ((Get-Item $src).PSIsContainer) {
        robocopy $src $dst /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $item ($LASTEXITCODE)" }
    } else {
        New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
        Copy-Item -Force $src $dst
    }
}
foreach ($item in $exclude) { Remove-Item -Recurse -Force (Join-Path $Out $item) -ErrorAction SilentlyContinue }
$global:LASTEXITCODE = 0

Push-Location $Out
try {
    git add -A
    $bad = git ls-files | Where-Object { $_ -match '(^|/)(ppc|analysis|private|run)/|\.(xex|arc|exe|dll|bmp)$' }
    if ($bad) { throw "game-derived files staged: $($bad -join ', ')" }
    git status --short
}
finally { Pop-Location }
