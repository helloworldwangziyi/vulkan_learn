@echo off
REM ===========================================================================
REM  Build and run one lesson.
REM
REM  Usage:
REM    build.cmd 01_instance            build + run
REM    build.cmd 01_instance norebuild  build only
REM
REM  No CMake, no Ninja - just the compiler and the two libraries.
REM  Toolchain: Qt-bundled MinGW g++ 13.1  +  GLFW (built with MinGW, tools\glfw)
REM             +  Vulkan loader (SDK's vulkan-1.lib, linked by full path).
REM  (MSYS2 UCRT64 g++ is currently blocked by Huorong AV - cc1plus exits silently.)
REM
REM  Two non-obvious things, both verified on this machine:
REM    1. Do NOT use mingw64's g++. Its CRT clashes with MSVC-built DLLs and
REM       the link dies with undefined __acrt_iob_func / __p__fmode.
REM    2. Pass vulkan-1.lib by FULL PATH. "-L C:\Windows\System32 -lvulkan-1"
REM       fails (ld returns 5), but handing ld the .lib directly works.
REM       MinGW's BFD does read MSVC import libraries fine.
REM ===========================================================================
setlocal

set "LESSON=%~1"
if "%LESSON%"=="" (
    echo Usage: build.cmd LESSONNAME [norebuild]
    echo.
    echo Available lessons:
    for %%F in ("%~dp0lessons\*.cpp") do echo     %%~nF
    exit /b 1
)

set "NORUN=0"
if /I "%~2"=="norebuild" set "NORUN=1"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"

set "UCRT=C:\Users\Administrator\Files\qt\Tools\mingw1310_64\bin"
set "GXX=%UCRT%\g++.exe"
if not exist "%GXX%" set "GXX=g++"

set "QTBIN=%UCRT%"
set "GLFW=%ROOT%\tools\glfw"

if not defined VULKAN_SDK (
    for /d %%D in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%D"
)
if not defined VULKAN_SDK (
    echo [error] Vulkan SDK not found
    exit /b 1
)

if not exist "%ROOT%\lessons\%LESSON%.cpp" (
    echo [error] lesson not found: %ROOT%\lessons\%LESSON%.cpp
    exit /b 1
)
if not exist "%GLFW%\lib\libglfw3.a" (
    echo [error] GLFW library missing: %GLFW%\lib\libglfw3.a
    echo         run tools\glfw\rebuild.cmd to build it from source
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

echo ==^> compile %LESSON%
"%GXX%" -std=c++17 -static-libgcc -static-libstdc++ ^
    -I"%GLFW%\include" ^
    -I"%VULKAN_SDK%\Include" ^
    "%ROOT%\lessons\%LESSON%.cpp" ^
    "%GLFW%\lib\libglfw3.a" ^
    "%VULKAN_SDK%\Lib\vulkan-1.lib" ^
    -lgdi32 -luser32 -lshell32 -lole32 ^
    -o "%BUILD%\%LESSON%.exe"
if errorlevel 1 (
    echo [error] build failed
    exit /b 1
)

for %%F in ("%BUILD%\%LESSON%.exe") do echo     %%~fF  (%%~zF bytes)

if "%NORUN%"=="1" (
    echo ==^> run skipped
    exit /b 0
)

echo ==^> run %LESSON%
"%BUILD%\%LESSON%.exe"
echo     exit code: %ERRORLEVEL%
endlocal
