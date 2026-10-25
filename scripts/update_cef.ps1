<#
.SYNOPSIS
  Check third_party\cef\ against the newest stable CEF build and install it when newer.

.EXAMPLE
  .\scripts\update_cef.ps1           # report both versions, install if newer
  .\scripts\update_cef.ps1 -Check    # report only, never download
  .\scripts\update_cef.ps1 -Yes      # update without asking
#>
[CmdletBinding()]
param(
    [string]$Platform = "",
    [switch]$Check,
    [switch]$Yes
)

$ErrorActionPreference = "Stop"

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$CefDir    = Join-Path $RepoRoot "third_party\cef"
$Install   = Join-Path $PSScriptRoot "install.ps1"
$IndexUrl  = "https://cef-builds.spotifycdn.com/index.json"

function Write-Log([string]$m) { Write-Host $m }
function Fail([string]$m)      {
    # Write-Error would THROW under $ErrorActionPreference = "Stop"; go to stderr and exit.
    [Console]::Error.WriteLine("update_cef.ps1: " + $m)
    exit 1
}

# --- what is installed now ---------------------------------------------------
$Current = ""
$Header = Join-Path $CefDir "include\cef_version.h"
if (Test-Path $Header) {
    $m = Select-String -Path $Header -Pattern '^#define CEF_VERSION "(.+)"' | Select-Object -First 1
    if ($m) { $Current = $m.Matches[0].Groups[1].Value }
}

# --- what is the newest stable build for this machine ------------------------
if (-not $Platform) {
    $arch = $env:PROCESSOR_ARCHITECTURE
    $a = switch -Regex ($arch) {
        '^(AMD64|x86_64)$'  { "x64"; break }
        '^(ARM64|aarch64)$' { "arm64"; break }
        default { Fail "cannot map PROCESSOR_ARCHITECTURE '$arch' to a CEF build. Use -Platform windows64." }
    }
    $Platform = "windows$a"
}

Write-Log "CEF index:     $IndexUrl"
Write-Log "Platform:      $Platform"

try {
    $index = Invoke-RestMethod -Uri $IndexUrl -TimeoutSec 120
} catch {
    Fail "could not read the CEF build index: $($_.Exception.Message)"
}
$stable = @($index.$Platform.versions) | Where-Object { $_.channel -eq "stable" } | Select-Object -First 1
if (-not $stable) { Fail "no stable CEF builds listed for $Platform" }
$Latest = $stable.cef_version

Write-Log "Installed:     $(if ($Current) { $Current } else { "nothing at third_party\cef\" })"
Write-Log "Latest stable: $Latest"
Write-Log ""

function To-Tuple([string]$v) {
    $core = ($v -split '\+')[0]              # 156.0.7+g47dbc1d+... -> 156.0.7
    $parts = $core -split '\.'
    ,@([int]$parts[0], [int]$parts[1], [int]$parts[2])
}

if (-not $Current) {
    if ($Check) {
        Write-Log "-Check: CEF is not installed; it would download $Latest."
        exit 0
    }
    Write-Log "No CEF distribution found - installing $Latest now."
    & $Install -Platform $Platform -Version $Latest -Yes
    exit $LASTEXITCODE
}

if ($Current -eq $Latest) {
    Write-Log "Already up to date - nothing to do."
    exit 0
}

$cur = To-Tuple $Current
$new = To-Tuple $Latest
$cmp = 0                                  # lexicographic: compare MAJOR, then MINOR, then PATCH
for ($i = 0; $i -lt 3; $i++) {
    if ($cur[$i] -lt $new[$i]) { $cmp = -1; break }
    if ($cur[$i] -gt $new[$i]) { $cmp = 1;  break }
}
if ($cmp -gt 0) {
    Write-Log "Installed build is NEWER than the index's stable head (custom or pre-release build)."
    Write-Log "Not downgrading. Use install.ps1 -Version <build> if you really want it."
    exit 0
}

Write-Log "Newer CEF build available: $Current -> $Latest"
if ($Check) {
    Write-Log "-Check: it would download and install $Latest."
    exit 0
}

if (-not $Yes -and -not [Console]::IsInputRedirected) {
    $answer = Read-Host "Update third_party\cef\ now? [y/N]"
    if ($answer -notmatch '^(y|yes)$') {
        Write-Log "Aborted - nothing downloaded."
        exit 0
    }
}

Write-Log "Updating: $Current -> $Latest"
& $Install -Platform $Platform -Version $Latest -Yes
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Now = ""
if (Test-Path $Header) {
    $m = Select-String -Path $Header -Pattern '^#define CEF_VERSION "(.+)"' | Select-Object -First 1
    if ($m) { $Now = $m.Matches[0].Groups[1].Value }
}
Write-Log ""
if ($Now -eq $Latest) {
    Write-Log "Done. third_party\cef\ is now $Now."
} else {
    Fail "update finished but third_party\cef\ reports '$Now' (expected $Latest)."
}
Write-Log "Rebuild against it: cmake -S . -B build && cmake --build build --config Release"
