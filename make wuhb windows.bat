@echo off
setlocal enabledelayedexpansion
title WUHB Builder

:: ── Config ────────────────────────────────────────────────────────────────────
set APP_NAME=Undertale
set SHORT_NAME=undertale
set AUTHOR=Grayforz2468
set VERSION=1.0.0

set SOURCE_DIR=.\source
set BUILD_DIR=.\build
set OUTPUT_DIR=.\output-wiiu

set ICON=.\resources\wiiu\icon.png
set TV_IMAGE=.\resources\wiiu\tv.png
set DRC_IMAGE=.\resources\wiiu\drc.png

set OUTPUT_WUHB=%OUTPUT_DIR%\%SHORT_NAME%.wuhb
:: ─────────────────────────────────────────────────────────────────────────────

echo ──────────────────────────────────────
echo  Building: %APP_NAME% v%VERSION%
echo ──────────────────────────────────────

:: Check devkitPro env
if "%DEVKITPRO%"=="" (
    echo Error: DEVKITPRO is not set.
    echo Set it in System Environment Variables, e.g:
    echo   DEVKITPRO=C:\devkitPro
    pause
    exit /b 1
)

if "%DEVKITPPC%"=="" (
    set DEVKITPPC=%DEVKITPRO%\devkitPPC
)

:: Add tools to PATH
set PATH=%DEVKITPRO%\tools\bin;%DEVKITPPC%\bin;%PATH%

:: Check required tools
for %%T in (make.exe wuhbtool.exe elf2rpl.exe) do (
    where %%T >nul 2>&1
    if errorlevel 1 (
        echo Error: '%%T' not found.
        echo Install wiiu-dev via devkitPro pacman:
        echo   pacman -S wiiu-dev
        pause
        exit /b 1
    )
)

:: Warn on missing assets
for %%A in ("%ICON%" "%TV_IMAGE%" "%DRC_IMAGE%") do (
    if not exist %%A (
        echo Warning: Asset not found: %%A
    )
)

:: Create dirs
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

:: ── Step 1: Compile ───────────────────────────────────────────────────────────
echo [1/3] Compiling source...
make BUILD="%BUILD_DIR%" SOURCE="%SOURCE_DIR%" --no-print-directory
if errorlevel 1 (
    echo Error: Compilation failed.
    pause
    exit /b 1
)

:: ── Step 2: ELF → RPX ────────────────────────────────────────────────────────
echo [2/3] Converting ELF to RPX...

set ELF_FILE=
for /r "%BUILD_DIR%" %%F in (*.elf) do (
    if "!ELF_FILE!"=="" set ELF_FILE=%%F
)

if "%ELF_FILE%"=="" (
    echo Error: No .elf file found in %BUILD_DIR% after build.
    pause
    exit /b 1
)

:: Strip .elf and add .rpx
set RPX_FILE=%ELF_FILE:.elf=.rpx%

elf2rpl "%ELF_FILE%" "%RPX_FILE%"
if errorlevel 1 (
    echo Error: elf2rpl conversion failed.
    pause
    exit /b 1
)

:: ── Step 3: Package WUHB ─────────────────────────────────────────────────────
echo [3/3] Packaging WUHB...

set WUHB_ARGS="%OUTPUT_WUHB%" "%RPX_FILE%" --name "%APP_NAME%" --short-name "%SHORT_NAME%" --author "%AUTHOR%"

if exist "%ICON%"      set WUHB_ARGS=%WUHB_ARGS% --icon "%ICON%"
if exist "%TV_IMAGE%"  set WUHB_ARGS=%WUHB_ARGS% --tv-image "%TV_IMAGE%"
if exist "%DRC_IMAGE%" set WUHB_ARGS=%WUHB_ARGS% --drc-image "%DRC_IMAGE%"

wuhbtool %WUHB_ARGS%
if errorlevel 1 (
    echo Error: wuhbtool packaging failed.
    pause
    exit /b 1
)

:: ── Done ─────────────────────────────────────────────────────────────────────
echo.
echo Done! Output: %OUTPUT_WUHB%
echo.
echo Copy to SD card:
echo   sd:\wiiu\apps\%SHORT_NAME%\%SHORT_NAME%.wuhb
echo.
pause