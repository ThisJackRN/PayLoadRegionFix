@echo off
setlocal
title RegionFix - Build

echo Building RegionFix and PayloadLoaderInstaller - RegionFix...
echo.

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1"
set "buildResult=%ERRORLEVEL%"
echo.
if not "%buildResult%"=="0" goto failed

echo Build succeeded. Output files:
echo   "%~dp0artifacts\regionfix.wuhb"
echo   "%~dp0artifacts\PayloadLoaderInstaller-RegionFix.wuhb"
echo   "%~dp0artifacts\PayloadLoaderInstaller-RegionFix-Diagnostic.wuhb"
goto done

:failed
echo BUILD FAILED. See the error above. Do not use old artifacts as a new build.

:done
if /I not "%~1"=="--no-pause" pause
exit /b %buildResult%
