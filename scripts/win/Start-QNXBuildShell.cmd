@echo off
setlocal

set "REPO_ROOT=%~dp0..\.."
set "CMAKE_MAKE_PROGRAM=C:\ninja-win\ninja.exe"

call "%REPO_ROOT%\libs\JUCE\tools\run_with_vcvars_and_qnx_env.bat" powershell.exe -NoExit -ExecutionPolicy Bypass -Command "Set-Location '%REPO_ROOT%'"
