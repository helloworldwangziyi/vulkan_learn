@echo off
REM One-click build + run for the hand-rolled VkSurfaceKHR experiment.
REM Same toolchain as the main project: Qt MinGW + self-built GLFW + Vulkan SDK.
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "GXX=C:\Users\Administrator\Files\qt\Tools\mingw1310_64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"
set "PROJ=C:\Users\Administrator\Desktop\VulkanLearn"
set "GLFW_INC=%PROJ%\tools\glfw\include"
set "GLFW_LIB=%PROJ%\tools\glfw\lib\libglfw3.a"

REM pick the newest Vulkan SDK under C:\VulkanSDK
set "VK_ROOT=C:\VulkanSDK\1.4.350.0"
if not exist "%VK_ROOT%\Include\vulkan\vulkan.h" (
    for /f "delims=" %%d in ('dir /b /ad /o-n C:\VulkanSDK 2^>nul') do (
        if exist "C:\VulkanSDK\%%d\Include\vulkan\vulkan.h" set "VK_ROOT=C:\VulkanSDK\%%d"
    )
)
set "VK_INC=%VK_ROOT%\Include"
set "VK_LIB=%VK_ROOT%\Lib\vulkan-1.lib"

echo ==^> build win32_surface
"%GXX%" -std=c++17 -g -I"%GLFW_INC%" -I"%VK_INC%" "%ROOT%\win32_surface.cpp" -o "%ROOT%\win32_surface.exe" "%GLFW_LIB%" "%VK_LIB%" -lgdi32 -luser32 -lshell32 -lole32 || exit /b 1

REM runtime dependency
if exist "C:\Windows\System32\vulkan-1.dll" copy /y "C:\Windows\System32\vulkan-1.dll" "%ROOT%\" >nul

echo.
echo ==^> run
cd /d "%ROOT%"
"%ROOT%\win32_surface.exe"
endlocal
