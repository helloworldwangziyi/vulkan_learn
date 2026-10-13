# 环境与构建

本教程的项目是一个 CMake 工程：**`lessons/` 下每课一个 `.cpp`，每课编译出一个独立可执行文件**。新增课程只需把文件放进 `lessons/` 并重新 configure（CMake 用 GLOB 自动发现新文件）。

## 依赖清单

| 依赖 | 本教程使用版本 | 说明 |
|---|---|---|
| Windows 10/11 | — | 教程在 Windows 上验证 |
| Vulkan SDK | 1.4.350.0 | 安装于 `C:\VulkanSDK\`，含头文件与 `vulkan-1.lib` |
| CMake | 3.x | 任意新版本均可 |
| MinGW g++ | 13.1.0 | Qt 自带；也可换其他 64 位 MinGW |
| GLFW | 3.4 | 仓库自带编译好的 `tools/glfw/lib/libglfw3.a`，无需安装 |
| VS Code | — | 可选，配合 CMake Tools 插件可一键编译 |

## 一次构建

```bash
# 配置（首次或新增 .cpp 后必须重跑，GLOB 才能发现新 target）
cmake --preset debug

# 编译指定课程
cmake --build --preset debug --target 01_instance

# 运行（注意：着色器路径是相对路径，要从项目根目录运行）
./build-debug/01_instance.exe
```

Windows 下仓库还提供了 `build.cmd <文件名>` 作为快捷方式（等价于上面两条命令）。

## 着色器

第 8 课起使用 GLSL 着色器，源码在 `shaders/NN/` 下，SPIR-V 字节码（`.spv`）由 Vulkan SDK 自带的 `glslc` 离线编译：

```bash
shaders/build.cmd    # 编译全部着色器
```

**改了 `.vert` / `.frag` 后必须重跑此脚本**，驱动只认 `.spv`。

## 运行成功的标志

- 校验层静默（无任何 `[vulkan]` 开头的 stderr 输出）＝ 一切健康
- 第 1-10 课的程序会正常启动窗口、打印日志后退出
- 第 11 课起，窗口里会出现渲染结果（先是清屏色，画出三角形后是三角形）

## 常见问题速查

| 现象 | 原因 |
|---|---|
| 链接报 `ld returned 5` | vulkan-1.lib 没按全路径传给链接器（本仓库 CMake 已规避，见 CMakeLists 注释） |
| 运行时打不开 `shaders/xx/vert.spv` | 不在项目根目录运行，或改了着色器没重跑 `build.cmd` |
| 杀毒软件拦截 g++ | 换 Qt 捆绑的 MinGW 可绕过（本仓库即如此） |

> **下一章**：[第 1 课 · 实例与扩展](ch01.md)
