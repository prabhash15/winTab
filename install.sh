#!/usr/bin/env bash
#
# Install winTab for the current user.
#
#   ./install.sh              # set up config, autostart, and run the checks
#   ./install.sh --uninstall  # remove the autostart entry
#
# Nothing here needs root. The only thing that does -- disabling Touchegg --
# is left to you, with the exact command printed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"
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
    say "left $CONFIG_DIR alone; delete it yourself for a clean slate"
    exit 0
fi

say "winTab install"
say "============="
say

# --- python and the libraries it needs ------------------------------------
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    warn "no $PYTHON on PATH"
    exit 1
fi
say "python: $("$PYTHON" -c 'import sys; print(sys.version.split()[0], sys.executable)')"

missing="$("$PYTHON" - <<'PY'
import importlib
needed = {
    "gi": "python3-gi",
    "cairo": "python3-cairo",
    "PIL": "python3-pil",
    "Xlib": "python3-xlib",
    "tomllib": "python3.11 or newer",
}
missing = []
for module, package in needed.items():
    try:
        importlib.import_module(module)
    except ImportError:
        missing.append(f"{module} ({package})")
print(" ".join(missing))
PY
)"
if [ -n "$missing" ]; then
    warn "missing python modules: $missing"
    say "    sudo apt install python3-gi python3-cairo python3-pil python3-xlib gir1.2-gtk-3.0"
    exit 1
fi
say "python modules: all present"

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
Exec=$PYTHON -m wintab
Path=$HERE
Icon=input-touchpad
Terminal=false
StartupNotify=false
X-GNOME-Autostart-enabled=true
EOF
say "autostart: wrote $DESKTOP_FILE"

# --- does anything else want these three fingers? ------------------------
say
say "Checking for other gesture handlers..."
conflicts="$("$PYTHON" - "$HERE" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
from wintab.app import find_conflicts
print("\n".join(find_conflicts()))
PY
)"
if [ -n "$conflicts" ]; then
    printf '%s\n' "$conflicts" | while IFS= read -r line; do warn "$line"; done
    say
    say "winTab will still run, but a three-finger swipe will do both things at"
    say "once until the above is dealt with."
else
    say "  nothing else is competing for the three-finger swipe"
fi

say
say "Try it without installing anything else:"
say "    $PYTHON -m wintab --probe 20     # three fingers on the pad: expect PASS"
say "    $PYTHON -m wintab --dry-run      # print what a swipe would decide"
say "    $PYTHON -m wintab                # the real thing"
say
say "Once you are happy, winTab starts with your session automatically."
