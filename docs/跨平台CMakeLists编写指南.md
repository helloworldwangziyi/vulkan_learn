# 跨平台 CMakeLists.txt 编写指南

> 以本仓库 `CMakeLists.txt` 为完整案例。这份指南面向"已经会写单平台 CMake、
> 要写三平台（Windows / Linux / macOS）都能跑的构建"的场景。
> 阅读建议：先通读第 1~3 节建立心智模型，后面各节按需查阅。

## 目录

1. [为什么跨平台构建难](#1-为什么跨平台构建难)
2. [心智模型：configure / generate / build](#2-心智模型configure--generate--build)
3. [CMakePresets.json：把命令行固化下来](#3-cmakepresetsjson把命令行固化下来)
4. [编译器与工具链](#4-编译器与工具链)
5. [依赖查找：find_package 与手动探测](#5-依赖查找find_package-与手动探测)
6. [目标定义与 target_* 系列](#6-目标定义与-target_-系列)
7. [编译选项的分平台写法](#7-编译选项的分平台写法)
8. [平台库与平台源码](#8-平台库与平台源码)
9. [自定义命令：着色器、资源拷贝、DLL](#9-自定义命令着色器资源拷贝dll)
10. [构建类型与多配置生成器](#10-构建类型与多配置生成器)
11. [常见坑清单（本项目实战记录）](#11-常见坑清单本项目实战记录)
12. [诊断技巧](#12-诊断技巧)
13. [进阶方向](#13-进阶方向)
14. [交付前检查清单](#14-交付前检查清单)

---

## 1. 为什么跨平台构建难

同一份 C++ 代码，三个平台的差异落在五个维度上：

| 维度 | Windows | Linux | macOS |
|---|---|---|---|
| 默认编译器 | MSVC（cl.exe）或 MinGW（g++） | GCC / Clang | AppleClang |
| 警告/编码选项 | `/W4` `/utf-8` | `-Wall -Wextra` | 同 Linux |
| 库文件形态 | `.lib`（导入/静态）+ `.dll` | `.so` / `.a` | `.dylib` / `.a` / framework |
| 系统依赖 | gdi32/user32 等 | libX11、dl、pthread | Cocoa 等 framework（还要 OBJC） |
| Vulkan 布局 | SDK 的 `Include/` + `Lib/vulkan-1.lib` | 系统 `/usr` 或 SDK 的 `include/lib` | SDK 的 `macOS/` 子目录，底层 MoltenVK |

CMake 的价值就在于：**用一份声明式脚本，把这五个维度的差异集中收编到少数
几个 `if(WIN32) / if(APPLE)` 分支里**，让 95% 的脚本保持平台无关。

写跨平台 CMake 的第一原则：

> **差异显式化，公共部分平台无关化。** 不要为每个平台复制一份脚本再小改，
> 而是写一份脚本，只在真正不同的地方分支。

---

## 2. 心智模型：configure / generate / build

CMake 一次构建分三步，理解这三步能避开 80% 的困惑：

1. **configure**：执行 `CMakeLists.txt`，求值所有变量、`find_package`、
   `if` 分支，结果缓存进 `build/CMakeCache.txt`。
   —— 改 CMakeLists、新增被 GLOB 匹配的文件、换编译器，都要重新 configure。
2. **generate**：生成具体构建系统的文件（`build.ninja`、Makefile、`.sln`）。
3. **build**：调用 ninja/make/msbuild 真正编译。只有这步产生 .obj/.exe。

对应命令（本项目）：

```bash
cmake --preset default          # configure + generate（读 CMakePresets.json）
cmake --build build             # build（或 cmake --build --preset default）
```

**GLOB 的经典坑**：`file(GLOB LESSON_SOURCES "lessons/*.cpp")` 在 configure
时展开一次。新增一个 .cpp 后直接 `cmake --build` 是看不到它的——必须重新
configure。这就是本项目注释里"新增一课要重新 configure"的由来。
（可选缓解：`file(GLOB ... CONFIGURE_DEPENDS ...)` 让构建系统每次检查，
代价是每次构建多一次目录扫描。）

---

## 3. CMakePresets.json：把命令行固化下来

Presets 解决"每个开发者敲的 cmake 命令不一样"的问题。本项目的
`CMakePresets.json` 是一个标准模板，逐字段看：

```jsonc
{
  "version": 3,                        // presets 格式版本，3 需要 CMake ≥ 3.21
  "configurePresets": [
    {
      "name": "default",               // cmake --preset default
      "displayName": "MinGW 13.1 (Qt) + Ninja [Windows]",
      "condition": {                   // 只有 Windows 主机上这个 preset 才可选
        "type": "equals",
        "lhs": "${hostSystemName}",
        "rhs": "Windows"
      },
      "generator": "Ninja",            // 用 Ninja 而不是默认的 Visual Studio
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {              // 等价于 -DKEY=VALUE
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_C_COMPILER": "C:/.../mingw1310_64/bin/gcc.exe",
        "CMAKE_CXX_COMPILER": "C:/.../mingw1310_64/bin/g++.exe",
        "CMAKE_MAKE_PROGRAM": "C:/.../ninja.exe",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    {
      "name": "debug",
      "inherits": "default",           // 继承所有字段，只覆盖下面两个
      "binaryDir": "${sourceDir}/build-debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    }
  ],
  "buildPresets": [
    { "name": "default", "configurePreset": "default" }
  ]
}
```

要点：

- **condition 按主机系统过滤**：Windows 上不会看到 linux/macos preset，
  VS Code 的 CMake Tools 也认这个过滤。
- **inherits 组合配置**：debug 版只覆盖 `binaryDir` + `CMAKE_BUILD_TYPE`，
  避免复制粘贴编译器路径。
- **编译器只能在首次 configure 时指定**。改 `CMAKE_CXX_COMPILER` 后必须
  删掉构建目录重来（CMakeCache.txt 已固化了旧编译器），否则报错或静默无效。
- **`CMAKE_EXPORT_COMPILE_COMMANDS=ON`** 生成 `compile_commands.json`，
  clangd / VS Code IntelliSense / 静态分析工具都靠它，建议常驻。
- preset 里写绝对路径是个人机器的做法；要分发给别人，改用环境变量
  （`"$env{VULKAN_SDK}/..."`）或 toolchain file（见第 13 节）。

---

## 4. 编译器与工具链

### 4.1 四种常见编译器在 CMake 里的身份

| 编译器 | `CMAKE_CXX_COMPILER_ID` | 典型场景 |
|---|---|---|
| MSVC | `MSVC` | Visual Studio 工具链 |
| MinGW g++ | `GNU` | Windows 上的 gcc 移植（注意 ID 是 GNU 不是 MSVC！） |
| Clang | `Clang` / `AppleClang` | Linux / macOS |
| clang-cl | `Clang`（且 `MSVC` 变量为真） | Windows 上用 clang 驱动 MSVC ABI |

判断写法：

```cmake
if(MSVC)                          # MSVC 或 clang-cl（最常用，推荐）
    target_compile_options(app PRIVATE /W4 /utf-8)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(app PRIVATE -Wall -Wextra)
endif()
```

**MinGW 的关键认知**：它是"Windows 平台的 GNU 工具链"——
`WIN32` 为真（链 Windows 系统库、吃 `.dll`），但编译器方言是 GCC
（吃 `-Wall`，不吃 `/W4`）。所以分支条件要分清"我在哪个 OS"（`WIN32`）
和"我是什么编译器"（`MSVC` / `GNU`）这两件事。

### 4.2 指定编译器的三种方式（优先级从高到低）

1. preset / 命令行 `-DCMAKE_CXX_COMPILER=...`（本项目方式，首次 configure 生效）
2. 环境变量 `CXX`（仅首次 configure 且无显式指定时）
3. toolchain file `-DCMAKE_TOOLCHAIN_FILE=xxx.cmake`（交叉编译的正规做法）

### 4.3 MinGW 的两个高频坑

- **产物依赖 libstdc++ 的 dll**：Qt/MSYS2 的 g++ 默认动态链接 C++ 运行时，
  exe 拷到没有这些 dll 的环境就起不来。解法（本项目 CMakeLists 有）：

  ```cmake
  target_compile_options(app PRIVATE -static-libgcc -static-libstdc++)
  # 注意：这是"编译选项"形式传递，等价地也可放 target_link_options
  ```

- **MinGW 的 ld 不能混用 MSVC 编的 .lib**：MSVC 静态库的 .obj 依赖
  `__GSHandlerCheck`、`_stdio_common_vsprintf` 等 MinGW CRT 没有的符号，
  链接报 `corrupt .drectve` / ld 返回 5。**规则：MinGW 链 MinGW 编的库，
  MSVC 链 MSVC 编的库**。本项目 GLFW 因此从源码用 MinGW 自编了一份
  （`tools/glfw/rebuild.cmd`）。

---

## 5. 依赖查找：find_package 与手动探测

### 5.1 find_package 的两种模式

```cmake
find_package(Vulkan QUIET)   # QUIET: 找不到不报错，自己处理 fallback
```

- **Module 模式**：用 CMake 自带的 `FindVulkan.cmake` 等模块脚本搜索。
  成功时给变量（`Vulkan_FOUND`、`Vulkan_LIBRARY`、`Vulkan_INCLUDE_DIRS`）
  和现代用法所需的 **Imported Target**：`Vulkan::Vulkan`。
- **Config 模式**：用库自己安装的 `XxxConfig.cmake`（vcpkg/conan/官方
  安装包提供）。`find_package(Xxx CONFIG)` 可强制只用这种模式。

**优先链接 Imported Target**（`Vulkan::Vulkan`）而不是手填 include/lib
路径——target 自带 include 目录、编译定义、传递依赖，是 CMake 的现代用法。

### 5.2 三级 fallback 模式（本项目 Vulkan 查找的实际结构）

```
find_package(Vulkan QUIET)                    # ① CMake 模块
  ↓ Vulkan_FOUND 为假
手动探测候选目录                               # ② 已知布局硬猜
  Windows: $VULKAN_SDK、C:/VulkanSDK/*（取最新版本号）
  macOS  : /usr/local、/opt/homebrew（Homebrew MoltenVK）
  Linux  : /usr
  判据: <根>/[Include|include|macOS/include]/vulkan/vulkan.h 存在
        且 <根>/[Lib|lib|macOS/lib]/{vulkan-1.lib,libvulkan.so,...} 存在
  ↓ 仍未找到
Linux 兜底 find_library / find_path            # ③ 走系统默认搜索路径
  ↓ 仍未找到
message(FATAL_ERROR "安装指引...")              # 给出可操作的报错
```

这套结构的通用价值：**报错信息里写安装指引**，让换台机器的人照着做就能修，
比一行 `Vulkan not found` 友好得多。

### 5.3 GLFW 的三种获取策略（本项目全用到了）

| 平台 | 策略 | 原因 |
|---|---|---|
| Windows | 预编译 `tools/glfw/lib/libglfw3.a`（MinGW 自编） | 官方预编译的是 MSVC 库，MinGW 链不上（见 4.3） |
| Linux | `tools/glfw/src` 源码现编 + `find_package(X11)` | 无预编译可用；现编免去依赖包管理器版本 |
| macOS | 同上，`_GLFW_COCOA` + 链接 Cocoa framework | 同上，且需要 `enable_language(OBJC)` |

源码现编第三方库的关键：源文件清单和平台宏（`_GLFW_X11` / `_GLFW_COCOA`）
照抄上游官方 `src/CMakeLists.txt`，不要自己猜——少一个源文件就是链接期的
undefined reference。

---

## 6. 目标定义与 target_* 系列

现代 CMake 的基本单位是 **target**（exe/lib），一切配置挂在 target 上：

```cmake
add_executable(app main.cpp)

target_include_directories(app PRIVATE third_party/include)
target_link_libraries(app      PRIVATE Vulkan::Vulkan glfw3)
target_compile_options(app     PRIVATE -Wall)
target_compile_definitions(app PRIVATE APP_VERSION=2)
```

**作用域关键字**：

- `PRIVATE`：只影响本 target（exe 永远用这个）
- `PUBLIC`：本 target + 链接我的人（库的公共头文件目录）
- `INTERFACE`：只影响链接我的人（header-only 库用这个）

**淘汰写法**（老式全局污染，不要再用）：

```cmake
include_directories(...)   # ✗ 全局，所有 target 都被迫带上
add_definitions(...)       # ✗ 同上
link_libraries(...)        # ✗ 同上
```

本项目因为每课一个 exe、数量多，把公共配置放在一个 `foreach` 循环里统一挂，
是"20+ 个同类 target"场景下的合理折中；如果是正经工程项目，应该把公共
配置收进一个 `INTERFACE` 库：

```cmake
add_library(project_common INTERFACE)
target_include_directories(project_common INTERFACE third_party/include)
target_link_libraries(project_common INTERFACE Vulkan::Vulkan)
target_link_libraries(app PRIVATE project_common)   # 一处链接，全部继承
```

---

## 7. 编译选项的分平台写法

### 7.1 警告等级对照

| 目的 | MSVC | GCC/Clang |
|---|---|---|
| 常规警告 | `/W4`（`/W3` 是历史默认） | `-Wall -Wextra` |
| UTF-8 源文件 | `/utf-8`（中文注释必备） | 默认就是 UTF-8 |
| 警告即错误 | `/WX` | `-Werror`（学习项目慎用） |

### 7.2 生成器表达式：比 if 更细粒度

`if(MSVC)` 是 configure 期判断；生成器表达式 `$<...>` 延迟到 generate 期，
能做到"按编译器/按配置"精确下发：

```cmake
target_compile_options(app PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    $<$<CONFIG:Debug>:-O0 -g>            # 仅 Debug 配置生效
    $<$<CONFIG:Release>:-O2>
)
target_compile_definitions(app PRIVATE
    $<$<CONFIG:Debug>:ENABLE_VALIDATION>  # 代码里 #ifdef ENABLE_VALIDATION
)
```

教学项目里 `if(MSVC)` 更易读，工程项目推荐生成器表达式——它能兼容
多配置生成器（见第 10 节），`if(CMAKE_BUILD_TYPE ...)` 那种写法在
Visual Studio 生成器下是错的。

---

## 8. 平台库与平台源码

### 8.1 平台判断变量

```cmake
if(WIN32)   ...   # Windows（含 MinGW）
if(APPLE)   ...   # macOS / iOS
if(UNIX)    ...   # Linux + macOS（注意 macOS 也是 UNIX！）
if(UNIX AND NOT APPLE)  # 纯 Linux 的标准写法
```

### 8.2 各平台要链的系统库（GLFW 的真实需求）

```cmake
if(WIN32)
    target_link_libraries(app PRIVATE gdi32 user32 shell32 ole32)
elseif(APPLE)
    enable_language(OBJC)   # GLFW 的 cocoa_*.m 需要
    target_link_libraries(app PRIVATE
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation")
else()
    find_package(X11 REQUIRED)
    target_link_libraries(app PRIVATE ${X11_LIBRARIES} dl pthread m)
endif()
```

- Windows：窗口系统调用分散在若干系统 dll，链**导入库名**即可，CMake 知道
  去哪找。
- Linux：GLFW 3.3+ 把 X11 扩展库改成运行期 dlopen，所以链接期只需
  libX11 本体 + `dl`（dlopen 所在）+ `pthread` + `m`。
- macOS：framework 用 `-framework Xxx` 形式的链接标志传递。

### 8.3 平台专用源文件

按平台往 target 里加源文件，而不是在代码里 `#ifdef` 整文件：

```cmake
target_sources(app PRIVATE src/common.cpp)
if(WIN32)
    target_sources(app PRIVATE src/platform_win32.cpp)
elseif(APPLE)
    target_sources(app PRIVATE src/platform_cocoa.mm)  # .mm = ObjC++
else()
    target_sources(app PRIVATE src/platform_x11.cpp)
endif()
```

---

## 9. 自定义命令：着色器、资源拷贝、DLL

### 9.1 把着色器编译纳入构建（本项目的写法）

```cmake
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vert.spv"  # 产物
    COMMAND "${GLSLC_EXECUTABLE}" shader.vert -o vert.spv   # 怎么产
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/shader.vert" # 依赖：变了才重跑
    COMMENT "glslc shader.vert → vert.spv"                   # 构建时打印
    VERBATIM)                                                # 参数不做 shell 展开
add_custom_target(shaders ALL DEPENDS <所有 spv 产物>)
```

关键点：

- **`OUTPUT` + `DEPENDS` 构成增量关系**：只改 .cpp 不会重编着色器；
  改了 .vert 才会重新调 glslc。
- **`VERBATIM` 永远要写**：否则路径里的空格、分号会被错误转义。
- custom command 必须被某个 target 依赖（这里收进 `add_custom_target
  (shaders ALL ...)`，`ALL` 表示默认构建目标之一），孤儿 command 不会执行。
- 工具可能不存在（没装 SDK）：用 `if(GLSLC_EXECUTABLE)` 整体跳过，
  并 `message(STATUS ...)` 告知——**降级可用**比**硬报错**更适合学习环境。

### 9.2 POST_BUILD：构建后动作

```cmake
add_custom_command(TARGET app POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${VULKAN_RUNTIME_DLL}" "$<TARGET_FILE_DIR:app>/vulkan-1.dll"
    VERBATIM)
```

- `POST_BUILD` 挂在 target 上，每次该 target 重新链接后执行。
- **`cmake -E` 子命令是跨平台脚本能力的来源**：`copy_if_different`、
  `copy_directory`、`make_directory`、`env` 等，代替 cp/mkdir 这类
  平台命令。
- `$<TARGET_FILE_DIR:app>` 这类生成器表达式避免硬编码输出路径
  （多配置生成器下 exe 在 `build/Release/` 子目录里）。

### 9.3 运行期资源（纹理、shader）放哪

本项目的约定：源目录的 `shaders/`、`textures/` 原样拷到构建目录旁边，
代码里用**相对路径**读（`readFile("shaders/15/vert.spv")`），运行时 CWD
= 构建目录。优点：简单、双击 exe 可运行。缺点：发布时要带上资源目录。

工程化做法（超纲，了解即可）：`$<TARGET_FILE_DIR>` + 安装规则把资源
装进包；或用 `xxd -i`/CMake `configure_file` 把小资源嵌进二进制。

---

## 10. 构建类型与多配置生成器

**单配置生成器**（Ninja、Makefile）：一个构建目录一种配置，
`CMAKE_BUILD_TYPE` 在 configure 时定死。想切 Debug？换目录（本项目
`build/` = Release、`build-debug/` = Debug 的双 preset 就是标准解法）。

**多配置生成器**（Visual Studio、Xcode、Ninja Multi-Config）：一个目录
包含全部配置，构建时才选：

```bash
cmake -G "Ninja Multi-Config" -B build
cmake --build build --config Debug    # 产物在 build/Debug/
```

两条规则：

1. 脚本里**不要读 `CMAKE_BUILD_TYPE` 做分支**（多配置下它为空）；
   用生成器表达式 `$<CONFIG:Debug>`。
2. 脚本开头给单配置生成器设默认值（本项目做法）：

```cmake
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()
```

各配置的含义：`Debug`（-O0 -g）、`Release`（-O3/-O2）、
`RelWithDebInfo`（优化+调试信息，性能分析用）、`MinSizeRel`（最小体积）。

---

## 11. 常见坑清单（本项目实战记录）

按出现频率排序，每条都是真踩过的：

1. **改了 CMakeLists 或新增 GLOB 匹配的文件，构建却看不见**
   → 忘了重新 configure。GLOB 是 configure 期展开的。
2. **MinGW 链接 MSVC 编的 .lib 报 `corrupt .drectve` / ld 返回 5**
   → 工具链和库必须同源（见 4.3）。本项目 GLFW 因此从源码自编。
3. **MinGW 下 `-L dir -lvulkan-1` 找不到库**
   → 实测 MinGW 的 ld 对 `-l` 名称解析在某些布局下失败；把**导入库全路径**
   （含 `.lib` 扩展名）传给 `target_link_libraries`，CMake 会原样递给
   链接器，不做名称转换。
4. **exe 双击闪退 / 换机器起不来**
   → 运行期依赖没带上：Windows 要 `vulkan-1.dll`（POST_BUILD 拷贝）、
   MinGW 要静态链 libstdc++/libgcc（`-static-libstdc++`）。
5. **杀毒软件拦截编译器**（本项目：火绒拦 MSYS2 的 cc1plus，编译静默
   失败）→ 换工具链或加白名单；症状是 ninja 报奇怪错误但手动跑同样
   命令能过。
6. **MSVC 编译含中文注释的源码报语法错误/乱码**
   → 加 `/utf-8`（让 MSVC 按 UTF-8 读源码），或文件存成带 BOM。
7. **多配置生成器下 `if(CMAKE_BUILD_TYPE STREQUAL "Debug")` 不生效**
   → 多配置下该变量为空，用 `$<CONFIG:Debug>`（见第 10 节）。
8. **路径含空格导致自定义命令失败**
   → 所有路径加引号 `"${VAR}"`，custom command 加 `VERBATIM`。
9. **`find_package` 在 CI/新机器上找不到依赖**
   → 报错信息里写清安装方法；提供 `VULKAN_SDK` 等环境变量的检测分支。
10. **行尾符**：Windows 上 git 自动 CRLF 转换对 .cmd/sh 脚本敏感
    （`LF will be replaced by CRLF` 警告的由来）。`.gitattributes` 里给
    shell 脚本钉死 `eol=lf`。

---

## 12. 诊断技巧

```bash
cmake --preset default --trace-expand --trace-source=CMakeLists.txt
# 逐行跟踪 configure，看变量实际取值

cmake --build build --verbose
# 打印每条编译/链接命令（排查 include 路径、链接顺序问题）

cmake --system-information | less
# 当前主机+工具链的全部检测结果
```

脚本内诊断：

```cmake
message(STATUS "Vulkan SDK  : ${Vulkan_LIBRARY}")   # configure 时打印关键路径
```

`compile_commands.json`（preset 里已开）不只是给 IDE 用的：任何"这个文件
到底用了什么编译参数"的疑问，直接查它，一翻就有。

链接错误的排查顺序：undefined reference → 库没链上（顺序、路径、
名称转换）；`corrupt .drectve` / CRT 符号缺失 → 库和工具链不同源。

---

## 13. 进阶方向

学到中高级阶段可以按需引入，本项目刻意**没用**它们（保持零外部依赖）：

- **依赖管理**：`FetchContent`（CMake 内置，配置期拉源码）/ vcpkg
  （微软的包管理器，配 toolchain file 用）/ CPM.cmake（单文件包管理）。
- **toolchain file**：把编译器、sysroot、平台库收进一个 .cmake，
  `cmake -DCMAKE_TOOLCHAIN_FILE=...`，交叉编译（如 Windows 编 ARM）
  的标准做法。
- **install / export**：`install(TARGETS ...)` + `install(EXPORT ...)` 让你的
  库能被别人的 `find_package` 找到——从"写应用"到"写库"的一步。
- **测试**：`enable_testing()` + `add_test()`，配合 preset 里的
  `testPresets`。
- **加速**：ccache/sccache（编译缓存）、`CMAKE_UNITY_BUILD`（合并编译）。

---

## 14. 交付前检查清单

把 CMakeLists 交给别人（或换台机器）之前过一遍：

- [ ] 三台/三种工具链至少验证过两种（Windows + Linux 是最低覆盖）
- [ ] 全新构建目录从零 configure 通过（删掉 build/ 重跑，防缓存掩盖问题）
- [ ] Debug 和 Release 都能编译
- [ ] 缺依赖时的报错信息包含安装指引
- [ ] 没有硬编码个人机器的绝对路径（或在 preset 里并注明）
- [ ] 所有 target 用 `target_*` 配置，无全局 `include_directories`
- [ ] 自定义命令都有 `VERBATIM`、路径都加引号
- [ ] `compile_commands.json` 已生成，IDE 代码导航正常
- [ ] 运行期依赖（dll、资源）在产物目录就位，双击 exe 可运行
