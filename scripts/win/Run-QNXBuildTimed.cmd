@echo off
set "REPO_ROOT=%~dp0..\.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-QNXBuildTimed.ps1" -RepoRoot "%REPO_ROOT%" %*
exit /b %ERRORLEVEL%
