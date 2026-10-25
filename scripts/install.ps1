<#
.SYNOPSIS
  Download and unpack the CEF (Chromium) binary distribution into third_party\cef\.

.DESCRIPTION
  The PowerShell twin of scripts/install.sh, shipped in the Windows release bundle
  (a plain Windows box has no bash). CMake looks ONLY at third_party\cef\.

.EXAMPLE
  .\scripts\install.ps1                      # detect this machine, ask, install latest stable
  .\scripts\install.ps1 -Yes                 # no questions (CI)
  .\scripts\install.ps1 -Platform windows64  # pick the build by hand
  .\scripts\install.ps1 -Version 156.0.7+g47dbc1d+chromium-156.0.8078.17
  .\scripts\install.ps1 -Check               # show what it would do, change nothing
#>
[CmdletBinding()]
param(
    [string]$Platform = "",
    [string]$Version  = "",
    [switch]$Yes,
    [switch]$Check
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CefDir   = Join-Path $RepoRoot "third_party\cef"
$IndexUrl = "https://cef-builds.spotifycdn.com/index.json"
$Cdn      = "https://cef-builds.spotifycdn.com"
$Distro   = "standard"
$Known    = @("linux32","linux64","linuxarm","linuxarm64","macosarm64","macosx64",
              "windows32","windows64","windowsarm64")

function Write-Log([string]$m) { Write-Host $m }
function Fail([string]$m)      {
    # Write-Error would THROW under $ErrorActionPreference = "Stop"; go to stderr and exit.
    [Console]::Error.WriteLine("install.ps1: " + $m)
    exit 1
}

function Get-CurrentVersion {
    $h = Join-Path $CefDir "include\cef_version.h"
    if (Test-Path $h) {
        $m = Select-String -Path $h -Pattern '^#define CEF_VERSION "(.+)"' | Select-Object -First 1
        if ($m) { return $m.Matches[0].Groups[1].Value }
    }
    return ""
}

# ---------------------------------------------------------------- platform ---
function Resolve-Platform {
    $arch = $env:PROCESSOR_ARCHITECTURE
    switch -Regex ($arch) {
        '^(AMD64|x86_64)$' { $a = "x64" }
        '^(ARM64|aarch64)$' { $a = "arm64" }
        default { Fail "cannot map PROCESSOR_ARCHITECTURE '$arch' to a CEF build. Use -Platform windows64." }
    }
    return "windows$a"
}

$script:ChoseManually = $false
if (-not $Platform) {
    $Platform = Resolve-Platform
    Write-Log "Detected OS:   Windows"
    Write-Log "Detected arch: $env:PROCESSOR_ARCHITECTURE"
    Write-Log "CEF platform:  $Platform"
    Write-Log "Builds known to this index: $($Known -join ' ')"
    if (-not $Yes -and -not [Console]::IsInputRedirected) {
        $answer = Read-Host "Press Enter to install the latest stable CEF for $Platform, or type another platform name"
        if ($answer) {
            if ($Known -notcontains $answer) { Fail "'$answer' is not a known platform. Try one of: $($Known -join ' ')" }
            $Platform = $answer
            $script:ChoseManually = $true
            Write-Log "Using platform: $Platform"
        }
    } else {
        Write-Log "Non-interactive shell: keeping the detected platform."
    }
} else {
    if ($Known -notcontains $Platform) { Fail "'$Platform' is not a known platform. Try one of: $($Known -join ' ')" }
    Write-Log "CEF platform:  $Platform (chosen)"
}

# ------------------------------------------------------------------- index ---
Write-Log "Reading $IndexUrl ..."
try {
    $index = Invoke-RestMethod -Uri $IndexUrl -TimeoutSec 120
} catch {
    Fail "could not read the CEF build index: $($_.Exception.Message)"
}
$entry = $null
$all = @($index.$Platform.versions)
if (-not $all) { Fail "unknown platform '$Platform'" }
if ($Version) {
    $entry = $all | Where-Object { $_.cef_version -eq $Version } | Select-Object -First 1
    if (-not $entry) { Fail "version $Version is not in the index for $Platform" }
} else {
    $entry = $all | Where-Object { $_.channel -eq "stable" } | Select-Object -First 1
    if (-not $entry) { Fail "the index reports no stable builds for $Platform" }
}
$file = @($entry.files) | Where-Object { $_.type -eq $Distro } | Select-Object -First 1
if (-not $file) { Fail "build $($entry.cef_version) has no '$Distro' distribution for $Platform" }

$Found     = $entry.cef_version
$FileName  = $file.name
$FileSha1  = $file.sha1
$Url       = "$Cdn/$FileName"
$SizeMb    = [math]::Round([int64]$file.size / 1MB)
$Current   = Get-CurrentVersion

Write-Log ""
Write-Log "Latest stable in the index: $Found   (distribution: $Distro)"
Write-Log "Currently installed:        $(if ($Current) { $Current } else { "nothing at $CefDir" })"
Write-Log "Download:                   $Url"
Write-Log "Size:                       $SizeMb MB"

if ($Check) {
    if ($Current -eq $Found) { Write-Log "Already up to date: nothing to do (-Check)." }
    else                     { Write-Log "-Check: it would download and install $Found." }
    exit 0
}
if ($Current -eq $Found) {
    Write-Log ""
    Write-Log "Already installed: $Found. Nothing to download."
    Write-Log "To build: cmake -S . -B build && cmake --build build --config Release"
    exit 0
}

# ------------------------------------------------------------------ download --
if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
    Fail "tar.exe is required to unpack the distribution (it ships with Windows 10 1803+)."
}
$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("cef-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
    $Archive = Join-Path $Tmp $FileName
    Write-Log ""
    Write-Log "Downloading $FileName ..."
    $ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is 100x slower with the bar on
    Invoke-WebRequest -Uri $Url -OutFile $Archive -UseBasicParsing

    if ($FileSha1) {
        $got = (Get-FileHash -Path $Archive -Algorithm SHA1).Hash
        if ($got -ne $FileSha1) { Fail "sha1 mismatch for $FileName (expected $FileSha1, got $got). Not installing." }
        Write-Log "sha1 verified: $got"
    }

    Write-Log "Unpacking ..."
    $Unpacked = Join-Path $Tmp "unpacked"
    New-Item -ItemType Directory -Path $Unpacked | Out-Null
    & tar -xjf $Archive -C $Unpacked
    if ($LASTEXITCODE -ne 0) { Fail "tar failed to unpack $FileName (exit $LASTEXITCODE)." }

    $VersionHeader = Get-ChildItem -Path $Unpacked -Recurse -Filter cef_version.h |
                     Where-Object { $_.FullName -match [regex]::Escape("include") } |
                     Select-Object -First 1
    if (-not $VersionHeader) { Fail "unpacked archive has no include\cef_version.h" }
    $Inner = $VersionHeader.Directory.Parent.FullName
    $m = Select-String -Path (Join-Path $Inner "include\cef_version.h") -Pattern '^#define CEF_VERSION "(.+)"' |
         Select-Object -First 1
    $Installed = $m.Matches[0].Groups[1].Value

    # Swap it in only after a complete download + unpack.
    Write-Log "Installing into $CefDir ..."
    if (Test-Path $CefDir) { Remove-Item -Recurse -Force $CefDir }
    New-Item -ItemType Directory -Path (Split-Path -Parent $CefDir) -Force | Out-Null
    Move-Item -Path $Inner -Destination $CefDir

    Write-Log ""
    Write-Log "Installed CEF version:  $Installed"
    Write-Log "Installed at:           $CefDir"
    Write-Log ""
    Write-Log "Next step - build LucyN:"
    Write-Log "  cmake -S . -B build && cmake --build build --config Release"
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}
