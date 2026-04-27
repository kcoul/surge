@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b %errorlevel%
set "PATH_ORIG=%PATH%"

call "C:\Users\kicoulter\qnx800\qnxsdp-env.bat" || exit /b %errorlevel%
set "MAKEFLAGS="
set "CMAKE_MAKE_PROGRAM=C:\ninja-win\ninja.exe"

powershell.exe -NoExit -ExecutionPolicy Bypass -Command "Set-Location 'C:\qnxbuilds\repos\surge'"
