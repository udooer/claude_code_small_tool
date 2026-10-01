@echo off
rem PerfCollector launcher. Double-click for the menu, or pass arguments, e.g.
rem   Collect.cmd -ProcessName myapp -Duration 120
rem   Collect.cmd -Trigger -ProcessName myapp -CpuThreshold 30
setlocal
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if "%~1"=="" (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Collect-PerfLogs.ps1" -Interactive
) else (
  "%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Collect-PerfLogs.ps1" %*
)
endlocal
