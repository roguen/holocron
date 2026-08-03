@echo off
setlocal EnableDelayedExpansion
REM ===========================================================================
REM Holocron build.
REM
REM   scripts\build.cmd             build and test
REM   scripts\build.cmd configure   reconfigure first, then build and test
REM   scripts\build.cmd build       build only, no tests
REM
REM WHY THIS FILE EXISTS
REM
REM The build has three ordering constraints that are invisible until one of
REM them is violated, and each has cost a session's time at least once. They are
REM written down in CLAUDE.md, but prose in a document is not a thing you can
REM run, and every fresh start re-derived this by hand.
REM
REM   1. CMake and Ninja must be on PATH BEFORE vcvars64.bat is called.
REM      Appending to %PATH% afterwards on the same command line expands the
REM      PRE-vcvars value and wipes the compiler paths back out.
REM
REM   2. vcvars64.bat OVERWRITES VCPKG_ROOT with Visual Studio's own bundled
REM      vcpkg. Setting it before the call silently resolves the manifest
REM      against the wrong tree. It has to be set AFTER.
REM
REM   3. Ninja lives inside the Build Tools installation, not on PATH.
REM
REM Nothing here is hardcoded to one machine: everything is discovered, and a
REM failure to find something says what was not found rather than failing later
REM as a confusing compiler error.
REM ===========================================================================

set "REPO=%~dp0.."
set "MODE=%~1"
if "%MODE%"=="" set "MODE=default"

REM -- Visual Studio, via vswhere -------------------------------------------
REM
REM vswhere ships with every VS installer since 2017 and is at a fixed path, so
REM it is the supported way to find an installation rather than guessing at
REM version numbers and edition names.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo build: vswhere not found at "%VSWHERE%"
    echo build: install Visual Studio Build Tools with the C++ workload
    exit /b 1
)

set "VSROOT="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
    -property installationPath`) do set "VSROOT=%%i"

if not defined VSROOT (
    echo build: no Visual Studio installation with the C++ toolset was found
    exit /b 1
)

set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo build: found "%VSROOT%" but no vcvars64.bat inside it
    exit /b 1
)

REM -- CMake and Ninja, BEFORE vcvars (constraint 1) ------------------------

set "CMAKE_DIR="
where cmake >nul 2>&1 && set "CMAKE_DIR=."
if not defined CMAKE_DIR if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_DIR=%ProgramFiles%\CMake\bin"
if not defined CMAKE_DIR (
    echo build: cmake not found on PATH or in "%ProgramFiles%\CMake\bin"
    exit /b 1
)

REM Ninja ships inside the VS tree. Searched for rather than assumed, because
REM the path under CommonExtensions has moved between VS versions.
set "NINJA_DIR="
where ninja >nul 2>&1 && set "NINJA_DIR=."
if not defined NINJA_DIR (
    for /f "usebackq tokens=*" %%n in (`dir /b /s "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" 2^>nul`) do set "NINJA_DIR=%%~dpn"
)
if not defined NINJA_DIR (
    echo build: ninja not found on PATH or inside "%VSROOT%"
    exit /b 1
)

if not "%CMAKE_DIR%"=="." set "PATH=%CMAKE_DIR%;%PATH%"
if not "%NINJA_DIR%"=="." set "PATH=%NINJA_DIR%;%PATH%"

REM -- The compiler environment ---------------------------------------------

call "%VCVARS%" >nul
if errorlevel 1 (
    echo build: vcvars64.bat failed
    exit /b 1
)

REM -- vcpkg, AFTER vcvars (constraint 2) -----------------------------------
REM
REM Whatever the caller had is honoured, but it is re-applied here because the
REM call above has just overwritten it with VS's bundled copy.

if not defined HOLOCRON_VCPKG_ROOT set "HOLOCRON_VCPKG_ROOT=%USERPROFILE%\vcpkg"
set "VCPKG_ROOT=%HOLOCRON_VCPKG_ROOT%"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo build: no vcpkg at "%VCPKG_ROOT%"
    echo build: set HOLOCRON_VCPKG_ROOT to a bootstrapped vcpkg, or bootstrap one there
    exit /b 1
)

echo build: vs     %VSROOT%
echo build: vcpkg  %VCPKG_ROOT%

REM -- Configure, build, test -----------------------------------------------

cd /d "%REPO%" || exit /b 1

if /i "%MODE%"=="configure" goto :configure
if not exist "build\windows\CMakeCache.txt" goto :configure
goto :build

:configure
cmake --preset windows || exit /b 1

:build
cmake --build --preset windows || exit /b 1

if /i "%MODE%"=="build" (
    echo build: ok ^(tests skipped^)
    exit /b 0
)

ctest --preset windows --output-on-failure || exit /b 1
echo build: ok
