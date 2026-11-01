@echo off
rem OvalFFR launcher for Windows. Double-click it.
setlocal
cd /d "%~dp0"
set "PYX="
where py >nul 2>nul && set "PYX=py -3"
if not defined PYX where python >nul 2>nul && set "PYX=python"
if not defined PYX (
  echo Python 3 was not found.
  echo Install it from https://www.python.org/downloads/ - tick "Add python.exe to PATH" and keep "tcl/tk and IDLE" selected.
  echo Then double-click this file again.
  pause
  exit /b 1
)
%PYX% -c "import tkinter" >nul 2>nul
if errorlevel 1 (
  echo tkinter is missing from this Python. Re-run the Python installer, choose Modify, and tick "tcl/tk and IDLE".
  echo Starting text mode instead.
  echo.
  %PYX% src\main.py --cli
  pause
  exit /b 0
)
where pyw >nul 2>nul && (start "" pyw -3 src\main.py & exit /b 0)
where pythonw >nul 2>nul && (start "" pythonw src\main.py & exit /b 0)
start "" %PYX% src\main.py
