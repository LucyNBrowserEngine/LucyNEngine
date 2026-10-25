#!/usr/bin/env bash
#
# scripts/update_cef.sh — check third_party/cef/ against the newest stable CEF
# build and, when there is one, install it.
#
#   ./scripts/update_cef.sh            # report both versions, install if newer
#   ./scripts/update_cef.sh --check    # report only, never download
#   ./scripts/update_cef.sh --yes      # update without asking
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CEF_DIR="$REPO_ROOT/third_party/cef"
INSTALL="$REPO_ROOT/scripts/install.sh"
INDEX_URL="https://cef-builds.spotifycdn.com/index.json"

CHECK_ONLY=0
ASSUME_YES=0
PLATFORM=""

log() { printf '%s\n' "$*"; }
err() { printf 'update_cef.sh: error: %s\n' "$*" >&2; exit 1; }

# python3 on Linux/macOS; Windows (Git Bash) usually only has `python`.
PYTHON="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
py()  { [ -n "$PYTHON" ] || err "python3 (or python) is required to read the CEF build index"; "$PYTHON" "$@"; }

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK_ONLY=1; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --platform) PLATFORM="${2:-}"; shift 2 ;;
        --help|-h) usage ;;
        *) err "unknown option: $1 (try --help)" ;;
    esac
done

[ -n "$PYTHON" ] || err "python3 (or python) is required to read the CEF build index"
[ -x "$INSTALL" ] || [ -f "$INSTALL" ] || err "missing $INSTALL"

# --- what is installed now ---------------------------------------------------
if [ -f "$CEF_DIR/include/cef_version.h" ]; then
    CURRENT="$(sed -n 's/^#define CEF_VERSION "\(.*\)".*/\1/p' "$CEF_DIR/include/cef_version.h" | head -1)"
else
    CURRENT=""
fi

# --- what is the newest stable build for this machine ------------------------
if [ -z "$PLATFORM" ]; then
    os="$(uname -s 2>/dev/null || echo unknown)"
    arch="$(uname -m 2>/dev/null || echo unknown)"
    case "$os" in
        Linux) os=linux ;;
        Darwin) os=macos ;;
        MINGW*|MSYS*|CYGWIN*) os=windows ;;
        *) os="" ;;
    esac
    case "$arch" in
        x86_64|amd64|x64) arch=x64 ;;
        aarch64|arm64) arch=arm64 ;;
        *) arch="" ;;
    esac
    if [ -z "$os" ] || [ -z "$arch" ]; then
        err "cannot map this machine to a CEF build. Use --platform <name>."
    fi
    case "$os-$arch" in
        linux-x64)     PLATFORM=linux64 ;;
        linux-arm64)   PLATFORM=linuxarm64 ;;
        macos-x64)     PLATFORM=macosx64 ;;
        macos-arm64)   PLATFORM=macosarm64 ;;
        windows-x64)   PLATFORM=windows64 ;;
        windows-arm64) PLATFORM=windowsarm64 ;;
        *) err "no CEF build for $os-$arch. Use --platform <name>." ;;
    esac
fi

CEF_INDEX_URL="$INDEX_URL" CEF_PLATFORM="$PLATFORM" py - <<'PY' > /tmp/lucyn_cef_latest.$$
import json, os, sys, urllib.request
try:
    with urllib.request.urlopen(os.environ["CEF_INDEX_URL"], timeout=120) as r:
        data = json.load(r)
except Exception as e:
    sys.exit(f"could not read the CEF build index: {e}")
plat = os.environ["CEF_PLATFORM"]
if plat not in data:
    sys.exit(f"unknown platform {plat!r}")
stable = [v for v in data[plat].get("versions", []) if v.get("channel") == "stable"]
if not stable:
    sys.exit(f"no stable CEF builds listed for {plat}")
print(stable[0]["cef_version"])
PY
LATEST="$(cat /tmp/lucyn_cef_latest.$$)"
rm -f /tmp/lucyn_cef_latest.$$
[ -n "$LATEST" ] || err "could not determine the latest stable CEF build"

# --- compare (MAJOR.MINOR.PATCH) ---------------------------------------------
ver_of() { printf '%s' "$1" | sed 's/^\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/'; }
norm() { printf '%s' "$1" | awk -F. '{ printf "%06d.%06d.%06d\n", $1, $2, $3 }'; }

log "CEF index:    $INDEX_URL"
log "Platform:     $PLATFORM"
log "Installed:    ${CURRENT:-nothing at third_party/cef/}"
log "Latest stable: $LATEST"
log ""

if [ -z "$CURRENT" ]; then
    if [ "$CHECK_ONLY" -eq 1 ]; then
        log "--check: CEF is not installed; it would download $LATEST."
        exit 0
    fi
    log "No CEF distribution found — installing $LATEST now."
    exec bash "$INSTALL" --platform "$PLATFORM" --version "$LATEST" $([ "$ASSUME_YES" -eq 1 ] && echo --yes)
fi

if [ "$CURRENT" = "$LATEST" ]; then
    log "Already up to date — nothing to do."
    exit 0
fi

CUR_N="$(norm "$(ver_of "$CURRENT")")"
NEW_N="$(norm "$(ver_of "$LATEST")")"

if [ "$CUR_N" \> "$NEW_N" ]; then
    log "Installed build is NEWER than the index's stable head (custom or pre-release build)."
    log "Not downgrading. Re-run with --version via scripts/install.sh if you really want it."
    exit 0
fi

log "Newer CEF build available: $(ver_of "$CURRENT") -> $(ver_of "$LATEST")"

if [ "$CHECK_ONLY" -eq 1 ]; then
    log "--check: it would download and install $LATEST."
    exit 0
fi

if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ]; then
    printf 'Update third_party/cef/ now? [y/N] '
    read -r answer || answer=""
    case "$answer" in
        y|Y|yes|YES) ;;
        *) log "Aborted — nothing downloaded."; exit 0 ;;
    esac
fi

log "Updating: $CURRENT -> $LATEST"
bash "$INSTALL" --platform "$PLATFORM" --version "$LATEST" --yes

# --- confirm ----------------------------------------------------------------
NOW="$(sed -n 's/^#define CEF_VERSION "\(.*\)".*/\1/p' "$CEF_DIR/include/cef_version.h" 2>/dev/null | head -1 || true)"
log ""
if [ "$NOW" = "$LATEST" ]; then
    log "Done. third_party/cef/ is now $NOW."
else
    err "update finished but third_party/cef/ reports '${NOW:-nothing}' (expected $LATEST)."
fi
log "Rebuild against it: cmake -S . -B build && cmake --build build -j"
