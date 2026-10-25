#!/usr/bin/env bash
#
# scripts/install.sh — download and unpack the CEF (Chromium) binary distribution
# into third_party/cef/, which is the ONLY place LucyN's build looks for it.
#
#   ./scripts/install.sh                 # detect this machine, ask, install latest stable
#   ./scripts/install.sh --yes            # same, no question (CI)
#   ./scripts/install.sh --platform linuxarm64   # pick the build by hand
#   ./scripts/install.sh --version 156.0.7+g47dbc1d+chromium-156.0.8078.17
#   ./scripts/install.sh --check          # show what it would install, change nothing
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CEF_DIR="$REPO_ROOT/third_party/cef"
INDEX_URL="https://cef-builds.spotifycdn.com/index.json"
CDN_BASE="https://cef-builds.spotifycdn.com"
DISTRO_TYPE="standard"          # standard = headers + libcef_dll wrapper + Release/ + Resources/
KNOWN_PLATFORMS="linux32 linux64 linuxarm linuxarm64 macosarm64 macosx64 windows32 windows64 windowsarm64"

PLATFORM=""
WANT_VERSION=""
ASSUME_YES=0
CHECK_ONLY=0

log()  { printf '%s\n' "$*"; }
err()  { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }

# python3 on Linux/macOS; Windows (Git Bash) usually only has `python`.
PYTHON="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
py()  { [ -n "$PYTHON" ] || err "python3 (or python) is required to read the CEF build index"; "$PYTHON" "$@"; }

usage() {
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

# ---------------------------------------------------------------- platform ---
detect_platform() {
    local os arch
    os="$(uname -s 2>/dev/null || echo unknown)"
    arch="$(uname -m 2>/dev/null || echo unknown)"
    case "$os" in
        Linux)                         os=linux ;;
        Darwin)                        os=macos ;;
        MINGW*|MSYS*|CYGWIN*|Windows*) os=windows ;;
        *)                             os="" ;;
    esac
    case "$arch" in
        x86_64|amd64|x64)       arch=x64 ;;
        aarch64|arm64|armv8l)   arch=arm64 ;;
        *)                      arch="" ;;
    esac
    if [ -z "$os" ] || [ -z "$arch" ]; then
        err "cannot map this machine to a CEF build (uname: $os/$arch). Use --platform <name>."
    fi
    case "$os-$arch" in
        linux-x64)     echo linux64 ;;
        linux-arm64)   echo linuxarm64 ;;
        macos-x64)     echo macosx64 ;;
        macos-arm64)   echo macosarm64 ;;
        windows-x64)   echo windows64 ;;
        windows-arm64) echo windowsarm64 ;;
        *) err "no CEF build for $os-$arch. Use --platform <name>." ;;
    esac
}

print_detected() {
    local os arch
    os="$(uname -s 2>/dev/null || echo unknown)"
    arch="$(uname -m 2>/dev/null || echo unknown)"
    log "Detected OS:   $os"
    log "Detected arch: $arch"
    log "CEF platform:  $PLATFORM"
}

# ------------------------------------------------------------- index query ---
# Writes:  VERSION<TAB>FILENAME<TAB>SHA1<TAB>SIZE   for the wanted build.
query_index() {
    [ -n "$PYTHON" ] || err "python3 (or python) is required to read the CEF build index"
    CEF_INDEX_URL="$INDEX_URL" CEF_PLATFORM="$PLATFORM" CEF_WANT="$WANT_VERSION" CEF_TYPE="$DISTRO_TYPE" \
    py - <<'PY'
import json, os, sys, urllib.request

url, plat, want, dtype = (os.environ[k] for k in
                          ("CEF_INDEX_URL", "CEF_PLATFORM", "CEF_WANT", "CEF_TYPE"))
try:
    with urllib.request.urlopen(url, timeout=120) as r:
        data = json.load(r)
except Exception as e:
    sys.exit(f"could not read {url}: {e}")

if plat not in data:
    sys.exit(f"unknown platform {plat!r}; known: {', '.join(sorted(data))}")

vers = data[plat].get("versions", [])
stable = [v for v in vers if v.get("channel") == "stable"]
if not stable:
    sys.exit(f"the index reports no stable builds for {plat}")

if want:                                    # pin a specific build
    hits = [v for v in vers if v.get("cef_version") == want]
    if not hits:
        sys.exit(f"version {want} is not in the index for {plat}")
    entry = hits[0]
else:                                       # newest stable (index is newest-first)
    entry = stable[0]

files = [f for f in entry.get("files", []) if f.get("type") == dtype]
if not files:
    sys.exit(f"build {entry.get('cef_version')} has no {dtype!r} distribution for {plat}")
f = files[0]
print(f"{entry['cef_version']}\t{f['name']}\t{f.get('sha1','')}\t{f.get('size',0)}")
PY
}

current_version() {
    if [ -f "$CEF_DIR/include/cef_version.h" ]; then
        sed -n 's/^#define CEF_VERSION "\(.*\)".*/\1/p' "$CEF_DIR/include/cef_version.h" | head -1
    fi
}

sha1_of() {
    if command -v sha1sum >/dev/null 2>&1; then sha1sum "$1" | cut -d' ' -f1
    elif command -v shasum   >/dev/null 2>&1; then shasum -a 1 "$1" | cut -d' ' -f1
    else echo ""; fi
}

extract_tarball() {
    local archive="$1" dest="$2"
    if tar -xjf "$archive" -C "$dest" 2>/dev/null; then return 0; fi
    py - "$archive" "$dest" <<'PY'   # bzip2 via Python, for hosts whose tar has no -j
import sys, tarfile
with tarfile.open(sys.argv[1], "r:bz2") as t:
    t.extractall(sys.argv[2])
PY
}

# ------------------------------------------------------------------- args ----
while [ $# -gt 0 ]; do
    case "$1" in
        --platform) PLATFORM="${2:-}"; shift 2 ;;
        --version)  WANT_VERSION="${2:-}"; shift 2 ;;
        --yes|-y)   ASSUME_YES=1; shift ;;
        --check)    CHECK_ONLY=1; shift ;;
        --help|-h)  usage ;;
        *) err "unknown option: $1 (try --help)" ;;
    esac
done

# ------------------------------------------------------------- confirmation --
if [ -z "$PLATFORM" ]; then
    PLATFORM="$(detect_platform)"
    print_detected
    log "Builds known to this index: $KNOWN_PLATFORMS"
    if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
        printf 'Press Enter to install the latest stable CEF for %s, or type another platform name: ' "$PLATFORM"
        read -r answer || answer=""
        case "$answer" in
            "") ;;
            *)
                case " $KNOWN_PLATFORMS " in
                    *" $answer "*) PLATFORM="$answer" ;;
                    *) err "'$answer' is not a known platform. Try one of: $KNOWN_PLATFORMS" ;;
                esac
                log "Using platform: $PLATFORM"
                ;;
        esac
    else
        log "Non-interactive shell: keeping the detected platform."
    fi
else
    case " $KNOWN_PLATFORMS " in
        *" $PLATFORM "*) ;;
        *) err "'$PLATFORM' is not a known platform. Try one of: $KNOWN_PLATFORMS" ;;
    esac
    log "CEF platform:  $PLATFORM (chosen)"
fi

# ------------------------------------------------------------------ resolve ---
CURRENT="$(current_version || true)"
IFS=$'\t' read -r FOUND FILE_NAME FILE_SHA1 FILE_SIZE <<EOF
$(query_index)
EOF
[ -n "${FOUND:-}" ] || err "could not resolve a CEF build from the index"

log ""
log "Latest stable in the index: $FOUND   (distribution: $DISTRO_TYPE)"
log "Currently installed:        ${CURRENT:-nothing at $CEF_DIR}"
log "Download:                   $CDN_BASE/$FILE_NAME"
[ -n "$FILE_SIZE" ] && log "Size:                       $(( FILE_SIZE / 1048576 )) MB"

if [ "$CHECK_ONLY" -eq 1 ]; then
    if [ "$CURRENT" = "$FOUND" ]; then log "Already up to date: nothing to do (--check)."
    else log "--check: it would download and install $FOUND."; fi
    exit 0
fi

if [ "$CURRENT" = "$FOUND" ]; then
    log ""
    log "Already installed: $FOUND. Nothing to download."
    log "To build: cmake -S . -B build && cmake --build build -j"
    exit 0
fi

# ------------------------------------------------------------------ download --
command -v tar >/dev/null 2>&1 || err "tar is required to unpack the distribution"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ARCHIVE="$TMP/$FILE_NAME"

log ""
log "Downloading $FILE_NAME ..."
if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 -o "$ARCHIVE" "$CDN_BASE/$FILE_NAME"
elif command -v wget >/dev/null 2>&1; then
    wget -O "$ARCHIVE" "$CDN_BASE/$FILE_NAME"
else
    err "need curl or wget to download"
fi

if [ -n "$FILE_SHA1" ]; then
    GOT="$(sha1_of "$ARCHIVE")"
    if [ -n "$GOT" ] && [ "$GOT" != "$FILE_SHA1" ]; then
        err "sha1 mismatch for $FILE_NAME (expected $FILE_SHA1, got $GOT). Not installing."
    fi
    [ -n "$GOT" ] && log "sha1 verified: $GOT"
fi

# ------------------------------------------------------------------- unpack ---
log "Unpacking ..."
mkdir -p "$TMP/unpacked"
extract_tarball "$ARCHIVE" "$TMP/unpacked"
INNER="$(dirname "$(find "$TMP/unpacked" -name cef_version.h -path '*/include/*' | head -1)")/.."
INNER="$(cd "$INNER" && pwd)"
[ -f "$INNER/include/cef_version.h" ] || err "unpacked archive has no include/cef_version.h"

INSTALLED="$(sed -n 's/^#define CEF_VERSION "\(.*\)".*/\1/p' "$INNER/include/cef_version.h" | head -1)"

# Swap it in: only after a fully successful download + unpack.
log "Installing into $CEF_DIR ..."
mkdir -p "$(dirname "$CEF_DIR")"
rm -rf "$CEF_DIR"
mv "$INNER" "$CEF_DIR"

log ""
log "Installed CEF version:  $INSTALLED"
log "Installed at:           $CEF_DIR"
log "Chromium version:       ${INSTALLED##*+chromium-}"
log ""
log "Next step — build LucyN:"
log "  cmake -S . -B build && cmake --build build -j"
