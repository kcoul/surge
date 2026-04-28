@echo off
setlocal

set "REPO_ROOT=%~dp0..\.."
pushd "%REPO_ROOT%" || exit /b 1

call "%REPO_ROOT%\libs\JUCE\tools\run_with_vcvars.bat" %*
set "ERR=%ERRORLEVEL%"

popd
exit /b %ERR%
