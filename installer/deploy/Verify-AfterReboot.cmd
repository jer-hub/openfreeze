@echo off
REM Post-reboot detection / remediation script for lab fleets.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\Verify-OpenFreeze.ps1"
exit /b %ERRORLEVEL%
