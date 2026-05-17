#!/usr/bin/env bash
# Jarvis installer - https://github.com/Tehns/Jarvis
# Usage: curl -fsSL https://raw.githubusercontent.com/Tehns/Jarvis/main/install.sh | bash

set -e

REPO="https://github.com/Tehns/Jarvis.git"
TMP="$(mktemp -d)"
BINARY="/usr/local/bin/jarvis"

RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'; CYAN='\033[36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}==>${RESET} $*"; }
success() { echo -e "${GREEN}${BOLD} ✓${RESET} $*"; }
warn()    { echo -e "${YELLOW}  !${RESET} $*"; }
die()     { echo -e "${RED}error:${RESET} $*" >&2; exit 1; }

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo -e "\n${BOLD}  Jarvis Installer${RESET}\n"

# ── Check dependencies ────────────────────────────────────────────────────────
info "Checking dependencies..."

command -v gcc  >/dev/null 2>&1 || die "gcc not found. Install it: sudo pacman -S gcc"
command -v make >/dev/null 2>&1 || die "make not found. Install it: sudo pacman -S make"
command -v git  >/dev/null 2>&1 || die "git not found. Install it: sudo pacman -S git"

# Check for libcurl header
if ! $(gcc -x c - -lcurl -o /dev/null 2>/dev/null <<< 'int main(){}'); then
    # Try to detect the package manager and suggest the right command
    if command -v pacman >/dev/null 2>&1; then
        die "libcurl not found. Install it: sudo pacman -S curl"
    elif command -v apt >/dev/null 2>&1; then
        die "libcurl not found. Install it: sudo apt install libcurl4-openssl-dev"
    elif command -v dnf >/dev/null 2>&1; then
        die "libcurl not found. Install it: sudo dnf install libcurl-devel"
    else
        die "libcurl not found. Install libcurl development headers for your distro."
    fi
fi

success "Dependencies OK"

# ── Remove old version ────────────────────────────────────────────────────────
if [ -f "$BINARY" ]; then
    OLD_VER=$("$BINARY" --version 2>/dev/null || echo "unknown")
    warn "Removing old version ($OLD_VER)..."
    sudo rm -f "$BINARY"
fi

# ── Clone ─────────────────────────────────────────────────────────────────────
info "Cloning Jarvis..."
git clone --depth=1 "$REPO" "$TMP/jarvis" 2>&1 | grep -v "^$" || die "git clone failed"
success "Cloned"

# ── Build ─────────────────────────────────────────────────────────────────────
info "Building..."
cd "$TMP/jarvis"
make 2>&1 | tail -1 || die "Build failed. Run manually to see errors."
success "Built"

# ── Install ───────────────────────────────────────────────────────────────────
info "Installing to $BINARY (needs sudo)..."
sudo install -m 755 jarvis "$BINARY" || die "Install failed"

NEW_VER=$(jarvis --version 2>/dev/null || echo "?")
success "Installed $NEW_VER"

echo -e "\n${GREEN}${BOLD}  Jarvis is ready.${RESET} Run: ${BOLD}jarvis${RESET}\n"
