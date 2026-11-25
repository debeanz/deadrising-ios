# Builds the PUBLIC source repository (docs/ios.md) in a separate folder: only source code, the
# tools as submodules pinned to the commits the patches in patches/ apply to, no game files and
# nothing generated from them (no ppc/, analysis/, private/, run/, dumps or screenshots).
#   powershell -File scripts\ios\make-source-repo.ps1 [-Out F:\downloads\deadrising-ios] [-NoCommit]
param([string]$Out = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) "deadrising-ios"),
      [switch]$NoCommit)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (Test-Path $Out) { throw "$Out exists: remove it or pass -Out" }
New-Item -ItemType Directory $Out | Out-Null

# Source files to copy (relative to the project), and what never leaves this machine.
$include = @(
    ".github", "config", "docs", "patches", "scripts", "xfb2xenos\main.cpp", "xfb2xenos\build.bat",
    "runtime\CMakeLists.txt", "runtime\src", "runtime\ios", "runtime\thirdparty\o1heap",
    "README.md", "build_runtime.bat", "build_tools.bat", "build_xenos.bat", "run_runtime.ps1"
)
$exclude = @("docs\images", "runtime\src\profiler\call_profiler_gen.cpp",
    "config\pdata_dump.txt", "config\imports.txt", "config\imports_status.txt", "config\imports_to_implement.txt")

foreach ($item in $include) {
    $src = Join-Path $root $item
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $Out $item
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -Recurse $src $dst
}
foreach ($item in $exclude) { Remove-Item -Recurse -Force (Join-Path $Out $item) -ErrorAction SilentlyContinue }

@"
# Never commit anything made from the game (docs/ios.md).
ppc/
analysis/
private/
run/
run_*/
build/
build_*/
out/
deps/
*.xex
*.arc
*.exe
*.dll
*.log
*.bmp
docs/images/
"@ | Set-Content -Encoding utf8 (Join-Path $Out ".gitignore")

Push-Location $Out
try {
    git init -q -b main
    git config core.autocrlf false
    # Tools and libraries as submodules at the commits this project was built and tested with.
    $submodules = @(
        @{ Path = "tools/XenonRecomp"; Url = "https://github.com/hedge-dev/XenonRecomp.git"; Commit = (git -C (Join-Path $root "tools\XenonRecomp") rev-parse HEAD) },
        @{ Path = "tools/XenosRecomp"; Url = "https://github.com/squidbus/XenosRecomp.git"; Commit = (git -C (Join-Path $root "tools\XenosRecomp") rev-parse HEAD) },
        @{ Path = "tools/plume"; Url = "https://github.com/DevZer0D4Y/plume.git"; Commit = (git -C (Join-Path $root "tools\plume") rev-parse HEAD) },
        @{ Path = "runtime/thirdparty/SDL"; Url = "https://github.com/libsdl-org/SDL.git"; Commit = (git -C (Join-Path $root "runtime\thirdparty\SDL") rev-parse HEAD) },
        @{ Path = "runtime/thirdparty/unordered_dense"; Url = "https://github.com/martinus/unordered_dense.git"; Commit = (git -C (Join-Path $root "runtime\thirdparty\unordered_dense") rev-parse HEAD) }
    )
    foreach ($s in $submodules) {
        Write-Output "submodule $($s.Path) @ $($s.Commit)"
        git submodule add -q $s.Url $s.Path
        git -C $s.Path checkout -q $s.Commit
        git add $s.Path
    }
    git add -A
    $files = git ls-files | Measure-Object
    Write-Output "$($files.Count) files staged in $Out"
    $bad = git ls-files | Where-Object { $_ -match '(^|/)(ppc|analysis|private|run)/|\.(xex|arc|exe|dll|bmp)$' }
    if ($bad) { throw "game-derived files staged: $($bad -join ', ')" }
    if (-not $NoCommit) {
        git -c core.autocrlf=false commit -q -m "Dead Rising recompiled: source for the iOS build (no game files)"
        Write-Output "committed; push with scripts\ios\publish.ps1"
    }
}
finally { Pop-Location }
