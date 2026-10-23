#!/usr/bin/env bash
#
# Build and install winTab for the current user.
#
#   ./install.sh              # build, install, set up config and autostart
#   ./install.sh --uninstall  # remove the autostart entry again
#
# Nothing here needs root. The only thing that does -- disabling Touchegg -- is
# left to you, with the exact command printed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin/wintab"
BUILD_DIR="$HERE/build"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
CONFIG_DIR="$CONFIG_HOME/wintab"
CONFIG_FILE="$CONFIG_DIR/config.toml"
AUTOSTART_DIR="$CONFIG_HOME/autostart"
DESKTOP_FILE="$AUTOSTART_DIR/wintab.desktop"

say() { printf '%s\n' "$*"; }
warn() { printf '  ! %s\n' "$*"; }

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$DESKTOP_FILE"
    say "removed $DESKTOP_FILE"
    say "left $CONFIG_DIR and $BIN alone; delete them yourself for a clean slate"
    exit 0
fi

say "winTab install"
say "============="
say

# --- build dependencies ----------------------------------------------------
if ! command -v pkg-config >/dev/null 2>&1; then
    warn "no pkg-config on PATH"
    say "    sudo apt install build-essential pkg-config"
    exit 1
fi

missing=""
for module in gtk+-3.0 cairo x11 xext xdamage xcomposite; do
    if ! pkg-config --exists "$module"; then
        missing="$missing $module"
    fi
done
if [ -n "$missing" ]; then
    warn "missing development libraries:$missing"
    say "    sudo apt install build-essential libgtk-3-dev libcairo2-dev libx11-dev libxext-dev libxdamage-dev libxcomposite-dev"
    exit 1
fi
say "libraries: gtk $(pkg-config --modversion gtk+-3.0), cairo $(pkg-config --modversion cairo)"

if ! command -v cmake >/dev/null 2>&1; then
    warn "no cmake on PATH"
    say "    sudo apt install cmake ninja-build"
    exit 1
fi

# --- build ----------------------------------------------------------------
generator=()
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi
say "building: cmake -S \"$HERE\" -B \"$BUILD_DIR\" -DCMAKE_BUILD_TYPE=Release"
cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "${generator[@]}" >/dev/null
cmake --build "$BUILD_DIR" --target wintab --parallel "$(nproc)" >/dev/null
cmake --install "$BUILD_DIR" --prefix "$PREFIX" >/dev/null
if [ ! -x "$BIN" ]; then
    warn "the build finished but $BIN is not executable"
    exit 1
fi
say "binary: $BIN ($("$BIN" --version))"

# --- can we read the touchpad --------------------------------------------
if id -nG | tr ' ' '\n' | grep -qx input; then
    say "input group: yes"
else
    warn "your user is not in the 'input' group, so the touchpad cannot be read"
    say "    sudo usermod -aG input $USER   # then log out and back in"
    say "    (running it under sudo is not needed and not supported)"
fi

# --- config ---------------------------------------------------------------
mkdir -p "$CONFIG_DIR"
if [ -f "$CONFIG_FILE" ]; then
    say "config: keeping your existing $CONFIG_FILE"
else
    cp "$HERE/config.example.toml" "$CONFIG_FILE"
    say "config: wrote $CONFIG_FILE"
fi

# --- autostart ------------------------------------------------------------
mkdir -p "$AUTOSTART_DIR"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=winTab
Comment=Windows 11 style three-finger app switching
Exec="$BIN"
Icon=input-touchpad
Terminal=false
StartupNotify=false
X-GNOME-Autostart-enabled=true
EOF
say "autostart: wrote $DESKTOP_FILE"

# --- does anything else want these three fingers? ------------------------
say
say "Checking for other gesture handlers..."
conflicts="$("$BIN" --check-conflicts)"
if [ -n "$conflicts" ]; then
    printf '%s\n' "$conflicts" | while IFS= read -r line; do warn "$line"; done
    say
    say "winTab will still run, but a three-finger swipe will do both things at"
    say "once until the above is dealt with."
else
    say "  nothing else is competing for the three-finger swipe"
fi

say
say "Try it:"
say "    $BIN --probe 20     # three fingers on the pad: expect PASS"
say "    $BIN --dry-run      # print what a swipe would decide"
say "    $BIN                # the real thing"
say
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) say "note: $PREFIX/bin is not on your PATH; the autostart entry uses the" \
            "full path, but add it if you want to run 'wintab' by name." ;;
esac
say "Once you are happy, winTab starts with your session automatically."
