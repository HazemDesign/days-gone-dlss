<#
.SYNOPSIS
    Fast local iteration build for the Days Gone Luma mod (vs CI 3-config matrix in .github/workflows/build.yml:39).

.DESCRIPTION
    Overlays this repo onto a local Luma-Framework checkout, builds ONLY the
    Days Gone project (x64, single config), and copies the resulting *.addon
    into the game directory. No vcpkg, no ReShade rebuild.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File scripts\build-local.ps1
    pwsh -ExecutionPolicy Bypass -File scripts\build-local.ps1 -Config Development-Release
    pwsh -ExecutionPolicy Bypass -File scripts\build-local.ps1 -GameDir "G:\Games\Days Gone\BendGame\Binaries\Win64"
    (or set $env:LUMA_DAYS_GONE_BIN_PATH to the folder containing DaysGone.exe
    instead of passing -GameDir every time)
#>
param(
    [string]$LumaRoot = (Join-Path $PSScriptRoot "..\..\Luma-Framework"),
    [ValidateSet("Development-Debug", "Development-Release", "Test-Release", "Publishing-Release")]
    [string]$Config = "Test-Release",
    [string]$GameDir = $env:LUMA_DAYS_GONE_BIN_PATH
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    throw "GameDir is not set. Pass -GameDir <path to folder containing DaysGone.exe> or set the LUMA_DAYS_GONE_BIN_PATH environment variable."
}

# Resolve LumaRoot relative to this script (default ..\Luma-Framework).
try {
    $LumaRoot = (Resolve-Path $LumaRoot).Path
} catch {
    throw "Invalid LumaRoot '$LumaRoot': directory not found. Clone Luma-Framework next to this repo or pass -LumaRoot <path>. $_"
}
$slnPath = Join-Path $LumaRoot "Luma.sln"
if (-not (Test-Path $slnPath -PathType Leaf)) {
    throw "Invalid LumaRoot '$LumaRoot': Luma.sln not found at '$slnPath'."
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# 1. Inject overlay (same step CI performs).
& (Join-Path $PSScriptRoot "inject-game.ps1") -LumaRoot $LumaRoot
if ($LASTEXITCODE -ne 0 -and $null -eq $LASTEXITCODE) { }
Write-Host "Overlay injected into $LumaRoot"

# 2. Locate msbuild (mirror CI microsoft/setup-msbuild).
$msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
if (-not $msbuild) {
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $msbuild = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
    }
}
if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw "msbuild not found. Install VS Build Tools / run from a Developer PowerShell prompt."
}
Write-Host "Using msbuild: $msbuild"

# 3. Build single project (mirror build.yml:101-106).
$vcxproj = Join-Path $LumaRoot "Source\Games\Days Gone\Days Gone.vcxproj"
if (-not (Test-Path $vcxproj -PathType Leaf)) {
    throw "Project not found at '$vcxproj' (inject-game.ps1 may have failed)."
}
$solutionDir = $LumaRoot.TrimEnd('\') + "\"
& $msbuild $vcxproj /p:Configuration=$Config /p:Platform=x64 /p:SolutionDir="$solutionDir" /m /v:m
if ($LASTEXITCODE -ne 0) { throw "msbuild failed with exit code $LASTEXITCODE." }

# 4. Find built addon: primary Luma bin path, fallback recursive search.
$primary = Join-Path $LumaRoot "Source\Games\Days Gone\bin\x64\$Config\*.addon"
$found = @(Get-ChildItem -Path $primary -ErrorAction SilentlyContinue)
if (-not $found -or $found.Count -eq 0) {
    Write-Host "Primary bin path empty ($primary); falling back to recursive search..."
    $found = @(Get-ChildItem -Path (Join-Path $LumaRoot "Source\Games\Days Gone") -Filter "*.addon" -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 5)
}
if (-not $found -or $found.Count -eq 0) {
    throw "Build succeeded but no *.addon found under 'Source\Games\Days Gone'. Check the msbuild output above."
}
$found | ForEach-Object { Write-Host ("Built: " + $_.FullName) }

# 5. Copy to game dir.
if (-not (Test-Path $GameDir -PathType Container)) {
    throw "GameDir not found: '$GameDir'. Pass -GameDir <path to folder containing DaysGone.exe>."
}
foreach ($a in $found) {
    Copy-Item -Path $a.FullName -Destination $GameDir -Force
    Write-Host ("Copied to: " + (Join-Path $GameDir $a.Name))
}

# 6. Verify: repo build id + game-dir marker tail.
$mainCpp = Join-Path $RepoRoot "game\Days Gone\main.cpp"
$buildId = "(unknown)"
$m = Select-String -Path $mainCpp -Pattern 'DG_BUILD_ID\s*=\s*"([^"]+)"' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($m -and $m.Matches.Count -gt 0) { $buildId = $m.Matches[0].Groups[1].Value }
Write-Host "DG_BUILD_ID (game\Days Gone\main.cpp:26): $buildId"

$marker = Join-Path $GameDir "Luma-DaysGone-load-marker.txt"
if (Test-Path $marker -PathType Leaf) {
    Write-Host "--- GameDir marker tail ($marker) ---"
    Get-Content $marker -Tail 5 | ForEach-Object { Write-Host $_ }
} else {
    Write-Host "NOTE: no load marker yet at $marker (expected until the new addon boots once)."
}
Write-Host "Local build OK: Config=$Config GameDir=$GameDir"
