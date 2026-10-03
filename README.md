# Vulkan 学习环境

配套 `文档/Vulkan_Tutorial_CN.pdf`（Alexander Overvoorde《Vulkan 教程》中文版，2025-11）。

**每课就是教程的代码，逐字一致。** 没有封装、没有抽象层、没有自定义类。

## 用法

```cmd
cd C:\Users\Administrator\Desktop\Vulkan学习
build.cmd 01_instance
```

不带参数会列出所有课程。

## 目录

```
Vulkan学习/
  build.cmd            编译+运行一课（就是 g++ 一行命令）
  lessons/             每课一个 .cpp，内容与教程一致
    └ 01_instance.cpp
  tools/glfw/          GLFW 3.4，用 MinGW 自己编译的
    ├ include/GLFW/     官方头文件
    ├ lib/libglfw3.a    静态库
    ├ src/              源码（重编用）
    └ rebuild.cmd       重编脚本
  build/                编译产物
```

## 三个必须知道的点

这些都是在本机实测出来的，出问题时先看这里。

### 0. 项目路径必须全是 ASCII 字符

**不要把本项目放在含中文的目录下**，例如 `Desktop\Vulkan学习` 会失败。

原因：`g++` 的 driver（`collect2`）把路径参数交给 `ld` 时按本地代码页转码，
非 ASCII 字符会被毁掉。实测报错：

```
ld.exe: cannot find C:\Users\Administrator\Desktop\Vulkan??\build\...\01_instance.cpp.obj: Invalid argument
                                    ↑ “学习”变成了 ??
```

症状很有迷惑性，**因为编译阶段是好的**（那个阶段 gcc 自己开文件，
路径不经手 ld）：

| 阶段 | 中文路径下 |
|---|---|
| 编译 `source.cpp` → `.obj` | ✅ 成功 |
| 链接 `.obj` → `.exe` | ❌ `collect2.exe: error: ld returned 1`，且**不给符号错误** |
| CMake configure | ❌ 卡在 `Detecting CXX compiler ABI info` |

注意 CMake 那条：它会**挂起**而不是报错，很容易被误判成沙箱或环境问题。

现用路径 `C:\Users\Administrator\Desktop\VulkanLearn` 已验证全流程通过。
换路径时保持纯英文即可。

### 1. 编译器用 Qt 自带的 MinGW g++

```
C:\Users\Administrator\Files\qt\Tools\mingw1310_64\bin\g++.exe   正确（现用）
C:\Users\Administrator\Files\msys\ucrt64\bin\g++.exe             曾用，现被火绒拦截
C:\Users\Administrator\Files\msys\mingw64\bin\g++.exe            错误
```

2026-10 起，UCRT64 的 g++ 15.1.0 被火绒安全拦截：`cc1plus.exe` 一启动就
静默退出（退出码 1，无任何输出），gcc 驱动跟着报 `ld returned 1` 之类的
假错误。临时改用 Qt 安装目录里捆绑的 MinGW g++ 13.1.0，已验证编译、
链接、运行（含校验层）全部正常。如果你给火绒的"信任区"加了
`C:\Users\Administrator\Files\msys` 白名单，可以把 preset 里的编译器
换回 ucrt64 那套。

`mingw64` 那套的 CRT 与 MSVC 构建的 DLL 冲突，链接会失败：

```
multiple definition of `__imp___C_specific_handler'
undefined reference to `__acrt_iob_func'
undefined reference to `__p__fmode'
```

### 2. Vulkan 库要写全路径

```
"%VULKAN_SDK%\Lib\vulkan-1.lib"       正确
-L C:\Windows\System32 -lvulkan-1     失败（ld 返回 5）
```

MinGW 的 BFD 能正常读 MSVC 格式的导入库，但不认 `-L` + `-l` 这种找法。

## 用 VS Code 编译和调试

`.vscode/` 和 `CMakePresets.json` 已配好，装好扩展后打开目录即可。

需要的扩展（`extensions.json` 里已列为推荐，VS Code 会提示安装）：

| 扩展 | 本机状态 |
|---|---|
| **CMake Tools** `ms-vscode.cmake-tools` | 已装 1.24.42 |
| **C/C++** `ms-vscode.cpptools` | 已装 1.34.4 |

### 编译

打开目录后 CMake Tools 会自动 configure。底部状态栏：

```
[default]  [01_instance]  [Build]  [▶ 运行]  [🐞 调试]
   ↑preset      ↑选目标
```

点 **Build** 编译，点 **▶** 运行，点 **🐞** 调试。

命令面板（`Ctrl+Shift+P`）里对应 `CMake: Configure` / `CMake: Build` /
`CMake: Run Without Debugging` / `CMake: Debug`。

### 调试

`launch.json` 已配好 gdb。要用断点必须先构建 **debug** preset
（Release 没有调试符号）：

```powershell
cmake --preset debug
cmake --build --preset debug
```

然后底部状态栏把 configure preset 切成 `debug`，按 `F5`。
`launch.json` 里的 `program` 指向 `build-debug/01_instance.exe`，
换课时改这一行。

### 加课后要重新 configure

`CMakeLists.txt` 用 `file(GLOB ...)` 收集 `lessons/*.cpp`，所以新增文件后
要重新 configure（命令面板 `CMake: Configure`）。改动 `CMakeLists.txt`
本身会自动触发。

### 命令行等价操作

```powershell
cd C:\Users\Administrator\Desktop\Vulkan学习
cmake --preset default
cmake --build --preset default
.\build\01_instance.exe
```

`build.cmd` 仍然可用，两者产物都在 `build/`，可以混用。

## GLFW 为什么要自己编

你 `C:\Users\Administrator\Files\OpenGL\lib\glfw3.lib` 那份是 **MSVC 静态库**，
MinGW 链接不了。实测报错：

```
Warning: corrupt .drectve at end of def file   （重复 21 次）
collect2.exe: error: ld returned 5 exit status
```

原因不是格式或名字改编——`nm` 能读，公开符号也都在（`T glfwInit` 等）。
真正的原因是那些 .obj 依赖 4 个 MinGW CRT 里**不存在**的 MSVC 运行时符号：

| 符号 | 出处 |
|---|---|
| `__GSHandlerCheck` | MSVC `/GS` 栈保护检查，每个函数序言都要调 |
| `_stdio_common_vsprintf` | MSVC CRT 内部 stdio |
| `_stdio_common_vsscanf` | 同上 |
| `_wassert` | MSVC 断言 |

MSVC 默认开 `/GS`，`__GSHandlerCheck` 是硬依赖，MinGW 补不了。

**解决办法**：你 `Downloads` 里有 GLFW 3.4 源码（`glfw-3.4.zip` 及已解压目录），
用 MinGW 重新编译即可。已经编好放在 `tools/glfw/`，21 个源文件全通过。

需要重编（比如想加 `-g` 调试或 ASan）就跑：

```cmd
tools\glfw\rebuild.cmd
```

## 环境现状（已实测）

| 项 | 状态 |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER（驱动 API 1.4.325）+ AMD 核显 |
| Vulkan loader | `C:\Windows\System32\vulkan-1.dll`，1.4.350 |
| Vulkan SDK | `C:\VulkanSDK\1.4.350.0` |
| GLFW | 3.4，MinGW 自编，`tools/glfw/lib/libglfw3.a`（331 KB） |
| 编译器 | Qt 捆绑 MinGW g++ 13.1.0（原 UCRT64 g++ 15.1.0 被火绒拦截） |

验证过的调用链：`glfwInit` → `glfwCreateWindow` →
`glfwGetRequiredInstanceExtensions`（返回 2 个）→ `vkCreateInstance` 返回 0。

## 加课

往 `lessons/` 放一个 `.cpp`，照教程写，然后 `build.cmd 文件名`。

按教程章节顺序，接下来的课程大致是：

| 课 | 内容 |
|---|---|
| 02 | 验证层 + 调试回调 |
| 03 | 物理设备与队列族 |
| 04 | 逻辑设备与队列 |
| 05 | 窗口表面 |
| 06 | 交换链 |
| 07 | 图像视图 |
| 08 | 图形管线（渲染通道 + 管线装配 + SPIR-V 着色器） |
| 09 | 帧缓冲 |
| 10 | 命令缓冲与录制 |
| 11 | 渲染呈现流水线 —— `drawFrame()` 留作练习，自己画出三角形 |
