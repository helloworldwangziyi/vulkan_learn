@echo off
REM 迷你加载器演示的一键构建：编两个插件 DLL + 宿主 exe
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "GXX=C:\Users\Administrator\Files\qt\Tools\mingw1310_64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"

if not exist "%ROOT%\plugins\out" mkdir "%ROOT%\plugins\out"

echo ==^> build plugin: double_it
"%GXX%" -std=c++17 -shared "%ROOT%\plugins\double_it.cpp" -o "%ROOT%\plugins\out\double_it.dll" || exit /b 1

echo ==^> build plugin: add_ten
"%GXX%" -std=c++17 -shared "%ROOT%\plugins\add_ten.cpp" -o "%ROOT%\plugins\out\add_ten.dll" || exit /b 1

echo ==^> build host
"%GXX%" -std=c++17 -static-libgcc -static-libstdc++ "%ROOT%\host.cpp" -o "%ROOT%\host.exe" || exit /b 1

REM 宿主在运行时从 plugins\ 目录扫描 DLL，把产物放到那
copy /y "%ROOT%\plugins\out\*.dll" "%ROOT%\plugins\" >nul

echo.
echo ==^> run host
cd /d "%ROOT%"
"%ROOT%\host.exe"
endlocal
