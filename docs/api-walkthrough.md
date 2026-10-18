# Vulkan API 全解（第 1-11 课）

> 本文是 `lessons/11_hello_triangle.cpp`（807 行）的**逐 API 注解**：按 `initVulkan()` 执行顺序，把每个 Vulkan 调用、每个结构体字段的用途和理由全部摊开。建议对照源码阅读。
> 阅读进度：截至本文，管线装配已全部完成，剩余 `createFrameBuffers` / `createCommandPool` / `createCommandBuffers` / `drawFrame` 为练习部分。

## 总流程

```
run()
 ├── initWindow()        GLFW 窗口（GLFW_NO_API：不建 OpenGL 上下文）
 ├── initVulkan()        下面 12 步，顺序即依赖链
 ├── mainLoop()          事件循环（drawFrame 待实现）
 └── cleanUp()           销毁，严格"引用方先死"顺序
```

## ① 实例：`createInstance()`

### `VkApplicationInfo`（自我介绍，可选但建议填）

| 字段 | 值 | 细节 |
|---|---|---|
| `sType` | `APPLICATION_INFO` | Vulkan 所有结构体第一个字段都是 `sType`，驱动靠它识别内存布局 |
| `pApplicationName` | `"Hello Triangle"` | 纯元数据，调试工具可见 |
| `applicationVersion` | `VK_MAKE_VERSION(1,0,0)` | 宏把三段版本打包成 `(major<<22)\|(minor<<12)\|patch` |
| `pEngineName` / `engineVersion` | `"No Engine"` / 1.0.0 | 同为元数据 |
| `apiVersion` | `VK_API_VERSION_1_0` | 声明"我需要的版本"，加载器拿它与各层/驱动比对 |

### `VkInstanceCreateInfo`（真正的参数表）

| 字段 | 细节 |
|---|---|
| `pApplicationInfo` | 指向自我介绍 |
| `enabledLayerCount` / `ppEnabledLayerNames` | 启用校验层；不启用时**必须显式写 0/nullptr** |
| `pNext = &debugCreateInfo` | 把 debug messenger"图纸"挂进创建流程，覆盖**实例创建期**的报错（messenger 本是实例创建后才能建的对象，加载器见图纸会内部代建临时的——鸡生蛋问题的官方解法） |
| `enabledExtensionCount` / `ppEnabledExtensionNames` | GLFW 申报的窗口扩展 + `VK_EXT_debug_utils` |

### 配套函数

- **`checkValidationLayerSupport()`**：两段式枚举 `vkEnumerateInstanceLayerProperties`，`strcmp` 核对需求 ⊆ 本机全集。**"遍历需求清单查全集"模式全教程出现四次**（层、实例扩展、设备扩展、队列族）。
- **`getRequiredExtensions()`**：`glfwGetRequiredInstanceExtensions` 让 GLFW 亲口告诉你窗口扩展；开校验层时追加 `VK_EXT_DEBUG_UTILS_EXTENSION_NAME`（宏展开即 `"VK_EXT_debug_utils"`）。
- **`populateDebugMessengerCreateInfo()`**：severity 三级全开（VERBOSE/WARNING/ERROR）、type 三类全开（GENERAL/VALIDATION/PERFORMANCE）、`pfnUserCallback = debugCallback`。
- **`debugCallback`**：`VKAPI_ATTR ... VKAPI_CALL` 是调用约定修饰（Windows 为 `__stdcall`），C 接口回调必须加。返回 `VK_FALSE` = 不中止触发它的调用。**回调里不能 throw**——可能跑在驱动任意线程上。
- **壳函数 `CreateDebugUtilsMessengerEXT` / `Destroy...EXT`**：扩展函数不在 `vulkan-1.lib` 导入表，`vkGetInstanceProcAddr(instance, "名字")` 按名查指针，查到转发、查不到返回 `VK_ERROR_EXTENSION_NOT_PRESENT`。**壳函数模式**在第 6 课交换链处再次实战。

## ② 表面：`createSurface()`

```cpp
glfwCreateWindowSurface(instance, window, nullptr, &surface)
```

GLFW 内部按平台分发表代办（Win32 填 `VkWin32SurfaceCreateInfoKHR` 含 `HWND`）。归属：**表面跟实例走**，在 `vkDestroyInstance` 之前销毁，不是设备级对象。

## ③ 物理设备：`pickPhysicalDevice()`

### `vkEnumeratePhysicalDevices`（两段式）

`nullptr` 先拿数量，再填满数组。得到的是**句柄**：不创建、不销毁。

### `vkGetPhysicalDeviceProperties`

取名字与类型（`DISCRETE_GPU` 独显 / `INTEGRATED_GPU` 核显 / `VIRTUAL_GPU` / `CPU` 软渲染），只用于打印。

### `findQueueFamilies()`——两个查询

**`vkGetPhysicalDeviceQueueFamilyProperties`**（两段式）：`queueFlags` 是位掩码，判断用 `& VK_QUEUE_GRAPHICS_BIT` **不能用 `==`**（一族可同时支持图形+计算+传输）。

**`vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &support)`**：三个参数缺一不可——呈现能力是"设备 × 族 × 表面"组合的属性。

### `QueueFamilyIndices`

双 `std::optional<uint32_t>`：`has_value()` 区分"没找到"和"下标 0"，`isComplete()` 双齐放行。

> ⚠️ 当前 `isDeviceSuitable()` 只查队列族、未查设备扩展。`createLogicalDevice` 无条件启用 `VK_KHR_swapchain`，不支持交换链的设备会死在设备创建。教程第 6 课的 `checkDeviceExtensionSupport()` 是标准补丁。

## ④ 逻辑设备：`createLogicalDevice()`

### `VkDeviceQueueCreateInfo`（每族一份，`std::set` 去重）

| 字段 | 细节 |
|---|---|
| `queueFamilyIndex` | 图形族=呈现族时 set 去重后只建一份申请 |
| `queueCount` | 创建后不可追加，想换只能重建设备 |
| `pQueuePriorities` | **数组指针**，长度 ≥ queueCount；0.0~1.0 只在同族多队列间有区分意义 |

### `VkDeviceCreateInfo`

| 字段 | 细节 |
|---|---|
| `pEnabledFeatures` | `VkPhysicalDeviceFeatures{}` 全零 = 不开任何可选特性。各向异性、深度钳制等能力**默认关闭**，要用必须在此声明 |
| `ppEnabledExtensionNames` | `VK_KHR_SWAPCHAIN_EXTENSION_NAME`——**设备级扩展**，与实例扩展两条通道 |
| `enabledLayerCount = 0` | 设备层自 Vulkan 1.1 起废弃，规范要求必须为 0（与实例层声明不冲突） |

### `vkGetDeviceQueue(device, family, 0, &queue)`

按（族，族内序号）取队列句柄，队列随设备生灭，**不需要销毁**。

## ⑤ 交换链：`createSwapChain()`

### `querySwapchainSupport()`——三查

| API | 返回 |
|---|---|
| `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` | 数量/尺寸范围/当前变换，单结构体 |
| `vkGetPhysicalDeviceSurfaceFormatsKHR`（两段式） | 格式 × 色彩空间清单 |
| `vkGetPhysicalDeviceSurfacePresentModesKHR`（两段式） | 呈现模式清单 |

### 三个三选一

- **格式**：首选 `B8G8R8A8_SRGB` + `SRGB_NONLINEAR`，没有就降级 `formats[0]`（规范保证非空）
- **呈现模式**：首选 `MAILBOX_KHR`（不撕裂、低延迟），退 `FIFO_KHR`（规范保证存在）
- **分辨率**：`currentExtent.width != UINT32_MAX` 照抄；特殊值时取 GLFW 帧缓冲尺寸再 `std::clamp` 进 `[min, max]`

### `VkSwapchainCreateInfoKHR` 逐字段

| 字段 | 值 | 为什么 |
|---|---|---|
| `minImageCount` | `min+1`，再夹到 max | 卡下限=一张被占用就干等，+1 买余量；`max=0` 表示无上限 |
| `imageUsage` | `COLOR_ATTACHMENT_BIT` | 管线需要什么 usage 必须在此声明 |
| `imageSharingMode` | 同族 `EXCLUSIVE` / 异族 `CONCURRENT`+双下标 | EXCLUSIVE 性能更好但同一时刻只能一族用 |
| `preTransform` | `capabilities.currentTransform` | 不额外旋转 |
| `compositeAlpha` | `OPAQUE_BIT_KHR` | 窗口不透明，不与桌面混合 |
| `clipped` | `VK_TRUE` | 被遮挡像素不画，省性能 |
| `oldSwapchain` | `VK_NULL_HANDLE` | 首次创建；resize 重建时传旧链 |

### `vkGetSwapchainImagesKHR`（两段式）

**数量以驱动实发为准**（请求 3 张可能给 4 张）。`VkImage` 借用不销毁；`format`/`extent` 存成员，后续全靠它俩。

## ⑥ 图像视图：`createImageViews()`

| 字段 | 值 | 细节 |
|---|---|---|
| `viewType` | `VIEW_TYPE_2D` | 还能 3D/Cube |
| `components.*` | `IDENTITY` | 如实解读；SWIZZLE 可不搬数据换通道顺序 |
| `subresourceRange.aspectMask` | `COLOR_BIT` | 这份视图"当颜色用"（深度是 `DEPTH_BIT`） |
| `levelCount` / `layerCount` | **1** / 1 | 无 mip 无数组也要写 1——写 0 等于空视图 |

## ⑦ 渲染通道：`createRenderPass()`

`VkAttachmentDescription`：

| 字段 | 值 | 细节 |
|---|---|---|
| `loadOp` | `CLEAR` | 每帧开工前清屏 |
| `storeOp` | `STORE` | 要呈现，必须保留 |
| `stencilLoadOp/StoreOp` | `DONT_CARE` | 没用到模板，但 Vulkan 要求显式填 |
| `initialLayout` | `UNDEFINED` | 不关心旧内容（配合 CLEAR） |
| `finalLayout` | `PRESENT_SRC_KHR` | 子通道结束后直接可上屏 |

子通道：`VkAttachmentReference{attachment=0, layout=COLOR_ATTACHMENT_OPTIMAL}` 是附件的"角色位"。

依赖：`srcSubpass=EXTERNAL → dstSubpass=0`，双方阶段都是 `COLOR_ATTACHMENT_OUTPUT` + `dstAccess=COLOR_ATTACHMENT_WRITE`——"交换链图刚被呈现引擎用过，本帧写颜色前必须等它空出来"。

## ⑧ 图形管线：`createGraphicsPipline()`

### 着色器链

- `readFile`：`ate` 定位文件尾 + `tellg` 拿大小，回卷整读——读 SPIR-V 标准姿势
- `createShaderModule`：`codeSize`=字节数、`pCode` 强转 `const uint32_t*`（SPIR-V 是 4 字节码）
- 模块是"代码容器"，**装配完管线立刻销毁**（`vkDestroyShaderModule`）
- `VkPipelineShaderStageCreateInfo`：`stage` 位 + `module` + `pName="main"`（入口名）

### 十个状态块

| 块 | 关键值 | 一句话 |
|---|---|---|
| 顶点输入 | 两 count=0 | 坐标写死在着色器里，无顶点缓冲 |
| 输入装配 | `TRIANGLE_LIST` | 每 3 顶点独立成三角形；restart 关 |
| 视口裁剪 | 两 count=1 | 只登记数量——动态状态 |
| 光栅化 | FILL、背面剔除、`CLOCKWISE` | 顺时针是为后续投影 Y 翻转预留（与教程默认相反，有理由） |
| 多重采样 | 关 | 占位，MSAA 后升级 |
| 混合（附件） | `blendEnable=false`、写掩码全开 | 新像素直接覆盖旧像素 |
| 混合（全局） | `logicOpEnable=false` | 走 blend 路线不走位运算；`blendConstants` 显式置零 |
| 动态状态 | VIEWPORT + SCISSOR | 录制命令时 `vkCmdSetViewport/Scissor` 现填 |
| 管线布局 | 两 count=0 | 着色器不读外部资源，空白申报单 |
| 总装配 | `VkGraphicsPipelineCreateInfo` | 全部块指针 + `layout` + `renderPass` + `subpass=0` |

`vkCreateGraphicsPipelines(device, 缓存池=NULL, 1, &info, ..., &pipeline)`：复数+数组，一次可建多条（引擎预编译管线变体靠它）。

## 销毁顺序铁律（cleanUp）

```
帧缓冲 → 管线 → 管线布局 → 渲染通道 → 图像视图 → 交换链
→ 命令池 → 设备 → (debug messenger) → 表面 → 实例 → GLFW
```

原则只有一条：**引用方先于被引用方销毁**。漏一个，校验层退出时必吼 "leaked objects"。

## 当前缺口与隐患清单

**待写**：`createFrameBuffers`（图纸+视图→一图一本）、`createCommandPool`（`RESET_COMMAND_BUFFER_BIT`+graphicsFamily）、`createCommandBuffers`（reset→begin→renderPass→setViewport/Scissor→bindPipeline→`vkCmdDraw(3,1,0,0)`→end→end）、`drawFrame`（acquire→等 fence→录制→submit→present，信号量"一到一离"）。

**隐患**：

1. `cleanUp()` 需补 `vkDestroyPipeline` / `vkDestroyPipelineLayout` / `vkDestroyRenderPass`（已创建但未销毁）
2. 成员 `imageAvailaleSemaphore` 拼写（`Availale`→`Available`），搜索时坑自己
3. `querySwapchainSupport` 第二次取 formats 用了成员 `physicalDevice` 而非参数 `device`——巧合正确，建议改参数

> **回到**：[目录](README.md)
