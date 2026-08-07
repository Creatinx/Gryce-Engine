@echo off
rem E0 build helper (equivalent to build.py --jobs 8)
set PATH=D:\msys64\ucrt64\bin;D:\CMake\bin;C:\env;C:\Windows;C:\Windows\System32
C:\Windows\py.exe build.py --jobs 8 > build\last_build.log 2>&1
if %ERRORLEVEL%==0 (echo BUILD_SCRIPT_OK) else (echo BUILD_SCRIPT_FAIL)
