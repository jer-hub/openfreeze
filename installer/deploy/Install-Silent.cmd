@echo off
REM SCCM / Intune / PDQ wrapper — path A (testsigning image).
REM Set OPENFREEZE_PASSWORD in the task sequence or replace below.
setlocal
cd /d "%~dp0\.."
if "%OPENFREEZE_PASSWORD%"=="" set OPENFREEZE_PASSWORD=CHANGE-ME-LAB
"%~dp0..\..\OpenFreeze-Setup.exe" /S -Password "%OPENFREEZE_PASSWORD%" -AllowNonVm -OverlaySizeGB 2
set ERR=%ERRORLEVEL%
echo OpenFreeze Setup exit=%ERR%
exit /b %ERR%
