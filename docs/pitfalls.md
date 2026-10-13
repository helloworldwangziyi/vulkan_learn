# 踩坑记录总表

从 22 课实战中攒下的报错与对策，按症状字母序排列。**遇到报错先来这里查。**

## 编译 / 链接期

| 症状 | 原因与对策 |
|---|---|
| 链接报 `ld returned 5` | MinGW 的 ld 不认 `-L <dir> -l vulkan-1` 找导入库。把 `vulkan-1.lib` **全路径**显式传给链接器（本仓库 CMakeLists 已处理） |
| 杀毒软件拦截 g++ | 换 Qt 捆绑的 MinGW 13.1.0 可绕过火绒类软件对 UCRT64 的拦截 |
| `undefined reference to vkCreateDebugUtilsMessengerEXT` | 扩展函数不在导入表里，别直接链接——用 `vkGetInstanceProcAddr` 动态加载（见第 2 课壳函数模式） |

## 运行期：校验层报错

| 症状 | 原因与对策 |
|---|---|
| `does not include VK_EXT_debug_utils, but the pNext chain includes VkDebugUtilsMessengerCreateInfoEXT` | pNext 挂了 debug messenger 图纸却忘了启用 debug utils **扩展**。两层都要：层=医生、扩展=电话线（第 2 课） |
| 设备创建期的错误没人报 | 校验层是**两道关**：实例级声明后，创建 `VkDevice` 时还要再声明一次（第 4 课） |
| `VUID-vkDestroyDevice-device-05137 ... has N leaked objects` | 销毁顺序错了。铁律：引用方先死——帧缓冲 → 图像视图 → 交换链 → 命令池 → 设备（第 6-10 课） |

## 运行期：渲染异常

| 症状 | 原因与对策 |
|---|---|
| 窗口全黑、校验层一声不吭 | ① 三角形绕序与 `frontFace` 不匹配被背面剔除；② 顶点全在 NDC 之外。先查这两项（第 8 课） |
| 改了着色器没变化 | 忘了重跑 `shaders/build.cmd` 重新生成 `.spv`（第 8 课） |
| `VK_ERROR_OUT_OF_DATE_KHR` 崩溃 | 窗口 resize 后交换链过期。要检查返回码并重建交换链（第 11 课） |
| 画面缩在窗口一角 | 视口写死且 resize 后没重建管线。用动态状态把视口赶出管线（第 8 课） |
| 偶发花屏 / 帧错位 | 同步对象配对错了。信号量管 GPU↔GPU、fence 管 GPU→CPU，各归各位（第 11 课） |

## 运行期：资源路径

| 症状 | 原因与对策 |
|---|---|
| `打不开文件: shaders/xx/vert.spv` | 着色器/纹理用相对路径，**必须从项目根目录运行**可执行文件 |
| 模型加载失败 | `textures/` 与可执行文件的工作目录关系同上；OBJ/PNG 需已入库 |

## 经验法则

1. **校验层静默 = 程序健康**。它的每条消息都值得读完，verbose 级别在调试期建议全开
2. **报错先猜"生命周期"，再猜"参数"**：Vulkan 的对象引用关系比参数对错更容易出问题
3. **改 shader 必跑 build.cmd；加新课必重跑 cmake configure**（GLOB 才发现新 target）
4. **段错误优先怀疑野句柄**：Vulkan 句柄是裸指针，用错了不会报"野指针"而是随机崩溃——开校验层能拦下大半

> **回到**：[目录](README.md)
