@echo off
REM 重新编译 shaders/*.vert / *.frag → *.spv
REM 需要 Vulkan SDK 的 glslc；改了着色器源码后跑一次本脚本即可
setlocal
set "GLSLC=C:\VulkanSDK\1.4.350.0\Bin\glslc.exe"
if not exist "%GLSLC%" set "GLSLC=glslc"
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

"%GLSLC%" "%ROOT%\shader.vert" -o "%ROOT%\vert.spv" || exit /b 1
"%GLSLC%" "%ROOT%\shader.frag" -o "%ROOT%\frag.spv" || exit /b 1
echo shaders compiled.
endlocal
