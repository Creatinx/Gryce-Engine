@echo off
setlocal EnableDelayedExpansion

echo [Gryce Engine] Build script for Windows

:: ---------------------------------------------------------------------------
:: 1. 检测编译器
:: ---------------------------------------------------------------------------
where gcc >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] gcc not found in PATH.
    echo.
    echo This project is primarily built with MSYS2 UCRT64 MinGW-w64.
    echo Please use one of the following methods:
    echo.
    echo   1. Open MSYS2 UCRT64 terminal and run this script from there:
    echo      pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
    echo      .\build.bat
    echo.
    echo   2. Add C:\msys64\ucrt64\bin to your system PATH, then re-run.
    echo.
    echo   3. Use Visual Studio 2022 Developer Command Prompt and run:
    echo      cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    echo      cmake --build build
    echo.
    exit /b 1
)

for /f "tokens=*" %%i in ('gcc --version 2^>^&1 ^| findstr "gcc"') do (
    echo [OK] Found: %%i
)

:: ---------------------------------------------------------------------------
:: 2. 解析参数
:: ---------------------------------------------------------------------------
if "%1"=="" (
    set CONFIG=Debug
) else (
    set CONFIG=%1
)

echo [Gryce Engine] Build configuration: %CONFIG%

:: ---------------------------------------------------------------------------
:: 3. 配置（如果不存在）
:: ---------------------------------------------------------------------------
if not exist "build\%CONFIG%\build.ninja" (
    echo [Gryce Engine] Configuring with CMake...
    cmake -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG%
    if %errorlevel% neq 0 (
        echo [ERROR] CMake configuration failed.
        exit /b 1
    )
)

:: ---------------------------------------------------------------------------
:: 4. 构建
:: ---------------------------------------------------------------------------
echo [Gryce Engine] Building...
cmake --build build\%CONFIG%
if %errorlevel% neq 0 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [Gryce Engine] Build complete.
echo   Binaries: build\%CONFIG%\bin\%CONFIG%\
