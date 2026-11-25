# One-time setup + build trigger for the iPhone build (docs/ios.md). Run it yourself: it creates
# GitHub repositories under your account and uploads your game files to the PRIVATE one.
#   1. a PUBLIC repository with the source (scripts\ios\make-source-repo.ps1 makes it), which runs
#      the macOS build for free;
#   2. a PRIVATE repository holding default.xex + shader.arc (release "game-data") that receives the
#      IPA (releases "ios-N").
# The workflow needs a fine-grained token (Contents: read and write, on the private repository
# only), stored as the secret GAME_DATA_TOKEN of the public one: the script asks for it.
#
#   powershell -File scripts\ios\publish.ps1 -GameDir "F:\downloads\Dead Rising"
param(
    [Parameter(Mandatory)][string]$GameDir,
    [string]$Source = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) "deadrising-ios"),
    [string]$SourceRepo = "deadrising-ios",
    [string]$DataRepo = "deadrising-ios-data"
)
$ErrorActionPreference = "Stop"
$owner = gh api user --jq .login
$xex = Join-Path $GameDir "default.xex"
$arc = Join-Path $GameDir "ntvX\system\shader.arc"
foreach ($f in @($xex, $arc)) { if (-not (Test-Path $f)) { throw "missing $f" } }
if (-not (Test-Path (Join-Path $Source ".git"))) { throw "run scripts\ios\make-source-repo.ps1 first ($Source)" }

# Private data repository + your files.
if (-not (gh repo view "$owner/$DataRepo" --json name 2>$null)) {
    gh repo create "$owner/$DataRepo" --private --description "Dead Rising iOS: my own game files and builds (private)" --add-readme
}
$vis = gh repo view "$owner/$DataRepo" --json visibility --jq .visibility
if ($vis -ne "PRIVATE") { throw "$owner/$DataRepo must be private" }
gh release view game-data --repo "$owner/$DataRepo" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { gh release create game-data --repo "$owner/$DataRepo" --title "Game data" --notes "default.xex and shader.arc (my copy)." }
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "dr-ios-data"
New-Item -ItemType Directory -Force $tmp | Out-Null
Copy-Item $xex (Join-Path $tmp "default.xex") -Force
Copy-Item $arc (Join-Path $tmp "shader.arc") -Force
gh release upload game-data --repo "$owner/$DataRepo" --clobber (Join-Path $tmp "default.xex") (Join-Path $tmp "shader.arc")
Remove-Item -Recurse -Force $tmp

# Public source repository.
if (-not (gh repo view "$owner/$SourceRepo" --json name 2>$null)) {
    gh repo create "$owner/$SourceRepo" --public --description "Dead Rising recompiled for iPhone/iPad (source only; bring your own game files)" --source $Source --push
} else {
    git -C $Source push origin main
}
gh variable set GAME_DATA_REPO --repo "$owner/$SourceRepo" --body "$owner/$DataRepo"
gh secret list --repo "$owner/$SourceRepo" | Select-String GAME_DATA_TOKEN | Out-Null
if (-not $?) {
    Write-Output "Create a fine-grained token (github.com/settings/personal-access-tokens/new): repository access = $owner/$DataRepo only, Contents = Read and write."
    gh secret set GAME_DATA_TOKEN --repo "$owner/$SourceRepo"
}

gh workflow run ios.yml --repo "$owner/$SourceRepo"
Write-Output "Build started: https://github.com/$owner/$SourceRepo/actions - the IPA arrives as a release of $owner/$DataRepo."
