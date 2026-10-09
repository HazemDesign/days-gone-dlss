<#
.SYNOPSIS
    Overlay this repo's Days Gone mod onto a Luma-Framework checkout and register
    it in Luma.sln — the same step the GitHub Actions workflow performs.

.DESCRIPTION
    This repo intentionally does NOT vendor Luma-Framework (it's huge plus
    submodules). Instead we keep only our overlay:
      game\Days Gone\*     -> <LumaRoot>\Source\Games\Days Gone\
      shaders\Days Gone\*  -> <LumaRoot>\Shaders\Days Gone\
    and inject the project + its x64-only build configs into <LumaRoot>\Luma.sln
    (same mapping upstream uses for x64-only games: Win32 solution configs map
    to the x64 project config without a Build.0 entry).

.EXAMPLE
    # CI usage (from the repo root, after checking out Luma-Framework to .\luma):
    pwsh -ExecutionPolicy Bypass -File scripts\inject-game.ps1 -LumaRoot .\luma

    # Local dev usage (Luma cloned next to this repo):
    pwsh -ExecutionPolicy Bypass -File scripts\inject-game.ps1 -LumaRoot ..\Luma-Framework
#>
param(
    [string]$LumaRoot = "luma",
    [string]$OverlayRoot = $PSScriptRoot + "\..",
    [string]$ProjectName = "Days Gone",
    [string]$ProjectGuid = "{0D346BDD-7E1D-4360-9C41-48052DB7EEA5}"
)

$ErrorActionPreference = "Stop"

$LumaRoot = (Resolve-Path $LumaRoot).Path
if ([string]::IsNullOrEmpty($OverlayRoot)) { $OverlayRoot = Join-Path $PSScriptRoot '..' }
$OverlayRoot = (Resolve-Path $OverlayRoot).Path

$gameSrc = Join-Path $OverlayRoot "game\$ProjectName"
$shaderSrc = Join-Path $OverlayRoot "shaders\$ProjectName"
foreach ($dir in @($gameSrc, $shaderSrc)) {
    if (-not (Test-Path $dir -PathType Container)) { throw "Overlay dir missing: $dir" }
}

$gameDst = Join-Path $LumaRoot "Source\Games\$ProjectName"
$shaderDst = Join-Path $LumaRoot "Shaders\$ProjectName"
New-Item -ItemType Directory -Path $gameDst -Force | Out-Null
New-Item -ItemType Directory -Path $shaderDst -Force | Out-Null
Copy-Item -Path "$gameSrc\*" -Destination $gameDst -Recurse -Force
Copy-Item -Path "$shaderSrc\*" -Destination $shaderDst -Recurse -Force
Write-Host "Overlay copied: $gameDst, $shaderDst"

# --- Patch Luma.sln (idempotent) ---
$slnPath = Join-Path $LumaRoot "Luma.sln"
$sln = Get-Content $slnPath -Raw
if ($sln -match [regex]::Escape($ProjectGuid)) {
    Write-Host "Luma.sln already contains $ProjectName ($ProjectGuid), skipping patch."
    return
}

# 1. Project declaration, right before the Global section (upstream pattern).
$projectDecl = "Project(`"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}`") = `"$ProjectName`", `"Source\Games\$ProjectName\$ProjectName.vcxproj`", `"$ProjectGuid`"`r`nEndProject`r`n"
$globalIdx = $sln.IndexOf("`r`nGlobal")
if ($globalIdx -lt 0) { $globalIdx = $sln.IndexOf("`nGlobal"); }
if ($globalIdx -lt 0) { throw "Could not find Global section in Luma.sln" }
$sln = $sln.Insert($globalIdx + 2, $projectDecl)

# 2. x64-only build configs, mirroring e.g. "Mass Effect Andromeda" in upstream Luma.sln:
#    every solution config maps to the x64 project config; Win32 entries only set
#    ActiveCfg (no Build.0) so they never try to build this project as Win32.
$guidNoBraces = $ProjectGuid.Trim('{', '}')
$configs = @('Development-Debug', 'Development-Release', 'Publishing-Release', 'Test-Release')
$lines = foreach ($cfg in $configs) {
    "`t`t{$guidNoBraces}.$cfg|Win32.ActiveCfg = $cfg|x64"
    "`t`t{$guidNoBraces}.$cfg|x64.ActiveCfg = $cfg|x64"
    "`t`t{$guidNoBraces}.$cfg|x64.Build.0 = $cfg|x64"
}
$cfgBlock = ($lines -join "`r`n") + "`r`n"

$marker = "GlobalSection(ProjectConfigurationPlatforms) = postSolution"
$markerIdx = $sln.IndexOf($marker)
if ($markerIdx -lt 0) { throw "Could not find ProjectConfigurationPlatforms section in Luma.sln" }
$endIdx = $sln.IndexOf("EndGlobalSection", $markerIdx)
if ($endIdx -lt 0) { throw "Could not find EndGlobalSection after ProjectConfigurationPlatforms" }
$sln = $sln.Insert($endIdx, $cfgBlock)

# Preserve original encoding (UTF-8 with BOM) and CRLF.
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText($slnPath, $sln, $utf8Bom)
Write-Host "Patched Luma.sln with $ProjectName."
