@echo off
setlocal

set "REPO_ROOT=%~dp0..\.."
pushd "%REPO_ROOT%" || exit /b 1

if not defined CMAKE_MAKE_PROGRAM set "CMAKE_MAKE_PROGRAM=C:\ninja-win\ninja.exe"

if "%~1"=="" (
  echo Usage: Run-QNXBuild.cmd ^<command^>
  popd
  exit /b 1
)

call "%REPO_ROOT%\libs\JUCE\tools\run_with_vcvars_and_qnx_env.bat" %*
set "ERR=%ERRORLEVEL%"

popd
exit /b %ERR%
