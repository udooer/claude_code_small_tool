@echo off
rem TravelRecall launcher: serves this folder on http://localhost and opens the browser.
rem Opening index.html directly (file://) works too, but some map styles
rem (CARTO / OpenStreetMap) refuse tile requests that come from a local file.
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Serve.ps1"
endlocal
