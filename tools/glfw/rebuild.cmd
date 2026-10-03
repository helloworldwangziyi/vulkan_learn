@echo off
REM ===========================================================================
REM  Rebuild GLFW from source with MinGW and refresh tools\glfw\lib\libglfw3.a
REM
REM  Why this exists: the prebuilt glfw3.lib shipped with most GLFW downloads
REM  is an MSVC static library. MinGW cannot link it - see README for the
REM  linker evidence. So we build GLFW ourselves from tools\glfw\src.
REM
REM  Usage:  tools\glfw\rebuild.cmd
REM
REM  Only needs re-running if you change compiler flags (e.g. want -g or ASan).
REM  Requires the GLFW source in tools\glfw\src (already vendored).
REM ===========================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "SRC=%HERE%\src"
set "OUT=%HERE%\build"
set "LIB=%HERE%\lib\libglfw3.a"

set "UCRT=C:\Users\Administrator\Files\msys\ucrt64\bin"
set "GCC=%UCRT%\gcc.exe"
set "AR=%UCRT%\ar.exe"
if not exist "%GCC%" (
    set "GCC=gcc"
    set "AR=ar"
)

if not exist "%SRC%\window.c" (
    echo [error] GLFW source missing. Expected %SRC%\window.c
    echo         Download glfw-3.4 source and copy its src\ here.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%HERE%\lib" mkdir "%HERE%\lib"

REM Official Win32 build set: the shared core plus the Win32 platform backend
REM and the null backends that GLFW always compiles in.
set FILES=context init input monitor platform vulkan window ^
egl_context osmesa_context wgl_context ^
null_init null_joystick null_monitor null_window ^
win32_init win32_joystick win32_module win32_monitor win32_thread win32_time win32_window

echo ==^> compiling GLFW
set OBJS=
for %%F in (%FILES%) do (
    if not exist "%SRC%\%%F.c" (
        echo [error] missing source: %%F.c
        exit /b 1
    )
    "%GCC%" -c -O2 -D_GLFW_WIN32 -I"%HERE%\include" -I"%SRC%" "%SRC%\%%F.c" -o "%OUT%\%%F.o"
    if errorlevel 1 (
        echo [error] compile failed: %%F.c
        exit /b 1
    )
    set "OBJS=!OBJS! "%OUT%\%%F.o""
)

echo ==^> archiving
if exist "%LIB%" del "%LIB%"
"%AR%" rcs "%LIB%" !OBJS!
if errorlevel 1 (
    echo [error] ar failed
    exit /b 1
)

for %%F in ("%LIB%") do echo     %%~fF  (%%~zF bytes)
echo     done
endlocal
