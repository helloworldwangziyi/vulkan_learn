// ===========================================================================
// 中高级 第 04 章：描述符体系进阶 —— 帧 / 材质 / 物体 三层数据流 + 纹理
//
// 累积实现 = 01 基座（RAII 框架）+ 02（交换链重建）+ 03（push constants，
// 8 个四边形逐物体 vkCmdPushConstants）+ 本章（三层描述符体系）。
//
// 本章核心思想：【按更新频率给描述符分层】。
//   一个渲染器里的数据按"多久变一次"天然分三层：
//     帧级（set 0）  ：相机 VP 矩阵——每帧变一次
//                     → UBO，双帧各一套（per-frame 双份），每帧 memcpy 更新内容
//     材质级（set 1）：纹理 + 采样器——从加载到销毁不变
//                     → combined image sampler，每种材质一套，初始化写一次终生不改
//     物体级        ：model 矩阵 + 染色——逐物体变
//                     → push constants（第 03 章），直接写进命令缓冲，连描述符都不用
//
//   分层的好处（对照入门课 15/18 课"全塞一个 set"的做法）：
//     * set 1 只在切材质时重绑，set 0 每帧只绑一次——互不牵连；
//       vkCmdBindDescriptorSets 的 firstSet 参数就是为此设计的
//     * 每帧的 vkUpdateDescriptorSets 调用次数 = 0（帧级 UBO 走的是
//       "描述符指向固定缓冲，只 memcpy 缓冲内容"的路线；材质集初始化后不再碰）
//
// 渲染内容：8 个旋转四边形排成 4×2 网格，交替绑定两种材质——
//   材质 0：stb_image 加载 textures/texture.png
//   材质 1：程序化生成的棋盘格（不引入新文件，CPU 直接填像素）
//   最终颜色 = 纹理采样结果 × push constant 的 tint
//
// 纹理创建走 staging buffer + 布局迁移（lessons/16~18 流程的 RAII 重写）；
// 不做 mipmap：image 的 mipLevels = 1、sampler 的 maxLod = 0.0f。
// ===========================================================================
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// 数学库：Vulkan SDK 自带（Include/glm），无需额外安装
// GLM_FORCE_RADIANS：杜绝"角度制悄悄混进来"的历史包袱
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 图片解码：stb 单头文件（tools/include/stb_image.h）。
// 本文件是自包含单文件，STB_IMAGE_IMPLEMENTATION 直接放这里——
// 该宏让 stb 生成函数实现，全工程只能有一个翻译单元定义它（lessons/16 同款用法）
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

// 允许同时在 GPU 上执行的帧数：2 = 录制第 N+1 帧时第 N 帧还在光栅化
const int MAX_FRAMES_IN_FLIGHT = 2;

// 材质数量：纹理 PNG + 程序化棋盘格，两种
const uint32_t MATERIAL_COUNT = 2;

// 场景物体数量（第 03 章）：8 个四边形，4×2 网格
const uint32_t OBJECT_COUNT = 8;

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __APPLE__
    "VK_KHR_portability_subset",   // MoltenVK 必需
#endif
};

// ---------------------------------------------------------------------------
// VK_CHECK：统一检查 VkResult，失败抛出带 文件+行号+表达式+错误码 的异常。
// do-while(0) 包裹让宏在 if/else 里也能安全当一条语句用（行业惯例写法）。
// ---------------------------------------------------------------------------
#define VK_CHECK(expr)                                                        \
    do {                                                                      \
        VkResult vkCheckResult_ = (expr);                                     \
        if (vkCheckResult_ != VK_SUCCESS) {                                   \
            throw std::runtime_error(std::string(__FILE__) + ":" +            \
                std::to_string(__LINE__) + "  " #expr "  失败, VkResult = " + \
                std::to_string(static_cast<int>(vkCheckResult_)));            \
        }                                                                     \
    } while (0)

// ---------------------------------------------------------------------------
// UniqueVkHandle<T>：Vulkan 句柄的 RAII 包装（第 01 章基座，原样沿用）。
//   * 默认构造 = 空句柄；析构自动调销毁函数
//   * 禁拷贝（句柄是独占资源），noexcept 移动
//   * deleter 用 std::function<void(T)> 类型擦除，包装类不关心销毁细节
// ---------------------------------------------------------------------------
template <typename T>
class UniqueVkHandle {
public:
    UniqueVkHandle() = default;

    UniqueVkHandle(T handle, std::function<void(T)> deleter)
        : handle_(handle), deleter_(std::move(deleter)) {}

    ~UniqueVkHandle() { reset(); }

    // 独占资源：拷贝即 double-destroy，编译期禁掉
    UniqueVkHandle(const UniqueVkHandle&) = delete;
    UniqueVkHandle& operator=(const UniqueVkHandle&) = delete;

    // noexcept 移动：所有权转移，源对象置空（析构不再销毁）
    UniqueVkHandle(UniqueVkHandle&& other) noexcept
        : handle_(other.handle_), deleter_(std::move(other.deleter_)) {
        other.handle_ = VK_NULL_HANDLE;
    }

    UniqueVkHandle& operator=(UniqueVkHandle&& other) noexcept {
        if (this != &other) {
            reset();    // 先放掉自己持有的旧资源，再接手新的
            handle_ = other.handle_;
            deleter_ = std::move(other.deleter_);
            other.handle_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    // 手动提前销毁并回到空状态；空句柄上调用安全（什么都不做）
    void reset() {
        if (handle_ != VK_NULL_HANDLE && deleter_) {
            deleter_(handle_);
        }
        handle_ = VK_NULL_HANDLE;
    }

    T get() const { return handle_; }
    // 只读取地址：给 vkWaitForFences 这类"读句柄"的 API 用
    const T* ptr() const { return &handle_; }
    explicit operator bool() const { return handle_ != VK_NULL_HANDLE; }
    // 隐式转回裸句柄：可以直接传给任何 vk* 函数，调用点保持教程原貌
    operator T() const { return handle_; }

private:
    T handle_ = VK_NULL_HANDLE;
    std::function<void(T)> deleter_;
};

// ---------------------------------------------------------------------------
// GLFW 窗口的 RAII 包装。
// 第 02 章起窗口【允许缩放】（交换链重建已就绪），并留出 resize 回调的接线口
// ---------------------------------------------------------------------------
struct Window {
    GLFWwindow* handle = nullptr;

    Window(uint32_t width, uint32_t height, const char* title) {
        if (!glfwInit()) {
            throw std::runtime_error("GLFW 初始化失败");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // 不创建 OpenGL 上下文
        // 第 02 章：允许缩放，由交换链重建兜底（01 章是 GLFW_FALSE 禁止缩放）
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        handle = glfwCreateWindow(width, height, title, nullptr, nullptr);
        if (!handle) {
            glfwTerminate();
            throw std::runtime_error("创建 GLFW 窗口失败");
        }
    }

    ~Window() {
        if (handle) glfwDestroyWindow(handle);
        glfwTerminate();
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {

    std::cerr << "[vulkan] " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

// debug utils 扩展函数不在 loader 的导出表里，必须动态加载
VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance,
    VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, nullptr);
    }
}

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

// ---------------------------------------------------------------------------
// 顶点数据结构（本章新增 vec2 uv）：一个顶点 = 位置(vec2) + 纹理坐标(vec2)。
// 顶点色从 01 基座里移除了——逐物体的颜色改走第 03 章 push constant 的 tint，
// 顶点不再需要逐顶点颜色。
// 与 shaders/advanced/04/shader.vert 的 layout(location=0/1) 一一对应
// ---------------------------------------------------------------------------
struct Vertex {
    glm::vec2 pos;
    glm::vec2 uv;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;                   // 绑定点编号（绑定缓冲时用）
        bindingDescription.stride = sizeof(Vertex);       // 相邻两顶点的字节间距
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;  // 逐顶点取数
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
        // location 0 → inPosition：vec2，从顶点开头(offset 0)取
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);
        // location 1 → inTexCoord：vec2，紧跟 pos 之后(offset 8)
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, uv);
        return attributeDescriptions;
    }
};

// 单位四边形的四个顶点：局部坐标 [-0.5, 0.5]，UV 覆盖整张纹理 [0, 1]。
// 顺序按"屏幕空间"记 [上左, 下左, 下右, 上右] —— lessons/11 实测的正面绕序。
// 所有 8 个物体共享这同一份几何，位置/朝向差异由 push constant 的 model 给出
const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f}, {0.0f, 0.0f}},   // 0: 上左, UV 左上
    {{-0.5f,  0.5f}, {0.0f, 1.0f}},   // 1: 下左, UV 左下
    {{ 0.5f,  0.5f}, {1.0f, 1.0f}},   // 2: 下右, UV 右下
    {{ 0.5f, -0.5f}, {1.0f, 0.0f}},   // 3: 上右, UV 右上
};

// 两个三角形共用顶点 0 和 2：4 顶点 + 6 下标。
// 注意绕序与无 UBO 的课程相反：proj 的 Y 翻转让 NDC 里方向倒转，
// 下标若不跟着反转，整个矩形会被背面剔除（lessons/15 实测黑屏的教训）
const std::vector<uint16_t> indices = {
    0, 2, 1,   // 左下三角
    0, 3, 2,   // 右上三角
};

// ---------------------------------------------------------------------------
// 【帧级数据】set 0 的内容：只剩 view + proj。
// 01 基座的 UBO 里还有 model——第 03 章把它挪进了 push constants（逐物体数据
// 不该让所有物体共享一份 UBO）。剩下的 view/proj 是真正的"每帧一次"全局量。
// C++ 和 shader.vert 两边必须 layout 一致（glm::mat4 各占 64 字节、对齐 16）
// ---------------------------------------------------------------------------
struct FrameUniformBuffer {
    glm::mat4 view;
    glm::mat4 proj;
};

// ---------------------------------------------------------------------------
// 【物体级数据】push constant 块（第 03 章）：model + tint，共 80 字节。
// spec 保证 maxPushConstantsSize ≥ 128 字节，80 在任何设备上都安全。
// 它是"直接写在命令缓冲里"的数据：不需要缓冲、不需要描述符、不需要同步——
// 代价是容量小（≤128B 起步）且每次绘制都要重推。
// 选型口诀：每帧一次的大块 → UBO；终生不变 → 描述符；逐物体的小块 → push constant
// ---------------------------------------------------------------------------
struct ObjectPushConstants {
    glm::mat4 model;   // 物体变换（平移到网格位 + 绕自身中心旋转）
    glm::vec4 tint;    // 染色：片段着色器里与纹理采样结果相乘
};

// 场景物体（第 03 章）：网格位置 + 旋转速度 + 染色。
// 几何共享，材质按物体下标交替（i % MATERIAL_COUNT）
struct SceneObject {
    glm::vec2 position;      // 在 XY 平面上的摆放位置
    float rotationSpeed;     // 绕 Z 轴旋转角速度（弧度/秒），正负 = 顺逆时针
    glm::vec4 tint;          // 染色（乘在纹理颜色上）
};

// 4×2 网格：间距 1.3， quad 边长 1.0，留缝看清边界。
// 相机在 (0,0,4.5) 正视，FOV 45° 下横纵都刚好放下
const std::array<SceneObject, OBJECT_COUNT> objects = {{
    // 上排
    { {-1.95f,  0.65f},  0.6f, {1.0f, 1.0f, 1.0f, 1.0f} },   // 白（不染，看原图）
    { {-0.65f,  0.65f}, -0.8f, {1.0f, 0.4f, 0.4f, 1.0f} },   // 红
    { { 0.65f,  0.65f},  1.0f, {0.4f, 1.0f, 0.4f, 1.0f} },   // 绿
    { { 1.95f,  0.65f}, -1.2f, {0.4f, 0.6f, 1.0f, 1.0f} },   // 蓝
    // 下排
    { {-1.95f, -0.65f},  1.4f, {1.0f, 1.0f, 0.4f, 1.0f} },   // 黄
    { {-0.65f, -0.65f}, -1.6f, {0.4f, 1.0f, 1.0f, 1.0f} },   // 青
    { { 0.65f, -0.65f},  1.8f, {1.0f, 0.4f, 1.0f, 1.0f} },   // 品红
    { { 1.95f, -0.65f}, -2.0f, {1.0f, 0.7f, 0.2f, 1.0f} },   // 橙
}};

// ---------------------------------------------------------------------------
// VkBuffer 与 VkDeviceMemory 是一对连体婴：缓冲对象只是"描述符"，真正装
// 字节的显存是另一实体。打包成一个小 struct 一起 RAII。
// 成员声明顺序决定析构顺序：buffer 后声明 → 先析构（vkDestroyBuffer），
// memory 先声明 → 后析构（vkFreeMemory）。
// ---------------------------------------------------------------------------
struct BufferResource {
    UniqueVkHandle<VkDeviceMemory> memory;
    UniqueVkHandle<VkBuffer> buffer;
    void* mapped = nullptr;   // uniform 缓冲的持续映射地址（随 memory 失效，不拥有）
};

// ---------------------------------------------------------------------------
// VkImage 与 VkDeviceMemory 同理（本章新增）：图像对象 + 显存打包。
// 与 BufferResource 同构，只是创建/销毁走 vkCreateImage / vkDestroyImage
// ---------------------------------------------------------------------------
struct ImageResource {
    UniqueVkHandle<VkDeviceMemory> memory;
    UniqueVkHandle<VkImage> image;
};

// ---------------------------------------------------------------------------
// 一种材质（本章主角之一）= 纹理图像 + 视图 + 采样器 + 一个 set 1 描述符集。
// descriptorSet 不包装：它随描述符池销毁自动释放（spec 保证），这里只是
// 记录句柄供绑定时取用。
// 移动语义由成员自动合成（UniqueVkHandle 全是 move-only）
// ---------------------------------------------------------------------------
struct Texture {
    ImageResource image;                          // 图像 + 显存
    UniqueVkHandle<VkImageView> view;             // "怎么读这张图"
    UniqueVkHandle<VkSampler> sampler;            // "怎么取像素"
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;   // set 1，池拥有，不包装
};

class VulkanApp {
public:
    void run() {
        // 窗口由成员初始值构造（见成员区），Vulkan 在这里初始化
        initVulkan();
        mainLoop();
        // 没有 cleanup()！mainLoop 返回后 ~VulkanApp() 自动按成员声明逆序
        // 销毁所有资源——第 01 章的核心成果
    }

private:
    // =========================================================================
    // 成员声明区 —— 第 01 章定下的设计：
    //   C++ 保证成员按【声明的逆序】析构；Vulkan 要求按【创建的逆序】销毁。
    //   两个逆序对消 ⇒ 声明顺序 ≈ 创建顺序，就是天然正确的销毁顺序。
    //
    // 不包装（明确不拥有）的句柄：
    //   VkPhysicalDevice / VkQueue      枚举/获取而来，无需销毁
    //   swapChainImages                 归交换链所有，随交换链销毁
    //   commandBuffers / descriptorSets 随各自的池销毁
    //   imagesInFlight                  只是 fence 的"观测记录"，不拥有
    // =========================================================================
    Window window{WIDTH, HEIGHT, "advanced 04 - descriptor sets"};

    UniqueVkHandle<VkInstance> instance;
    UniqueVkHandle<VkDebugUtilsMessengerEXT> debugMessenger;   // 依赖 instance
    UniqueVkHandle<VkSurfaceKHR> surface;                      // 依赖 instance

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;          // 不拥有
    VkQueue graphicsQueue = VK_NULL_HANDLE;                    // 不拥有
    VkQueue presentQueue = VK_NULL_HANDLE;                     // 不拥有
    UniqueVkHandle<VkDevice> device;

    // ---- 以下全部依赖 device，都在 device 之后声明（先于 device 析构）----

    // 同步对象：imageAvailable/fence 按帧，双缓冲；
    // renderFinished 按交换链图像（呈现引擎可能还拿着信号量，只有重新采集到
    // 同一张图才允许再 signal）；imagesInFlight 记录每张图被哪帧占用。
    // renderFinished/imagesInFlight 在交换链重建时按新图像数量重建
    std::array<UniqueVkHandle<VkSemaphore>, MAX_FRAMES_IN_FLIGHT> imageAvailableSemaphores;
    std::vector<UniqueVkHandle<VkSemaphore>> renderFinishedSemaphores;
    std::array<UniqueVkHandle<VkFence>, MAX_FRAMES_IN_FLIGHT> inFlightFences;
    std::vector<VkFence> imagesInFlight;

    // 命令池（池销毁连带释放所有命令缓冲，故 commandBuffers 不包装）
    UniqueVkHandle<VkCommandPool> commandPool;
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers{};

    // -----------------------------------------------------------------
    // 描述符体系（本章）：两个 set layout + 一个池 + 两组集合
    //   frameSetLayout    → set 0：帧级 UBO，双帧各一套
    //   materialSetLayout → set 1：材质级 combined image sampler，每材质一套
    // 集合不包装：池销毁时自动释放
    // -----------------------------------------------------------------
    UniqueVkHandle<VkDescriptorSetLayout> frameSetLayout;
    UniqueVkHandle<VkDescriptorSetLayout> materialSetLayout;
    UniqueVkHandle<VkDescriptorPool> descriptorPool;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> frameDescriptorSets{};
    std::array<VkDescriptorSet, MATERIAL_COUNT> materialDescriptorSets{};

    // 两种材质：0 = texture.png（stb_image 加载），1 = 程序化棋盘格
    std::array<Texture, MATERIAL_COUNT> textures;

    // 几何与帧级 uniform 数据
    BufferResource vertexBuffer;
    BufferResource indexBuffer;
    std::array<BufferResource, MAX_FRAMES_IN_FLIGHT> uniformBuffers;   // 每帧一份

    // 交换链：图像本身不包装（交换链拥有），format/extent 是纯数据
    UniqueVkHandle<VkSwapchainKHR> swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;

    // 管线三件套 + 依附交换链的视图与帧缓冲
    UniqueVkHandle<VkRenderPass> renderPass;
    UniqueVkHandle<VkPipelineLayout> pipelineLayout;
    std::vector<UniqueVkHandle<VkImageView>> swapChainImageViews;
    std::vector<UniqueVkHandle<VkFramebuffer>> swapChainFramebuffers;
    UniqueVkHandle<VkPipeline> graphicsPipeline;

    uint32_t currentFrame = 0;          // 帧轮转下标
    bool framebufferResized = false;    // 第 02 章：窗口尺寸变化标记

    // -----------------------------------------------------------------
    // deleter 工厂：生成"依赖 device 的销毁函数"。
    // 注意捕获的是 device 句柄的**值**而非 this：句柄本质是个不透明标识符，
    // 拷贝值不影响销毁目标；这样包装对象可以自由移动，不受 app 牵连。
    // -----------------------------------------------------------------
    template <typename T>
    std::function<void(T)> deviceDeleter(
            void (*destroy)(VkDevice, T, const VkAllocationCallbacks*)) const {
        VkDevice dev = device;
        return [dev, destroy](T handle) { destroy(dev, handle, nullptr); };
    }

    // 同上，依赖 instance 的版本（surface 用）
    template <typename T>
    std::function<void(T)> instanceDeleter(
            void (*destroy)(VkInstance, T, const VkAllocationCallbacks*)) const {
        VkInstance inst = instance;
        return [inst, destroy](T handle) { destroy(inst, handle, nullptr); };
    }

    void initVulkan() {
        // 第 02 章：窗口尺寸回调只立旗标，重建推迟到 drawFrame 里做
        // （回调触发时交换链可能正在使用中，原地重建会踩坑）
        glfwSetWindowUserPointer(window.handle, this);
        glfwSetFramebufferSizeCallback(window.handle, framebufferResizeCallback);

        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapChain(VK_NULL_HANDLE);   // 首建没有旧链
        createImageViews();
        createRenderPass();
        createDescriptorSetLayouts();      // 本章：两个 set layout，管线创建要用
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createTextures();                  // 本章：两种材质（需要命令池做布局迁移）
        createVertexBuffer();              // staging 模式上显存
        createIndexBuffer();
        createUniformBuffers();            // 帧级 UBO，每帧一份
        createDescriptorPool();
        createFrameDescriptorSets();       // set 0 × 双帧
        createMaterialDescriptorSets();    // set 1 × 材质数，写完终生不改
        createCommandBuffers();
        createSyncObjects();
    }

    // 第 02 章：GLFW 回调必须是静态函数，借 user pointer 找回 this
    static void framebufferResizeCallback(GLFWwindow* w, int, int) {
        auto app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(w));
        app->framebufferResized = true;
    }

    // -----------------------------------------------------------------
    // 实例。每个创建函数都是同一个模式：
    //   裸句柄创建 → VK_CHECK → **立即**包装进 UniqueVkHandle
    // "立即"是关键：创建成功到包装之间不能夹任何可能抛异常的代码，
    // 否则异常路径上句柄泄漏——这行就是泄漏窗口的边界
    // -----------------------------------------------------------------
    void createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("请求的校验层不可用");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Advanced 04 - Descriptor Sets";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
#ifdef __APPLE__
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        auto extensions = getRequiredExtensions();
#ifdef __APPLE__
        // MoltenVK 是可移植性驱动，不显式声明不会被枚举（macOS 必需）
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
        } else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        VkInstance rawInstance;
        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &rawInstance));
        instance = UniqueVkHandle<VkInstance>(rawInstance,
            [](VkInstance h) { vkDestroyInstance(h, nullptr); });
    }

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                   | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
    }

    void setupDebugMessenger() {
        if (!enableValidationLayers) return;

        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        populateDebugMessengerCreateInfo(createInfo);

        VkDebugUtilsMessengerEXT rawMessenger;
        VK_CHECK(CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &rawMessenger));
        // 销毁函数同样要动态加载，签名与 vkDestroy* 不同，单独写 deleter
        VkInstance inst = instance;
        debugMessenger = UniqueVkHandle<VkDebugUtilsMessengerEXT>(rawMessenger,
            [inst](VkDebugUtilsMessengerEXT h) { DestroyDebugUtilsMessengerEXT(inst, h, nullptr); });
    }

    void createSurface() {
        VkSurfaceKHR rawSurface;
        VK_CHECK(glfwCreateWindowSurface(instance, window.handle, nullptr, &rawSurface));
        surface = UniqueVkHandle<VkSurfaceKHR>(rawSurface, instanceDeleter(vkDestroySurfaceKHR));
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("找不到任何支持 Vulkan 的显卡");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& dev : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            std::cout << "候选设备: " << props.deviceName << std::endl;

            if (isDeviceSuitable(dev)) {
                physicalDevice = dev;
                std::cout << ">> 选中: " << props.deviceName << std::endl;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("没有满足要求的显卡");
        }
    }

    bool isDeviceSuitable(VkPhysicalDevice dev) {
        QueueFamilyIndices indices = findQueueFamilies(dev);
        bool extensionsSupported = checkDeviceExtensionSupport(dev);
        return indices.isComplete() && extensionsSupported;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice dev) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }
        return requiredExtensions.empty();
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice dev) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &presentSupport);
            if (presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) {
                break;
            }
            i++;
        }

        return indices;
    }

    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {
            indices.graphicsFamily.value(), indices.presentFamily.value()
        };

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // 本章不需要任何特殊设备特性（lessons/17 的各向异性采样要在这里开
        // samplerAnisotropy，本章从简不开，采样器里 anisotropyEnable = FALSE）
        VkPhysicalDeviceFeatures deviceFeatures{};

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        createInfo.enabledLayerCount = 0;

        VkDevice rawDevice;
        VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &rawDevice));
        device = UniqueVkHandle<VkDevice>(rawDevice,
            [](VkDevice h) { vkDestroyDevice(h, nullptr); });

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    // -----------------------------------------------------------------
    // 交换链（第 02 章版：支持重建接力）。
    // oldSwapchain：重建时传入旧链句柄——驱动可以在旧链基础上平滑过渡；
    // 首建传 VK_NULL_HANDLE。旧链必须活到新链创建返回之后（见 recreateSwapChain）
    // -----------------------------------------------------------------
    void createSwapChain(VkSwapchainKHR oldSwapchain) {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);

        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 &&
            imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = {
            indices.graphicsFamily.value(), indices.presentFamily.value()
        };
        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = oldSwapchain;   // 第 02 章：入门课写死 NULL 的坑在这填上

        VkSwapchainKHR rawSwapChain;
        VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &rawSwapChain));
        swapChain = UniqueVkHandle<VkSwapchainKHR>(rawSwapChain, deviceDeleter(vkDestroySwapchainKHR));

        // 交换链图像归交换链所有（随交换链销毁），只存裸句柄供建视图用
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());

        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
                availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }
        return availableFormats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            int width, height;
            glfwGetFramebufferSize(window.handle, &width, &height);
            VkExtent2D actualExtent = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)
            };
            actualExtent.width = std::clamp(actualExtent.width,
                capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height,
                capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice dev) {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    // -----------------------------------------------------------------
    // 通用图像视图创建（lessons/17 的抽出）：交换链视图与纹理视图同构，
    // 只是 image/format 不同。返回 move-only 包装，所有权交给调用方
    // -----------------------------------------------------------------
    UniqueVkHandle<VkImageView> createImageView(VkImage image, VkFormat format) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;    // 本章不做 mipmap：只有第 0 层
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView rawImageView;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &rawImageView));
        return UniqueVkHandle<VkImageView>(rawImageView, deviceDeleter(vkDestroyImageView));
    }

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            swapChainImageViews[i] = createImageView(swapChainImages[i], swapChainImageFormat);
        }
    }

    // -----------------------------------------------------------------
    // 渲染通道：一张颜色附件（交换链图），加载时清屏、存储后用于呈现
    // -----------------------------------------------------------------
    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // 开始时清屏
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;    // 结果保留
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        // 子通道依赖：交换链图刚从"呈现"转来，要等它可用再开始渲染
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        VkRenderPass rawRenderPass;
        VK_CHECK(vkCreateRenderPass(device, &renderPassInfo, nullptr, &rawRenderPass));
        renderPass = UniqueVkHandle<VkRenderPass>(rawRenderPass, deviceDeleter(vkDestroyRenderPass));
    }

    // -----------------------------------------------------------------
    // 【本章核心 1/4】两个描述符集布局 —— 按更新频率分层：
    //
    //   set 0（帧级）：binding 0 = uniform 缓冲（view + proj），顶点阶段可见。
    //     每帧内容都变 → 双帧各一套集合，各指向自己的 UBO（per-frame 双份）。
    //     注意：变的是缓冲【内容】（memcpy 进持续映射的内存），集合与缓冲的
    //     绑定关系初始化时写一次就不再动——每帧 vkUpdateDescriptorSets 次数 = 0
    //
    //   set 1（材质级）：binding 0 = combined image sampler，片段阶段可见。
    //     从加载到销毁不变 → 每种材质一套，初始化写一次，终生不改。
    //
    // 为什么要分？（对照入门课 18：UBO + 采样器塞同一个 set）
    //   若塞一个 set：每帧更新 UBO 就要动整份集合（双帧 × 每材质的组合数），
    //   且切材质会连带重绑帧数据。分层后各绑各的——vkCmdBindDescriptorSets
    //   的 firstSet 参数让 set 0 / set 1 的绑定互不覆盖，这正是引擎
    //   "per-frame / per-material / per-object" 三级资源绑定的标准做法
    // -----------------------------------------------------------------
    void createDescriptorSetLayouts() {
        // ---- set 0：帧级 UBO ----
        VkDescriptorSetLayoutBinding frameUboBinding{};
        frameUboBinding.binding = 0;                       // shader: layout(set=0, binding=0)
        frameUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        frameUboBinding.descriptorCount = 1;
        frameUboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;  // 顶点着色器读 VP
        frameUboBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo frameLayoutInfo{};
        frameLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        frameLayoutInfo.bindingCount = 1;
        frameLayoutInfo.pBindings = &frameUboBinding;

        VkDescriptorSetLayout rawFrameLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &frameLayoutInfo, nullptr, &rawFrameLayout));
        frameSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawFrameLayout, deviceDeleter(vkDestroyDescriptorSetLayout));

        // ---- set 1：材质级 combined image sampler ----
        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;                        // shader: layout(set=1, binding=0)
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;  // 片段着色器采样
        samplerBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo materialLayoutInfo{};
        materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        materialLayoutInfo.bindingCount = 1;
        materialLayoutInfo.pBindings = &samplerBinding;

        VkDescriptorSetLayout rawMaterialLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &materialLayoutInfo, nullptr, &rawMaterialLayout));
        materialSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawMaterialLayout, deviceDeleter(vkDestroyDescriptorSetLayout));
    }

    // -----------------------------------------------------------------
    // 装配图形管线。顺序：着色器 → 顶点装配 → 视口 → 光栅化 →
    //                      多重采样 → 颜色混合 → 管线布局 → 渲染通道
    // -----------------------------------------------------------------
    void createGraphicsPipeline() {
        auto vertShaderCode = readFile("shaders/advanced/04/vert.spv");
        auto fragShaderCode = readFile("shaders/advanced/04/frag.spv");

        // shader module 是"临时工"：管线装配完成后就没用了。
        // 交给 RAII，函数 return（哪怕中途抛异常）都会自动销毁
        UniqueVkHandle<VkShaderModule> vertShaderModule(
            createShaderModule(vertShaderCode), deviceDeleter(vkDestroyShaderModule));
        UniqueVkHandle<VkShaderModule> fragShaderModule(
            createShaderModule(fragShaderCode), deviceDeleter(vkDestroyShaderModule));

        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";             // 入口函数名

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        // 顶点输入：把 Vertex 的内存布局告诉管线，
        // 与 shader.vert 里的 layout(location=0/1) 严格对应
        auto bindingDescription = Vertex::getBindingDescription();
        auto attributeDescriptions = Vertex::getAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        // 图元装配：每 3 个顶点一个三角形
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // 视口与裁剪：声明为"动态状态"，录制命令时才给具体值
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // 光栅化：填充模式，背面剔除
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // 多重采样：关闭
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // 颜色混合：直接覆盖（不做透明混合）
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // 动态状态列表
        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // -------------------------------------------------------------
        // 【本章核心 2/4】管线布局：按 set 下标顺序挂上两个描述符布局 +
        // 第 03 章的 push constant range。
        //   setLayouts[0] → set 0（帧级），setLayouts[1] → set 1（材质级）
        //   —— 数组下标即 set 编号，shader 里 layout(set=N) 必须对得上
        // -------------------------------------------------------------
        std::array<VkDescriptorSetLayout, 2> setLayouts = {
            frameSetLayout, materialSetLayout
        };

        // push constant range：offset 0 起、80 字节、顶点和片段阶段都可见
        // （model 在顶点阶段用，tint 在片段阶段用——两个阶段都要声明，
        //  漏了 FRAGMENT 的话片段着色器读到的 tint 是未定义值）
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);   // 80 ≤ 128 下限保证

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        VkPipelineLayout rawPipelineLayout;
        VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &rawPipelineLayout));
        pipelineLayout = UniqueVkHandle<VkPipelineLayout>(rawPipelineLayout, deviceDeleter(vkDestroyPipelineLayout));

        // 总装配
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;   // 不派生

        VkPipeline rawPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline));
        graphicsPipeline = UniqueVkHandle<VkPipeline>(rawPipeline, deviceDeleter(vkDestroyPipeline));

        std::cout << "渲染通道 + 图形管线已创建（set 0 帧级 + set 1 材质级 + push constants）" << std::endl;
    }

    // -----------------------------------------------------------------
    // 帧缓冲：把"渲染通道 + 这张图的视图"绑定为一次渲染的目标。
    // 交换链有几张图就建几个，绘制时按帧下标取用
    // -----------------------------------------------------------------
    void createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());

        for (size_t i = 0; i < swapChainImageViews.size(); i++) {
            VkImageView attachments[] = { swapChainImageViews[i] };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;          // 按哪张图纸
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;       // 用哪张图
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;

            VkFramebuffer rawFramebuffer;
            VK_CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &rawFramebuffer));
            swapChainFramebuffers[i] = UniqueVkHandle<VkFramebuffer>(rawFramebuffer, deviceDeleter(vkDestroyFramebuffer));
        }
        std::cout << swapChainFramebuffers.size() << " 个帧缓冲已创建" << std::endl;
    }

    // -----------------------------------------------------------------
    // 命令池：命令缓冲的内存分配器。flag 带 RESET_COMMAND_BUFFER_BIT，
    // 允许单独重置缓冲（每帧重录）
    // -----------------------------------------------------------------
    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

        VkCommandPool rawPool;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &rawPool));
        commandPool = UniqueVkHandle<VkCommandPool>(rawPool, deviceDeleter(vkDestroyCommandPool));
    }

    // -----------------------------------------------------------------
    // 通用缓冲创建：对象 + 显存 + 绑定，三步合一。
    // 返回值是 BufferResource（move-only），移动语义负责把所有权交给调用方
    // -----------------------------------------------------------------
    BufferResource createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags properties) {
        BufferResource result;

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer rawBuffer;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &rawBuffer));
        result.buffer = UniqueVkHandle<VkBuffer>(rawBuffer, deviceDeleter(vkDestroyBuffer));

        // 查这个缓冲"需要"什么样的内存（类型位是驱动的内部编码）
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, result.buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        VkDeviceMemory rawMemory;
        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &rawMemory));
        result.memory = UniqueVkHandle<VkDeviceMemory>(rawMemory, deviceDeleter(vkFreeMemory));

        // 对象和内存是分开的，必须手动绑定
        vkBindBufferMemory(device, result.buffer, result.memory, 0);
        return result;   // 移动返回（move-only 类型不能拷贝）
    }

    // -----------------------------------------------------------------
    // 通用图像创建（本章，lessons/16 的 RAII 版）：与 createBuffer 同构，
    // 但多了 tiling（像素排布：LINEAR 可被 CPU 读 / OPTIMAL 仅供 GPU）。
    // 本章不做 mipmap：mipLevels 恒为 1
    // -----------------------------------------------------------------
    ImageResource createImage(uint32_t width, uint32_t height, VkFormat format,
                              VkImageTiling tiling, VkImageUsageFlags usage,
                              VkMemoryPropertyFlags properties) {
        ImageResource result;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;                      // 不做 mipmap（mipmap 见入门课 21）
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // 出生即"无身份"，等布局迁移
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkImage rawImage;
        VK_CHECK(vkCreateImage(device, &imageInfo, nullptr, &rawImage));
        result.image = UniqueVkHandle<VkImage>(rawImage, deviceDeleter(vkDestroyImage));

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, result.image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        VkDeviceMemory rawMemory;
        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &rawMemory));
        result.memory = UniqueVkHandle<VkDeviceMemory>(rawMemory, deviceDeleter(vkFreeMemory));

        vkBindImageMemory(device, result.image, result.memory, 0);
        return result;
    }

    // -----------------------------------------------------------------
    // 【本章核心 3/4】两种材质的纹理创建。
    //   材质 0：stb_image 解码 textures/texture.png
    //   材质 1：程序化棋盘格（CPU 填像素，不引入新文件）
    // 殊途同归：解码/生成出 RGBA 像素后，都走 createTextureFromPixels 的
    // staging 上传管线（lessons/16 流程）
    // -----------------------------------------------------------------
    void createTextures() {
        textures[0] = loadTextureFromFile("textures/texture.png");
        std::cout << "材质 0：textures/texture.png 已上传" << std::endl;

        textures[1] = createCheckerboardTexture();
        std::cout << "材质 1：程序化棋盘格已生成并上传" << std::endl;
    }

    // 从 PNG 文件加载纹理（stb_image）。强制 4 通道 RGBA，与
    // VK_FORMAT_R8G8B8A8_SRGB 对齐——不用关心源文件是 RGB 还是带 alpha
    Texture loadTextureFromFile(const char* path) {
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(path, &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error(std::string("加载纹理失败: ") + path);
        }

        Texture tex = createTextureFromPixels(pixels,
            static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
        stbi_image_free(pixels);   // 像素已进 staging，CPU 侧这份可以放了
        return tex;
    }

    // 程序化生成 256×256 棋盘格（8×8 格，黑白相间）。
    // 棋盘格是纹理管线调试的经典图案：格子边界锐利，过滤/UV 问题一眼可见
    Texture createCheckerboardTexture() {
        const uint32_t size = 256;
        const uint32_t cells = 8;

        std::vector<uint8_t> pixels(size * size * 4);
        for (uint32_t y = 0; y < size; y++) {
            for (uint32_t x = 0; x < size; x++) {
                // (行号 + 列号) 奇偶 = 棋盘格
                bool white = ((x * cells / size) + (y * cells / size)) % 2 == 0;
                uint8_t v = white ? 255 : 30;
                size_t idx = (y * size + x) * 4;
                pixels[idx + 0] = v;      // R
                pixels[idx + 1] = v;      // G
                pixels[idx + 2] = v;      // B
                pixels[idx + 3] = 255;    // A
            }
        }

        return createTextureFromPixels(pixels.data(), size, size);
    }

    // -----------------------------------------------------------------
    // 一张纹理的完整诞生（lessons/16~17 流程的 RAII 重写）：
    //   ① staging 缓冲（CPU 可写）  ← memcpy 像素
    //   ② VkImage（DEVICE_LOCAL + TILING_OPTIMAL）
    //   ③ 布局迁移 UNDEFINED → TRANSFER_DST_OPTIMAL（准备接收拷贝）
    //   ④ vkCmdCopyBufferToImage 把像素灌进去
    //   ⑤ 布局迁移 TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    //     （采样器只认这个布局；此后本图终生停在这个布局）
    //   ⑥ 建视图 + 采样器
    // RAII 差异：staging 是局部变量出作用域即毁；返回的 Texture 若中途
    // 抛异常，已建好的 image 也随局部对象自动析构——对照 lessons/16
    // 末尾手动的 vkDestroyBuffer + vkFreeMemory（异常路径上是泄漏的）
    // -----------------------------------------------------------------
    Texture createTextureFromPixels(const uint8_t* pixels, uint32_t width, uint32_t height) {
        Texture tex;

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

        // ① staging 缓冲：CPU 可写，作为拷贝源
        BufferResource staging = createBuffer(imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* data;
        VK_CHECK(vkMapMemory(device, staging.memory, 0, imageSize, 0, &data));
        memcpy(data, pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(device, staging.memory);

        // ② 图像对象：TILING_OPTIMAL + 用途"拷贝目的 | 可采样"
        tex.image = createImage(width, height, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // ③④⑤ 布局迁移链 + 像素拷贝（全部走一次性命令缓冲，等队列排空才返回）
        transitionImageLayout(tex.image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(staging.buffer, tex.image.image, width, height);
        transitionImageLayout(tex.image.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // staging 出作用域自动销毁（此时拷贝已完成，安全）

        // ⑥ 视图 + 采样器
        tex.view = createImageView(tex.image.image, VK_FORMAT_R8G8B8A8_SRGB);
        tex.sampler = createSampler();
        return tex;
    }

    // -----------------------------------------------------------------
    // 采样器：描述"怎么从图像取像素"——过滤、寻址、LOD。
    // 它独立于图像：同一张图可配不同采样器（图像管"存"，采样器管"取"）。
    // 本章不做 mipmap：maxLod = 0.0f，只允许采第 0 层
    // -----------------------------------------------------------------
    UniqueVkHandle<VkSampler> createSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;   // 放大：像素不够，线性插值
        samplerInfo.minFilter = VK_FILTER_LINEAR;   // 缩小：同样线性
        // 三个方向的寻址都是 REPEAT：UV 超出 [0,1] 就回绕——平铺贴图的基础
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // 各向异性（lessons/17）需要开设备特性 samplerAnisotropy，本章从简关闭
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;  // clamp 寻址时才用
        samplerInfo.unnormalizedCoordinates = VK_FALSE;   // 用归一化 UV（0~1）
        samplerInfo.compareEnable = VK_FALSE;             // 阴影比较，本章不用
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;    // 无 mipmap：锁死第 0 层（mipLevels=1 的配套）
        samplerInfo.mipLodBias = 0.0f;

        VkSampler rawSampler;
        VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &rawSampler));
        return UniqueVkHandle<VkSampler>(rawSampler, deviceDeleter(vkDestroySampler));
    }

    // -----------------------------------------------------------------
    // 顶点缓冲：staging 两级搬运（与 01 基座相同）。
    //   ① staging 缓冲（CPU 可写）  ← memcpy 顶点数据
    //   ② 顶点缓冲（GPU 专用显存）  ← GPU 队列异步拷贝
    // -----------------------------------------------------------------
    void createVertexBuffer() {
        VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

        BufferResource staging = createBuffer(bufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        void* data;
        VK_CHECK(vkMapMemory(device, staging.memory, 0, bufferSize, 0, &data));
        memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, staging.memory);

        vertexBuffer = createBuffer(bufferSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        copyBuffer(staging.buffer, vertexBuffer.buffer, bufferSize);
    }

    // 索引缓冲：和顶点缓冲同款 staging 模式，数据是 uint16 下标数组
    void createIndexBuffer() {
        VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

        BufferResource staging = createBuffer(bufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        void* data;
        VK_CHECK(vkMapMemory(device, staging.memory, 0, bufferSize, 0, &data));
        memcpy(data, indices.data(), static_cast<size_t>(bufferSize));
        vkUnmapMemory(device, staging.memory);

        indexBuffer = createBuffer(bufferSize,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        copyBuffer(staging.buffer, indexBuffer.buffer, bufferSize);
    }

    // 帧级 uniform 缓冲：每帧一份（CPU 写下一帧时 GPU 可能还在读上一帧）。
    // 每帧都要 CPU 写的数据保持 HOST_VISIBLE，创建后持续映射
    // （persistently mapped），省每帧 map/unmap 开销
    void createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(FrameUniformBuffer);

        for (auto& uniformBuffer : uniformBuffers) {
            uniformBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkMapMemory(device, uniformBuffer.memory, 0, bufferSize, 0, &uniformBuffer.mapped));
        }
    }

    // -----------------------------------------------------------------
    // 【本章核心 4/4】描述符池的库存估算：按"描述符类型 × 该类型出现在
    // 多少个集合里"申报，再申报集合总数上限 maxSets：
    //   UNIFORM_BUFFER         ：set 0 有 1 个 × 双帧 2 套   = 2
    //   COMBINED_IMAGE_SAMPLER ：set 1 有 1 个 × 2 种材质     = 2
    //   maxSets = 2 + 2 = 4
    // 池宁多勿少：多报只是池里留余量，少报则 vkAllocateDescriptorSets
    // 直接失败（VK_ERROR_OUT_OF_POOL_MEMORY）
    // -----------------------------------------------------------------
    void createDescriptorPool() {
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = MATERIAL_COUNT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT + MATERIAL_COUNT);

        VkDescriptorPool rawPool;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &rawPool));
        descriptorPool = UniqueVkHandle<VkDescriptorPool>(rawPool, deviceDeleter(vkDestroyDescriptorPool));
    }

    // -----------------------------------------------------------------
    // set 0（帧级）× 双帧：集合 i 指向 uniformBuffers[i]。
    // 集合只是"接线表"：记录"binding 0 → 哪个缓冲的哪段内存"。
    // 接线关系写完就不再动——每帧更新的是缓冲内容（memcpy 进 mapped），
    // 这是"每帧 vkUpdate* 调用次数 = 0"的第一半答案
    // -----------------------------------------------------------------
    void createFrameDescriptorSets() {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
        layouts.fill(frameSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocInfo.pSetLayouts = layouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, frameDescriptorSets.data()));

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(FrameUniformBuffer);

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = frameDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    // -----------------------------------------------------------------
    // set 1（材质级）× 材质数：每套记录"binding 0 → 该材质的 视图+采样器"。
    // 写完终生不改——这是"每帧 vkUpdate* 调用次数 = 0"的第二半答案。
    // imageLayout 填 SHADER_READ_ONLY_OPTIMAL：声明"采样时图像所处的布局"，
    // 必须与 createTextureFromPixels 里最后一次布局迁移的终点一致
    // -----------------------------------------------------------------
    void createMaterialDescriptorSets() {
        std::array<VkDescriptorSetLayout, MATERIAL_COUNT> layouts{};
        layouts.fill(materialSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = MATERIAL_COUNT;
        allocInfo.pSetLayouts = layouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, materialDescriptorSets.data()));

        for (size_t i = 0; i < MATERIAL_COUNT; i++) {
            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = textures[i].view;
            imageInfo.sampler = textures[i].sampler;

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = materialDescriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pImageInfo = &imageInfo;   // 图像类描述符走 pImageInfo

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    // 每帧更新帧级 UBO：只有 view + proj（model 已移交 push constants）。
    // proj 必须每帧重算：第 02 章起窗口可缩放，交换链重建后宽高比会变。
    // HOST_COHERENT 保证 memcpy 后 GPU 立即可见（无需 flush）
    void updateUniformBuffer(uint32_t frame) {
        FrameUniformBuffer ubo{};
        ubo.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 4.5f),     // 相机在 +Z 正视原点
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));    // Y 轴朝上
        ubo.proj = glm::perspective(glm::radians(45.0f),
                                    swapChainExtent.width / static_cast<float>(swapChainExtent.height),
                                    0.1f, 100.0f);
        ubo.proj[1][1] *= -1;   // GLM 面向 OpenGL 的 clip 空间（Y 向上），
                                // Vulkan NDC Y 向下——翻回来，否则画面上下颠倒

        memcpy(uniformBuffers[frame].mapped, &ubo, sizeof(ubo));
    }

    // 距程序启动的秒数：驱动 8 个物体的旋转动画
    static float elapsedSeconds() {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float, std::chrono::seconds::period>
            (currentTime - startTime).count();
    }

    // -----------------------------------------------------------------
    // 一次性命令缓冲：分配 → 开始(ONE_TIME_SUBMIT) → [录命令] → 结束
    // → 提交 → 等队列排空 → 释放。staging 拷贝 / 布局迁移共用
    // （lessons/16 抽出的 begin/end 复用模式）
    // -----------------------------------------------------------------
    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer));

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;  // 只交一次
        VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));

        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(graphicsQueue));   // 粗粒度等待：搬完再走（staging 马上就拆）

        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    // 缓冲 → 缓冲
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

        endSingleTimeCommands(commandBuffer);
    }

    // 缓冲 → 图像（staging 里的像素灌进 VkImage）
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;      // 0 = 紧密排布（行宽=图像宽）
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(commandBuffer, buffer, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        endSingleTimeCommands(commandBuffer);
    }

    // -----------------------------------------------------------------
    // 布局迁移：VkImage 的"身份切换"必须走管线屏障。
    // 屏障 = 内存屏障 + 执行屏障的合体：声明"旧布局的写已全部完成，
    // 新布局的读可以开始"，参数是管线阶段 + 访问掩码的组合。
    // 本章只有颜色图，两种迁移：
    //   UNDEFINED → TRANSFER_DST_OPTIMAL     （创建后准备接收像素）
    //   TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL （拷完交给采样器）
    // -----------------------------------------------------------------
    void transitionImageLayout(VkImage image,
                               VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;   // 不做所有权转移
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage, destinationStage;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            // 创建后的首次使用：不需要等任何前置写
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;       // 不用等
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;     // 到拷贝阶段可用
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            // 拷完 → 给 shader 读：等"拷贝写"完成
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            throw std::runtime_error("未支持的布局迁移");
        }

        vkCmdPipelineBarrier(commandBuffer,
                             sourceStage, destinationStage,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        endSingleTimeCommands(commandBuffer);
    }

    // 按"需求位 + 属性要求"在设备的内存类型表里找第一个匹配项。
    // DEVICE_LOCAL（GPU 专用，快，CPU 摸不着）/ HOST_VISIBLE（CPU 可写，慢）
    // staging 模式两种都用——各取所需
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("找不到满足要求的内存类型");
    }

    // 分配 MAX_FRAMES_IN_FLIGHT 条命令缓冲（每帧各用一条，互不干扰）。
    // 命令缓冲不单独包装：池销毁时自动释放
    void createCommandBuffers() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

        VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()));
    }

    // -----------------------------------------------------------------
    // 同步对象：信号量 + 栅栏。
    // 信号量只同步 GPU（acquire→submit→present 的接力）；
    // 栅栏同步 CPU 与 GPU（防止 CPU 跑太快把在飞的帧覆盖掉）。
    // 分两组：帧级（双份，终生不变）+ 图像级（按交换链图像数，重建时可变）
    // -----------------------------------------------------------------
    void createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 初始即"完成"，首帧 wait 不卡死

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkSemaphore rawSemaphore;
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rawSemaphore));
            imageAvailableSemaphores[i] = UniqueVkHandle<VkSemaphore>(rawSemaphore, deviceDeleter(vkDestroySemaphore));

            VkFence rawFence;
            VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &rawFence));
            inFlightFences[i] = UniqueVkHandle<VkFence>(rawFence, deviceDeleter(vkDestroyFence));
        }

        createPerImageSyncObjects();
    }

    // 图像级同步对象：renderFinished 信号量 + imagesInFlight 追踪表。
    // 交换链重建时图像数量可能变化，这组要跟着重建（首建时也是调它）。
    // 重建路径上调它之前已经 vkDeviceWaitIdle，旧信号量销毁安全
    void createPerImageSyncObjects() {
        renderFinishedSemaphores.clear();   // vector 清空 = 逐个销毁旧信号量
        imagesInFlight.clear();

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        renderFinishedSemaphores.resize(swapChainImages.size());
        for (auto& semaphore : renderFinishedSemaphores) {
            VkSemaphore rawSemaphore;
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rawSemaphore));
            semaphore = UniqueVkHandle<VkSemaphore>(rawSemaphore, deviceDeleter(vkDestroySemaphore));
        }

        imagesInFlight.assign(swapChainImages.size(), VK_NULL_HANDLE);
    }

    // -----------------------------------------------------------------
    // 交换链重建（第 02 章）：窗口尺寸变了，交换链/视图/帧缓冲都得按
    // 新尺寸重来。必须先 vkDeviceWaitIdle——旧交换链可能还有在飞的呈现
    // -----------------------------------------------------------------
    void recreateSwapChain() {
        // 最小化时 framebuffer 尺寸为 0：挂起等恢复，而不是崩掉
        int width = 0, height = 0;
        glfwGetFramebufferSize(window.handle, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window.handle, &width, &height);
            glfwWaitEvents();
        }

        VK_CHECK(vkDeviceWaitIdle(device));

        // 依赖旧交换链的对象先拆（RAII：clear() 即逐个销毁）：
        // 帧缓冲引用视图，视图引用交换链图像——按依赖顺序逆序拆
        swapChainFramebuffers.clear();
        swapChainImageViews.clear();

        {
            // oldSwapchain 接力：新链创建期间旧链必须存活。
            // 把旧句柄移进局部包装，createSwapChain 拿着它建新链；
            // 出了这个作用域，旧链自动销毁——入门课手动 vkDestroySwapchainKHR
            // 的 RAII 版
            UniqueVkHandle<VkSwapchainKHR> oldSwapChain = std::move(swapChain);
            createSwapChain(oldSwapChain.get());
        }

        createImageViews();
        createFramebuffers();
        createPerImageSyncObjects();   // 图像数量可能变了，图像级同步对象重建

        // 不用重建的：renderPass / pipeline / pipelineLayout（不依赖尺寸，
        // 视口是动态状态）、描述符与全部缓冲/纹理（与交换链无关）
    }

    // -----------------------------------------------------------------
    // 录制渲染命令。一帧的完整指令序列：
    // begin → beginRenderPass(清屏) → 视口/裁剪 → 绑管线 →
    // 绑 set 0（每帧一次）→ 逐物体 [绑 set 1 → push constants → draw] →
    // end → end
    // -----------------------------------------------------------------
    void recordCommandBuffer(VkCommandBuffer buffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        VK_CHECK(vkBeginCommandBuffer(buffer, &beginInfo));

        // 渲染通道开始：clear 值是"没有几何覆盖处的底色"
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent;
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(buffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // 动态视口/裁剪：录制时给定具体值
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent.width);
        viewport.height = static_cast<float>(swapChainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(buffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent;
        vkCmdSetScissor(buffer, 0, 1, &scissor);

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        // 绑定顶点缓冲：8 个物体共享同一份单位四边形几何
        VkBuffer vertexBuffers[] = { vertexBuffer.buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(buffer, 0, 1, vertexBuffers, offsets);

        // 索引缓冲（uint16 下标）：6 个下标画出 2 个三角形
        vkCmdBindIndexBuffer(buffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);

        // 【帧级】set 0 每帧只绑一次：firstSet = 0，绑 1 个集合。
        // 此后无论 set 1 怎么换，这份绑定都保持有效
        vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &frameDescriptorSets[currentFrame], 0, nullptr);

        // 逐物体绘制（第 03 章的结构保留）：切材质 → 推常量 → draw
        float time = elapsedSeconds();
        for (size_t i = 0; i < objects.size(); i++) {
            const SceneObject& obj = objects[i];

            // 【材质级】set 1：firstSet = 1 —— 只替换 set 1 的绑定，
            // 上面绑的 set 0 不受干扰（这正是 firstSet 参数的用途）。
            // 8 个物体交替绑定两种材质。实际引擎会按材质排序减少重绑；
            // 这里故意逐物体切换，展示三层绑定互不干扰的机制
            vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 1, 1, &materialDescriptorSets[i % MATERIAL_COUNT], 0, nullptr);

            // 【物体级】push constants：model（平移到网格位 + 绕自身中心
            // 旋转）+ tint。数据直接写进命令缓冲——这是 push constant 与
            // UBO 的本质区别：UBO 在队列执行时被读取，push constant 在
            // 录制时就被烙进命令缓冲
            ObjectPushConstants pc{};
            pc.model = glm::translate(glm::mat4(1.0f), glm::vec3(obj.position, 0.0f));
            pc.model = glm::rotate(pc.model, time * obj.rotationSpeed,
                                   glm::vec3(0.0f, 0.0f, 1.0f));
            pc.tint = obj.tint;
            vkCmdPushConstants(buffer, pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(ObjectPushConstants), &pc);

            vkCmdDrawIndexed(buffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(buffer);

        VK_CHECK(vkEndCommandBuffer(buffer));
    }

    static std::vector<char> readFile(const std::string& filename) {
        // ate = 打开后立即定位到文件尾，配合 tellg 直接拿到大小
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("打不开文件: " + filename);
        }
        size_t fileSize = static_cast<size_t>(file.tellg());
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    }

    // SPIR-V 字节码 → VkShaderModule（Vulkan 统一离线编译好，驱动只做加载）
    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule;
        VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));
        return shaderModule;
    }

    bool checkValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const char* layerName : validationLayers) {
            bool found = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        return true;
    }

    std::vector<const char*> getRequiredExtensions() {
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    // =========================================================================
    // drawFrame：每帧一遍 [等栅栏 → 采集 → 录制 → 提交 → 呈现] 流水线
    // （第 02 章版：acquire/present 返回过期/次优时重建交换链）
    //
    // 同步关系：栅栏管 CPU 不超前排；信号量管 GPU 内部接力
    //   acquire --imageAvailable--> submit --renderFinished--> present
    //
    // 本章验收点：整个函数里没有任何 vkUpdate* 调用——
    //   帧级数据：memcpy 进 uniformBuffers[currentFrame].mapped
    //   材质数据：初始化时写死，这里只绑不改
    //   物体数据：vkCmdPushConstants 随录制进命令缓冲
    // =========================================================================
    void drawFrame() {
        // 1. 等当前帧的栅栏：CPU 最多超前 MAX_FRAMES_IN_FLIGHT 帧
        VK_CHECK(vkWaitForFences(device, 1, inFlightFences[currentFrame].ptr(), VK_TRUE, UINT64_MAX));

        // 2. 采集交换链图像。
        // 第 02 章：窗口缩放会让交换链过期（VK_ERROR_OUT_OF_DATE_KHR）——
        // 立即重建并跳过本帧；SUBOPTIMAL（次优，比如尺寸对不上但还能画）
        // 继续画完这帧，重建留给 present 之后统一处理
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX,
            imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            VK_CHECK(result);   // 借 VK_CHECK 抛出带位置信息的异常
        }

        // 3. 这张图若还被更早的帧占用，先等那帧交还（imagesInFlight 追踪）
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            VK_CHECK(vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX));
        }
        imagesInFlight[imageIndex] = inFlightFences[currentFrame];

        // 4. 重置并录制本帧命令（栅栏也将随提交重新"挂起"）
        VK_CHECK(vkResetFences(device, 1, inFlightFences[currentFrame].ptr()));
        VK_CHECK(vkResetCommandBuffer(commandBuffers[currentFrame], 0));
        recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

        // 录制完、提交前更新本帧的帧级 UBO（写已映射的内存，
        // GPU 真正读到它是在队列执行时，时序安全）
        updateUniformBuffer(currentFrame);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
        // 等到"写颜色附件"这一步才需要图像就绪——前面的顶点阶段可以先跑
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers[currentFrame];
        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[imageIndex] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]));

        // 5. 呈现。第 02 章：过期/次优/回调旗标 → 重建交换链
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;      // 等渲染完成才上屏
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapChain();
        } else if (result != VK_SUCCESS) {
            VK_CHECK(result);
        }

        // 帧轮转
        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void mainLoop() {
        while (!glfwWindowShouldClose(window.handle)) {
            glfwPollEvents();
            drawFrame();
        }

        // 退出前等 GPU 收尾：否则接下来的自动析构会撞上还在执行的命令。
        // 这是手动 cleanup 时代遗留下来的**唯一**一句——它不属于"销毁"，
        // 而是"销毁的前置条件"，RAII 无法代替它
        VK_CHECK(vkDeviceWaitIdle(device));
    }
};

int main() {
    VulkanApp app;
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
