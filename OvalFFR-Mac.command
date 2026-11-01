#!/bin/sh
# OvalFFR launcher for macOS. First run: right-click this file > Open (it is unsigned).
cd "$(dirname "$0")" || exit 1
PY=""
for c in python3 python; do
  command -v "$c" >/dev/null 2>&1 || continue
  "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null && { PY="$c"; break; }
done
if [ -z "$PY" ]; then
  echo "Python 3 was not found. Install it from https://www.python.org/downloads/macos/ and run this again."
  printf 'Press Enter to close.'; read -r _; exit 1
fi
if "$PY" -c 'import tkinter' 2>/dev/null; then
  exec "$PY" src/main.py "$@"
fi
echo "tkinter is missing from this Python. Install Python from python.org (it includes tkinter)."
echo "Starting text mode instead."
echo
"$PY" src/main.py --cli "$@"
printf 'Press Enter to close.'; read -r _
