# 资源与延伸阅读

## 一手资料

| 资源 | 说明 |
|---|---|
| [Vulkan 规范](https://docs.vulkan.org/spec/latest/) | 终极参考。每个校验层报错末尾都附规范链接，读 VUID 对应的段落是进阶必经之路 |
| [vulkan-tutorial.com](https://vulkan-tutorial.com) | 本教程课程结构的来源。英文原版，含在线代码对比 |
| [Vulkan SDK](https://vulkan.lunarg.com/) | 含 glslc（着色器编译）、校验层、Vulkan Configurator（可视化开关层与消息级别） |
| [GLFW 文档](https://www.glfw.org/docs/latest/) | 窗口/输入/上下文 API 参考 |

## 着色器与 GLSL

| 资源 | 说明 |
|---|---|
| [GLSL 参考](https://registry.khronos.org/OpenGL-Refpages/gl4/) | Khronos 官方 GLSL 4.x 手册，写 `.vert/.frag` 时查内置变量（如 `gl_VertexIndex`） |
| `glslc --help` | 命令行编译选项；`-O` 优化级别、`-g` 调试信息 |

## 进阶方向

完成全部 22 课后，可以探索的方向：

- **渲染器实战**：[SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan)（数百个特性示例）、[tinyrenderer](https://github.com/ssloy/tinyrenderer)（软件渲染，理解光栅化本质）
- **引擎视角**：UE 源码的 RHI 层（`Engine/Source/Runtime/RHI` + `VulkanRHI`）——你在本教程写的每一个对象，在那里都有工业级版本
- **同步进阶**：Vulkan 的 semaphore/fence 只是起点，event、timeline semaphore、subpass 依赖的精确用法见规范第 7 章
- **计算管线**：Vulkan 不只是图形 API——同样的队列和命令缓冲可以跑通用计算（CUDA 的平替思路）

## 工具

| 工具 | 用途 |
|---|---|
| [RenderDoc](https://renderdoc.org/) | 帧捕获与逐命令调试——Vulkan 开发的"显微镜"，支持查看每次 draw 的输入输出 |
| Vulkan Configurator（SDK 自带） | 图形界面开关校验层、过滤消息级别，比环境变量友好 |
| [VK_LAYER_AMD_switchable_graphics 等日志](https://vulkan.lunarg.com/doc/sdk/) | 启动时的 `[vulkan]` 日志来自加载器，可在排查 ICD/层问题时提供线索 |

## 关于本教程仓库

- 每课源码：`lessons/NN_xxx.cpp`（全中文注释）
- 构建说明：[环境与构建](setup.md)
- 踩坑速查：[踩坑记录总表](pitfalls.md)

> **回到**：[目录](README.md)
