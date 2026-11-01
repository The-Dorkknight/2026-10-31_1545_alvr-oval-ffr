#!/bin/sh
# OvalFFR launcher for Linux. Double-click it and choose "Run" (or run it from a terminal).
cd "$(dirname "$0")" || exit 1

say() {  # show a message in a terminal if there is one, otherwise in a dialog
  if [ -t 1 ]; then printf '%s\n' "$1"
  elif command -v zenity >/dev/null 2>&1; then zenity --error --title="OvalFFR" --no-markup --text="$1"
  elif command -v kdialog >/dev/null 2>&1; then kdialog --error "$1"
  elif command -v xmessage >/dev/null 2>&1; then xmessage -center "$1"
  fi
}

PY=""; PYTK=""
for c in python3 python3.13 python3.12 python3.11 python3.10 python3.9 python3.8 python; do
  command -v "$c" >/dev/null 2>&1 || continue
  "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null || continue
  [ -z "$PY" ] && PY="$c"
  if "$c" -c 'import tkinter' 2>/dev/null; then PYTK="$c"; break; fi
done

if [ -z "$PY" ]; then
  say "Python 3 was not found. Install it with:  sudo apt install python3 python3-tk   (Fedora/Bazzite: sudo dnf install python3 python3-tkinter)"
  [ -t 1 ] && { printf 'Press Enter to close.'; read -r _; }
  exit 1
fi

if [ -n "$PYTK" ]; then
  exec "$PYTK" src/main.py "$@"
fi

say "The window needs tkinter, which is missing. Install it with:  sudo apt install python3-tk   (Fedora/Bazzite: sudo dnf install python3-tkinter)"
if [ -t 1 ]; then
  printf 'Starting text mode instead.\n\n'
  "$PY" src/main.py --cli "$@"
  printf 'Press Enter to close.'; read -r _
fi
exit 0
