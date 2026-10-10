<#
.SYNOPSIS
    One-command log bundle for Days Gone DLSS debugging (read-only on game dir).

.DESCRIPTION
    Collects scattered logs (see game\Days Gone\main.cpp:4882 marker,
    :4966 frame.log, :5072 draws.log + ReShade.log) into a timestamped bundle
    with a summary.txt. Writes only to OutDir, never modifies GameDir.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File scripts\collect-bundle.ps1
    pwsh -ExecutionPolicy Bypass -File scripts\collect-bundle.ps1 -TailLines 800
    pwsh -ExecutionPolicy Bypass -File scripts\collect-bundle.ps1 -GameDir "<SteamLibrary>\BendGame\Binaries\Win64"
    (or set $env:LUMA_DAYS_GONE_BIN_PATH to the folder containing DaysGone.exe
    instead of passing -GameDir every time)
#>
param(
    [string]$GameDir = $env:LUMA_DAYS_GONE_BIN_PATH,
    [string]$OutDir = "",
    [int]$TailLines = 400
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    throw "GameDir is not set. Pass -GameDir <path to folder containing DaysGone.exe> or set the LUMA_DAYS_GONE_BIN_PATH environment variable."
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutDir = Join-Path $RepoRoot "artifacts\debug-bundles\$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "Bundle dir: $OutDir"

if (-not (Test-Path $GameDir -PathType Container)) {
    Write-Host "WARNING: GameDir not found (bundle will contain repo-side info only)."
}

# Privacy: never write the absolute local game path or user profile into
# bundles/shared text. Show only the last 3 game-dir segments
# (e.g. "...\BendGame\Binaries\Win64"); redact $env:USERPROFILE as "~".
$GameDirShown = $GameDir
try {
    $segs = @($GameDir -split '[\\/]' | Where-Object { $_ -ne '' })
    if ($segs.Count -gt 3) { $GameDirShown = '...\' + ($segs[-3..-1] -join '\') }
} catch { $GameDirShown = '(game dir redacted)' }

function Redact-PrivatePaths {
    param([string]$Text)
    $out = $Text
    try {
        if (-not [string]::IsNullOrEmpty($env:USERPROFILE)) {
            $out = $out -replace [regex]::Escape($env:USERPROFILE), '~'
        }
        if (-not [string]::IsNullOrEmpty($env:USERNAME)) {
            $out = $out -replace [regex]::Escape($env:USERNAME), '<user>'
        }
    } catch { }
    return $out
}

function Set-RedactedContent {
    param([string]$Path, [string[]]$Lines)
    $clean = @($Lines | ForEach-Object { Redact-PrivatePaths $_ })
    Set-Content -Path $Path -Value $clean -Encoding utf8
}

function Copy-IfPresent {
    param([string]$Src, [string]$Dst)
    if (Test-Path $Src -PathType Leaf) {
        Copy-Item -Path $Src -Destination $Dst -Force
        return $true
    }
    return $false
}

function Get-Tail {
    param([string]$Path, [int]$N)
    try { return @(Get-Content $Path -Tail $N -ErrorAction Stop) } catch { return @() }
}

# --- 1. Full copies: marker + draws log ---
$markerName = "Luma-DaysGone-load-marker.txt"
$drawsName  = "Luma-DaysGone-draws.log"
$frameName  = "Luma-DaysGone-frame.log"
$fullName   = "Luma-DaysGone-full.log"
Copy-IfPresent (Join-Path $GameDir $markerName) (Join-Path $OutDir $markerName) | Out-Null
Copy-IfPresent (Join-Path $GameDir $drawsName)  (Join-Path $OutDir $drawsName)  | Out-Null

# --- 2. frame.log: last $TailLines always; full copy too if < 1MB ---
$frameSrc = Join-Path $GameDir $frameName
$frameTailName = "frame-tail.log"
if (Test-Path $frameSrc -PathType Leaf) {
    $size = (Get-Item $frameSrc).Length
    Get-Tail $frameSrc $TailLines | Set-Content (Join-Path $OutDir $frameTailName) -Encoding utf8
    if ($size -lt 1MB) {
        Copy-Item -Path $frameSrc -Destination (Join-Path $OutDir $frameName) -Force
    } else {
        Write-Host ("frame.log is {0:N1} MB; bundled tail only + full skipped." -f ($size / 1MB))
    }
} else {
    "MISSING: $frameName not found (pass -GameDir explicitly)" | Set-Content (Join-Path $OutDir $frameTailName) -Encoding utf8
}

# --- 2b. full.log: last $TailLines always; full copy too if < 2MB ---
$fullSrc = Join-Path $GameDir $fullName
$fullTailName = "full-tail.log"
if (Test-Path $fullSrc -PathType Leaf) {
    $size = (Get-Item $fullSrc).Length
    Get-Tail $fullSrc $TailLines | Set-Content (Join-Path $OutDir $fullTailName) -Encoding utf8
    if ($size -lt 2MB) {
        Copy-Item -Path $fullSrc -Destination (Join-Path $OutDir $fullName) -Force
    } else {
        Write-Host ("full.log is {0:N1} MB; bundled tail only + full skipped." -f ($size / 1MB))
    }
} else {
    "MISSING: $fullName not found (run Start full-trace in TEST/DEVELOPMENT config)" | Set-Content (Join-Path $OutDir $fullTailName) -Encoding utf8
}

# --- 3. ReShade.log: filtered view + raw tail; ReShade.ini as-is ---
$reshadeLog = Join-Path $GameDir "ReShade.log"
$filterPattern = "DaysGone|cDLSS|cSTATS|cSCAN|cVIEW|capCS|AUDIT|FULL|FULLAGG"
if (Test-Path $reshadeLog -PathType Leaf) {
    try {
        $found = @(Select-String -Path $reshadeLog -Pattern $filterPattern |
            ForEach-Object { $_.Line })
        Set-RedactedContent (Join-Path $OutDir "ReShade-filtered.log") $found
    } catch {
        "FILTER-FAILED" | Set-Content (Join-Path $OutDir "ReShade-filtered.log") -Encoding utf8
    }
    Set-RedactedContent (Join-Path $OutDir "ReShade-tail.log") (Get-Tail $reshadeLog $TailLines)
} else {
    "MISSING: ReShade.log not found (pass -GameDir explicitly)" | Set-Content (Join-Path $OutDir "ReShade-tail.log") -Encoding utf8
    "MISSING: ReShade.log not found (pass -GameDir explicitly)" | Set-Content (Join-Path $OutDir "ReShade-filtered.log") -Encoding utf8
}
if (Test-Path (Join-Path $GameDir "ReShade.ini") -PathType Leaf) {
    $iniLines = @(Get-Content (Join-Path $GameDir "ReShade.ini") -ErrorAction Stop)
    Set-RedactedContent (Join-Path $OutDir "ReShade.ini") $iniLines
}

# --- 4. Addon version line from repo source ---
$buildId = "(unknown)"
$mainCpp = Join-Path $RepoRoot "game\Days Gone\main.cpp"
$m = Select-String -Path $mainCpp -Pattern 'DG_BUILD_ID\s*=\s*"([^"]+)"' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($m -and $m.Matches.Count -gt 0) { $buildId = $m.Matches[0].Groups[1].Value }
"DG_BUILD_ID=$buildId" | Set-Content (Join-Path $OutDir "version.txt") -Encoding utf8

# --- 5. summary.txt ---
$sumPath = Join-Path $OutDir "summary.txt"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("Days Gone DLSS debug bundle")
[void]$sb.AppendLine(("date=" + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")))
[void]$sb.AppendLine("build_id=$buildId")
[void]$sb.AppendLine("game_dir=$GameDirShown")
[void]$sb.AppendLine("")

function Add-FileStats {
    param([string]$Label, [string]$Path)
    if (Test-Path $Path -PathType Leaf) {
        $it = Get-Item $Path
        $n = @(Get-Content $Path -ErrorAction SilentlyContinue).Count
        [void]$sb.AppendLine(("{0}: {1} bytes, {2} lines ({3})" -f $Label, $it.Length, $n, $it.Name))
    } else {
        [void]$sb.AppendLine(("${Label}: MISSING"))
    }
}
Add-FileStats "marker" (Join-Path $GameDir $markerName)
Add-FileStats "draws"  (Join-Path $GameDir $drawsName)
Add-FileStats "frame"  (Join-Path $GameDir $frameName)
Add-FileStats "full"   (Join-Path $GameDir $fullName)
Add-FileStats "reshade" (Join-Path $GameDir "ReShade.log")

# First-Draw cDLSS line: last match of "cDLSS first Draw".
try {
    $fd = Select-String -Path $reshadeLog -Pattern "cDLSS first Draw" -ErrorAction Stop | Select-Object -Last 1
    if ($fd) { [void]$sb.AppendLine(""); [void]$sb.AppendLine("first-Draw cDLSS:"); [void]$sb.AppendLine((Redact-PrivatePaths $fd.Line)) }
    else { [void]$sb.AppendLine(""); [void]$sb.AppendLine("first-Draw cDLSS: (no match in ReShade.log)") }
} catch {
    [void]$sb.AppendLine(""); [void]$sb.AppendLine("first-Draw cDLSS: (ReShade.log unavailable)")
}

# mvhash/mvfmt/dec/inv counts + slot fire hints from available log text.
try {
    $lines = @()
    if (Test-Path $reshadeLog -PathType Leaf) { $lines = @(Get-Content $reshadeLog -ErrorAction Stop) }
    $mvhash = @($lines | Where-Object { $_ -match "mvhash=" }).Count
    $mvfmt  = @($lines | Where-Object { $_ -match "mvfmt=" }).Count
    $decN   = @($lines | Where-Object { $_ -match "\bdec=" }).Count
    $invN   = @($lines | Where-Object { $_ -match "\binv=" }).Count
    $fullN  = @($lines | Where-Object { $_ -match "DaysGone FULL" }).Count
    $fullStale = @($lines | Where-Object { $_ -match "DaysGone FULL.*\bmv=stale\b" }).Count
    $fullZero  = @($lines | Where-Object { $_ -match "DaysGone FULL.*\bmv=zero\b" }).Count
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("counts: mvhash=$mvhash mvfmt=$mvfmt dec=$decN inv=$invN")
    [void]$sb.AppendLine("full: lines=$fullN mv_stale=$fullStale mv_zero=$fullZero")
    $fullAggLines = @($lines | Where-Object { $_ -match "DaysGone FULLAGG" })
    $fullAggN = $fullAggLines.Count
    [void]$sb.AppendLine("fullagg: lines=$fullAggN")
    if ($fullAggN -gt 0) {
        [void]$sb.AppendLine("last FULLAGG: " + $fullAggLines[$fullAggLines.Count - 1].Trim())
    } else {
        [void]$sb.AppendLine("last FULLAGG: (none yet - fires every 600 presents)")
    }
    $lastFullLines = @($lines | Where-Object { $_ -match "DaysGone FULL " } | Select-Object -Last 1)
    if ($lastFullLines.Count -gt 0) {
        $lfm = [regex]::Match($lastFullLines[0], "view=\S+ rejD\+\S+ projrejD\+\S+ p\S+ rot=\S+")
        if ($lfm.Success) { [void]$sb.AppendLine("last FULL view: " + $lfm.Value) }
        else { [void]$sb.AppendLine("last FULL view: (legacy line without view fields)") }
    } else {
        [void]$sb.AppendLine("last FULL view: (no FULL lines)")
    }
    $slotLines = @($lines | Where-Object { $_ -match "attempts|att=|runs=|resets=" } | Select-Object -Last 3)
    if ($slotLines.Count -gt 0) {
        [void]$sb.AppendLine("slot fire hints (last menu/counter lines):")
        foreach ($l in $slotLines) { [void]$sb.AppendLine("  " + (Redact-PrivatePaths $l.Trim())) }
    } else {
        [void]$sb.AppendLine("slot fire hints: (no attempts/runs/resets lines in ReShade.log)")
    }
} catch {
    [void]$sb.AppendLine(""); [void]$sb.AppendLine("counts: (log unreadable: $_)")
}

[void]$sb.AppendLine("")
[void]$sb.AppendLine("open questions (fill in when reporting):")
[void]$sb.AppendLine("- mode: showcase (photo mode) or gameplay?")
[void]$sb.AppendLine("- pan tested? (slow 5s camera pan for matrix animation)")
[void]$sb.AppendLine("- jitter on/off? (native TAA jitter vs forced static)")
[void]$sb.AppendLine("- run: python tools/parse-logs.py <this-bundle-dir>  (or pass ReShade.log directly)")
[System.IO.File]::WriteAllText($sumPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Wrote summary: $sumPath"
Write-Host ""
Write-Host "BUNDLE: $OutDir"
