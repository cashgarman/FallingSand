@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "CONFIG=Release"
if /I "%~1"=="Debug" (
    set "CONFIG=Debug"
    shift
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo cmake was not found on PATH. Install CMake 3.28+ and ensure it is available.
    exit /b 1
)

if not exist "build\CMakeCache.txt" (
    echo Configuring CMake ^(Visual Studio 2022, x64^)...
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo CMake configure failed.
        exit /b 1
    )
)

echo Building %CONFIG%...
cmake --build build --config %CONFIG% --parallel
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

set "EXE=build\%CONFIG%\FallingSand.exe"
if not exist "%EXE%" (
    echo Expected executable not found: %EXE%
    exit /b 1
)

echo Starting %EXE%
"%EXE%" %*
exit /b %ERRORLEVEL%