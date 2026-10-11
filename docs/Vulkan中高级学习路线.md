# Vulkan 中高级学习路线（20 章）

> 前置：已完成 `lessons/` 入门 22 课（实例 → 三角形 → 顶点/索引/staging 缓冲 →
> Uniform Buffer → 纹理 → 深度 → 模型加载 → Mipmap → MSAA），即 vulkan-tutorial.com
> 的全部内容。本路线从这里继续，目标是能独立写一个结构清晰、性能可测、
> 跨平台可跑的小型渲染器。
>
> 目录约定：中高级课程代码在 `advanced/`，每章一个自包含的 `.cpp`（延续入门课
> 手敲单文件的风格），着色器放 `shaders/advanced/NN/`。目前已实现第 01~05 章。

## 阶段总览

| 阶段 | 章节 | 主题 | 一句话目标 |
|---|---|---|---|
| 一、工程化进阶 | 01~05 | 框架、健壮性、数据流 | 从"能跑"到"像工程"：RAII、resize 不崩、三层数据体系、离屏渲染 |
| 二、渲染技术 | 06~10 | compute、阴影、天空盒、延迟渲染 | 掌握现代渲染器的四大基础技术 |
| 三、性能与内存 | 11~15 | 实例化、多线程、VMA、性能分析 | 会测、会找瓶颈、会优化 |
| 四、高级综合 | 16~20 | PBR/IBL、现代同步、平台专题、综合项目 | 产出可当作品集的迷你渲染器 |

预计总投入：每章 4~10 小时，全程约 120~180 小时。

---

## 第一阶段：工程化进阶（01~05，已实现）

### 第 01 章 工程化重构：RAII 与完整资源生命周期

- **目标**：把入门课"创建齐全、清理随缘"的单文件，重构成资源生命周期严密、
  退出零泄漏（校验层无报错）的基座框架。后续所有章节都建立在它之上。
- **核心知识点**：
  - RAII 包装 Vulkan 句柄：move-only、析构释放、禁止拷贝
  - 销毁顺序的硬约束：pipeline/framebuffer → render pass → image view →
    swapchain → command pool → device → surface → instance
  - 双帧并行（MAX_FRAMES_IN_FLIGHT = 2）的 per-frame 同步对象数组
  - `VK_CHECK` 宏统一错误处理
- **关键 API**：无新 API，全是工程组织；`vkWaitForFences` 的正确用法
- **实现要点**：用一个泛型 move-only 包装（或每资源一个小 struct），
  mainLoop 每帧：wait fence → acquire → reset → 录制 → submit → present
- **验收**：`advanced/01_framework.cpp` 运行显示旋转四边形；关闭窗口后
  校验层零错误零警告
- **代码**：`advanced/01_framework.cpp`，着色器 `shaders/advanced/01/`

### 第 02 章 健壮的交换链：resize 重建与窗口事件

- **目标**：窗口任意缩放、最小化、跨屏拖动（DPI 变化）都不崩、不花屏。
- **核心知识点**：
  - `VK_ERROR_OUT_OF_DATE_KHR`（acquire/present 返回）与
    `VK_SUBOPTIMAL_KHR` 的区别与处理策略
  - framebuffer resize 回调只置标志位，重建放帧首做（回调里直接重建会踩坑）
  - 重建时 `oldSwapchain` 传旧句柄（入门课里那个 `VK_NULL_HANDLE` 的坑在这填）
  - 窗口最小化（extent 为 0）时挂起渲染循环而不是崩掉
  - 重建后哪些对象必须重来：image view、framebuffer、（若 render pass
    变了）pipeline
- **关键 API**：`glfwSetFramebufferSizeCallback`、`vkCreateSwapchainKHR` 的
  `oldSwapchain`、`glfwWaitEvents`
- **验收**：拖动窗口边框快速缩放 10 秒无崩溃无校验错误；最小化再恢复画面正常

### 第 03 章 Push Constants 与多物体渲染

- **目标**：一次帧循环画 8 个位置/颜色各异的四边形，每物体数据不走 UBO。
- **核心知识点**：
  - push constant 的本质：直接写在命令缓冲里的小块数据，无需 buffer/描述符
  - 128 字节下限保证（`maxPushConstantsSize`，桌面卡通常 128~256）
  - push constants vs UBO vs SSBO 的适用场景对比（更新频率、大小、数量）
  - 一个 pipeline layout 同时挂 descriptor set 和 push constant range
  - 多个相同物体共享顶点/索引缓冲，逐物体 `vkCmdPushConstants` 后 `vkCmdDrawIndexed`
- **关键 API**：`VkPushConstantRange`、`vkCmdPushConstants`、着色器
  `layout(push_constant) uniform`
- **验收**：8 个 quad 各自旋转/颜色不同；RenderDoc（或代码断言）确认每帧
  只更新一次 UBO，物体数据全走 push constants

### 第 04 章 描述符体系进阶：三层数据流（帧/材质/物体）

- **目标**：建立"按更新频率分层"的描述符体系：set 0 帧级（VP 矩阵）、
  set 1 材质级（纹理+sampler）、物体级继续用第 03 章的 push constants。
- **核心知识点**：
  - 为什么按更新频率分 set：减少每帧描述符重写
  - `VkDescriptorPool` 的 size 估算（每种类型 × 帧数 × 材质数）
  - set 1 分配一次终生不改（材质），set 0 每帧更新（双帧 ×2 份）
  - `VkPipelineLayout` 挂多个 set layout；着色器 `layout(set=..., binding=...)`
  - stb_image 加载纹理、mipmap 生成的回顾（入门 16~18、21 课的串联运用）
- **关键 API**：`vkAllocateDescriptorSets`、`vkUpdateDescriptorSets`、
  `vkCmdBindDescriptorSets`（带 firstSet 参数）
- **验收**：两个不同纹理的材质交替赋给 8 个 quad；校验层无
  descriptor 相关报错；说清楚每帧调用了哪些 `vkUpdate*`

### 第 05 章 离屏渲染与后处理入门

- **目标**：场景先渲染到一张离屏纹理（自带深度附件），第二个 pass 用全屏
  三角形采样它上屏，并做简单后处理。
- **核心知识点**：
  - 自建 color attachment：`VkImage` 用法从 swapchain 的 `COLOR_ATTACHMENT`
    变为 `COLOR_ATTACHMENT | SAMPLED`（一张图两种用法，layout 转换是关键）
  - 离屏 render pass：finalLayout 必须是 `SHADER_READ_ONLY_OPTIMAL`
  - 两条 pipeline：场景管线（画进离屏）+ 后处理管线（全屏三角形，无顶点
    输入，`gl_VertexIndex` 生成三个顶点）
  - subpass dependency 表达"写完了才能读"（`COLOR_ATTACHMENT_OUTPUT` →
    `FRAGMENT_SHADER`，srcAccessMask = `COLOR_ATTACHMENT_WRITE`）
  - 后处理效果用 push constant 切换：原样 / 灰度 / 反色 / 暗角
- **关键 API**：`vkCreateImage`（自建附件）、`VkAttachmentReference`、
  `vkCmdNextSubpass` 或多 render pass 两次 `vkCmdBeginRenderPass`
- **验收**：键盘 1/2/3/4 切换四种后处理效果；离屏分辨率与交换链解耦
  （可以渲一半分辨率再上采样）

---

## 第二阶段：渲染技术（06~10）

### 第 06 章 计算着色器入门

- **目标**：用 compute shader 做图像处理（对纹理做高斯模糊或灰度），
  理解 graphics 之外的第二根管线。
- **核心知识点**：compute pipeline（无 render pass/framebuffer）、
  storage image（`VK_IMAGE_USAGE_STORAGE_BIT`）、`vkCmdDispatch` 与
  workgroup 划分、`VK_ACCESS_SHADER_WRITE → SHADER_READ` 的 barrier、
  compute queue family 可能与 graphics 不同
- **验收**：对 texture.png 做一次 compute 模糊再显示；说清楚 barrier 两端
  的 stage/access 各是什么

### 第 07 章 GPU 粒子系统

- **目标**：上千个粒子完全在 GPU 上更新（compute）并渲染（graphics），
  CPU 每帧只提交命令。
- **核心知识点**：SSBO 存粒子状态、compute→graphics 的帧内同步
  （semaphore 或同队列 barrier）、点精灵/公告板渲染、双缓冲避免读写竞争
- **参考**：Sascha Willems 的 computeparticles 示例
- **验收**：4096+ 粒子 60fps；RenderDoc 抓帧确认 update 在 compute queue 执行

### 第 08 章 阴影映射（Shadow Mapping）

- **目标**：一个模型 + 地面，方向光产生正确阴影，带 PCF 软化。
- **核心知识点**：depth-only render pass（无 color attachment）、光源视锥的
  VP 矩阵、深度附件当纹理采样（`sampler2DShadow`）、shadow acne 与
  depth bias（`depthBiasConstantFactor/SlopeFactor`）、3×3 PCF
- **验收**：阴影边界无严重 acne/自遮挡条纹；bias 参数可调并观察效果差异

### 第 09 章 立方体贴图与天空盒

- **目标**：加载 6 面 cubemap，渲染天空盒，并让模型带环境反射。
- **核心知识点**：`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`、6 layer 的
  image view、天空盒深度技巧（`depthFunc = LEQUAL` + z=w）、`reflect()`
  采样环境、教程 20 课模型加载的复用
- **验收**：天空盒跟随相机旋转不跟随平移；viking_room 表面反射随视角变化

### 第 10 章 延迟渲染与 MRT

- **目标**：G-buffer（position/normal/albedo 三 attachment）+ lighting pass，
  支持几十个点光源。
- **核心知识点**：MRT 多 color attachment、subpass input attachment
  （G-buffer 不落地到纹理采样的优化版）、两种实现（多 render pass vs
  subpass input）的带宽差异、延迟 vs 前向的适用场景
- **验收**：32 个点光源 60fps；能说清 subpass input attachment 为什么省带宽

---

## 第三阶段：性能与内存（11~15）

### 第 11 章 实例化与间接绘制

- **目标**：一次 draw call 渲染 1000 个实例；再用 indirect 把 draw 参数本身
  放到 GPU。
- **核心知识点**：`VK_VERTEX_INPUT_RATE_INSTANCE`、实例属性除数、
  `vkCmdDrawIndexedIndirect`、间接参数的 `VkDrawIndexedIndirectCommand`、
  GPU 视锥剔除（compute 填 indirect buffer）的架构
- **验收**：1000 实例 1 次 draw call（RenderDoc 验证）；indirect 版本 draw
  参数零 CPU 写入

### 第 12 章 多线程命令录制

- **目标**：多线程并行录制 secondary command buffers，主线程合并提交。
- **核心知识点**：每线程独立 `VkCommandPool`（`vkCmd*` 非线程安全）、
  secondary command buffer 的 `VkCommandBufferInheritanceInfo`、
  `vkCmdExecuteCommands`、对象级并行粒度（每线程一批物体）
- **验收**：4 线程录制相比单线程在 CPU 耗时上有可见提升（配合第 14 章的
  计时手段验证）

### 第 13 章 内存管理：VMA 集成

- **目标**：换掉所有手写 `vkAllocateMemory`，接入 GPUOpen 的 VMA。
- **核心知识点**：为什么手写分配会失控（256MB 堆上限、`maxMemoryAllocationCount`
  4096 次上限）、VMA 的 pool/suballocation/defragmentation、预算查询
  `vmaGetBudget`、专用内存（dedicated allocation）的自动识别
- **验收**：全项目零直接 `vkAllocateMemory` 调用；VMA 统计输出无泄漏

### 第 14 章 性能分析与 GPU 计时

- **目标**：给渲染器装上"仪表盘"：每 pass 的 GPU 耗时、draw call 数、
  三角形数。
- **核心知识点**：`VkQueryPool`（`TIMESTAMP` 与 `PIPELINE_STATISTICS`）、
  `vkCmdWriteTimestamp`、timestamp 周期换算纳秒、双帧查询池的错位读取、
  RenderDoc 抓帧分析方法论（看 pass 划分、overdraw、纹理驻留）
- **验收**：屏幕上实时显示 scene pass / post pass 各自 GPU 毫秒数

### 第 15 章 异步传输与 DMA 队列

- **目标**：纹理/模型加载不卡帧，走独立 transfer queue 异步上传。
- **核心知识点**：专用 transfer queue family（`VK_QUEUE_TRANSFER_BIT` 无
  GRAPHICS）、跨队列族的 ownership transfer（`srcQueueFamilyIndex`）、
  与渲染帧的 timeline 同步、上传完成后平滑换入
- **验收**：运行中按 T 异步加载一张大纹理，帧率曲线无尖刺

---

## 第四阶段：高级主题与综合（16~20）

### 第 16 章 PBR 材质（metallic-roughness）

- **目标**：实现 Cook-Torrance BRDF，模型呈现金属/粗糙度差异。
- **核心知识点**：微表面理论（D 法线分布 / F 菲涅尔 / G 几何遮蔽）、
  metallic-roughness 工作流参数、法线贴图与 TBN、gamma 校正
  （sRGB attachment 与线性空间计算）、多光源
- **验收**：一排球体粗糙度从 0→1 渐变，视觉效果正确

### 第 17 章 IBL 基于图像的环境光照

- **目标**：用环境贴图照亮 PBR 场景，告别"黑背景打光"。
- **核心知识点**：irradiance map（卷积环境图）、prefiltered environment map
  （按粗糙度 mip 分层）、BRDF LUT（预计算积分查找表）、split-sum 近似
- **验收**：同一场景 IBL 开/关对比明显；粗糙反射随粗糙度模糊

### 第 18 章 Timeline Semaphore 与现代同步

- **目标**：用 timeline semaphore 重写全部帧同步，替换 fence+二元信号量数组。
- **核心知识点**：`VkSemaphoreTypeCreateInfo`、单调递增值语义、host 侧
  `vkWaitSemaphores`、一个 semaphore 表达整条多队列依赖链、与旧模型的
  心智负担对比
- **验收**：同步代码行数明显减少；多队列（graphics+transfer）依赖仍正确

### 第 19 章 跨平台与可移植性专题

- **目标**：渲染器在 Windows / Linux / macOS 三平台行为一致。
- **核心知识点**：surface 扩展差异（`VK_KHR_win32/xcb/wayland/metal_surface`）、
  macOS 的 `VK_KHR_portability_subset` 与 MoltenVK 限制、HDR/广色域
  surface format、高 DPI 处理、Wayland vs X11 的坑
- **验收**：同一份代码三平台编译运行，渲染结果一致（截图对比）

### 第 20 章 综合项目：迷你渲染器

- **目标**：整合前 19 章，产出一个可当作品集的完整渲染器。
- **必备特性清单**：场景图（多模型）、FPS 相机、PBR+IBL 材质、方向光阴影、
  天空盒、后处理链（bloom + tonemapping）、性能仪表盘、异步资源加载、
  三平台可编译
- **验收**：一个可交互漫游的 3D 场景，60fps，README 附三平台截图与
  性能数据

---

## 学习建议

1. **每章都手敲**，延续入门课的做法——第 01~05 章的参考实现在
   `advanced/`，建议先自己写，卡住再对照。
2. **每章做完跑一次 RenderDoc 抓帧**，哪怕只是确认结构——抓帧分析是
   第 14 章正式教、但从第 01 章就该养成的习惯。
3. **校验层全程开着**，退出时零错误才算完成。
4. 卡住时的求助顺序：Vulkan Spec 对应章节 → vulkan-tutorial.com →
   Sascha Willems 示例仓库 → RenderDoc 抓帧自查。
5. 第 06~20 章暂无参考实现，按本章节目录在 `advanced/` 下新建
   `NN_名字.cpp` 即可，CMake 会自动收编（重新 configure 一次）。

## 参考资源

- 本仓库 `Vulkan_Tutorial_CN.pdf`（入门 22 课的中文书）
- [vulkan-tutorial.com](https://vulkan-tutorial.com)（入门内容的原文）
- [Sascha Willems 示例集](https://github.com/SaschaWillems/Vulkan)（第 06~17
  章几乎每个主题都有对应示例）
- [Vulkan Spec](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)
- [RenderDoc](https://renderdoc.org/)（第 14 章起的主力工具）
- [VMA](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)（第 13 章）
