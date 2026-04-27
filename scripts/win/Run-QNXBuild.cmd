@echo off
setlocal enabledelayedexpansion

call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b %errorlevel%
set "PATH_ORIG=!PATH!"

call "C:\Users\kicoulter\qnx800\qnxsdp-env.bat" || exit /b %errorlevel%
set "MAKEFLAGS="
set "CMAKE_MAKE_PROGRAM=C:\ninja-win\ninja.exe"

if "%~1"=="" (
  echo Usage: Run-QNXBuild.cmd ^<command^>
  exit /b 1
)

cmd /c %*
