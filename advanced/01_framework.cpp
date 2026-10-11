// ===========================================================================
// 中高级 第 01 章：工程化重构 —— RAII 与完整资源生命周期
//
// 渲染内容与 lessons/15 相同（顶点着色的旋转四边形：顶点缓冲 + 索引缓冲 +
// UBO MVP 矩阵）。本章的重点不在画面，而在**代码结构**：把入门课"裸句柄 +
// 一长串手写 cleanup"的风格，改造成"对象生命周期即资源生命周期"的 RAII
// 风格。它是后续 02~20 章所有章节的基座。
//
// 核心组件：
//   VK_CHECK            —— 统一检查 VkResult，失败抛异常（带文件/行号）
//   UniqueVkHandle<T>   —— Vulkan 句柄的 RAII 包装（本章主角）
//   Window              —— GLFW 窗口也顺手 RAII
//   BufferResource      —— VkBuffer + VkDeviceMemory 打包成对
//
// RAII 带来的三个实际收益（可对照 lessons/22 的 cleanup() 阅读）：
//   1. 销毁顺序自动正确：C++ 保证成员按【声明的逆序】析构，而 Vulkan 要求
//      按【创建的逆序】销毁——两个逆序对消，"声明顺序 = 创建顺序"就是正确
//      的销毁顺序，写错顺序在成员区一眼能看出来
//   2. 异常安全：initVulkan 中途抛异常时，已创建的资源随部分构造的对象
//      一起自动析构（入门课裸句柄 + throw 的写法在这种路径上是泄漏的）
//   3. 局部资源自动回收：staging 缓冲、shader module 这类"临时工"，
//      出作用域即销毁，不用记着手动收
//
// 故意不做的事（留给后续章节）：
//   * 交换链重建（窗口缩放 → VK_ERROR_OUT_OF_DATE_KHR）→ 第 02 章
//   * 纹理 / 深度缓冲 → 第 03、04 章
// ===========================================================================
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// 数学库：Vulkan SDK 自带（Include/glm），无需额外安装
// GLM_FORCE_RADIANS：杜绝"角度制悄悄混进来"的历史包袱
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
// VK_CHECK：本章第一个工程化组件。
// 入门课里每个创建调用都写一遍 `if (vkCreateXxx(...) != VK_SUCCESS) throw
// std::runtime_error(...)`，几十处重复。宏统一收口：失败时抛出带
// **文件 + 行号 + 表达式文本 + 错误码**的异常，调试时一眼定位。
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
// UniqueVkHandle<T>：本章主角 —— Vulkan 句柄的 RAII 包装。
//
// 思路等价于 std::unique_ptr<T, Deleter>：句柄值 + 一个"知道怎么销毁它"
// 的函数对象。
//   * 默认构造 = 空句柄（VK_NULL_HANDLE），析构安全（什么都不做）
//   * 禁拷贝：Vulkan 句柄是独占资源，复制必然导致 double-destroy
//   * 移动构造/赋值 noexcept：能自由进出 std::vector / std::array，
//     也能作为工厂函数的返回值
//   * 析构自动调用销毁函数 —— 资源生命周期 = 包装对象的生命周期
//
// deleter 用 std::function<void(T)> 类型擦除：各类型的销毁函数签名不同
// （有的吃 device、有的吃 instance、debug messenger 还得动态加载），统一
// 成 void(T) 之后，包装类本身无需关心任何细节。代价是理论上一次间接调用
// ——教学代码里可读性优先；生产代码可用模板参数化 deleter 做到零开销。
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
// GLFW 窗口的 RAII 包装。它不是 Vulkan 句柄，但同样是"创建/销毁"配对的
// 资源，顺手一起收拾：析构时销毁窗口并 glfwTerminate()。
// ---------------------------------------------------------------------------
struct Window {
    GLFWwindow* handle = nullptr;

    Window(uint32_t width, uint32_t height, const char* title) {
        if (!glfwInit()) {
            throw std::runtime_error("GLFW 初始化失败");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // 不创建 OpenGL 上下文
        // 本章不做交换链重建（第 02 章的主题），直接禁止缩放窗口
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
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
        func(instance, debugMessenger, pAllocator);
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
// 顶点数据结构（与 lessons/15 相同）：一个顶点 = 位置(vec2) + 颜色(vec3)。
// 两个静态函数负责"翻译"——把 C++ 的内存布局告诉 GPU，与
// shaders/advanced/01/shader.vert 里的 layout(location=0/1) 一一对应
// ---------------------------------------------------------------------------
struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;

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
        // location 1 → inColor：vec3，跳过 pos 后(offset 8)取
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);
        return attributeDescriptions;
    }
};

// 矩形四个顶点：位置 + 颜色。
// 顺序按"屏幕空间"记 [上左, 下左, 下右, 上右] —— lessons/11 实测的正面绕序
const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},   // 0: 上左 红
    {{-0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},   // 1: 下左 绿
    {{ 0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},   // 2: 下右 蓝
    {{ 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}},   // 3: 上右 白
};

// 两个三角形共用顶点 0 和 2：4 顶点 + 6 下标。
// 注意绕序与无 UBO 的课程相反：proj 的 Y 翻转让 NDC 里方向倒转，
// 下标若不跟着反转，整个矩形会被背面剔除（lessons/15 实测黑屏的教训）
const std::vector<uint16_t> indices = {
    0, 2, 1,   // 左下三角
    0, 3, 2,   // 右上三角
};

// Uniform Buffer Object：C++ 和 shader.vert 两边必须 layout 一致
// （glm::mat4 各占 64 字节、对齐 16）。每帧由 CPU 更新一次
struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

// ---------------------------------------------------------------------------
// VkBuffer 与 VkDeviceMemory 是一对连体婴：缓冲对象只是"描述符"，真正装
// 字节的显存是另一实体。打包成一个小 struct 一起 RAII。
// 成员声明顺序决定析构顺序：buffer 后声明 → 先析构（vkDestroyBuffer），
// memory 先声明 → 后析构（vkFreeMemory），与入门课 cleanup 的顺序一致。
// ---------------------------------------------------------------------------
struct BufferResource {
    UniqueVkHandle<VkDeviceMemory> memory;
    UniqueVkHandle<VkBuffer> buffer;
    void* mapped = nullptr;   // uniform 缓冲的持续映射地址（随 memory 失效，不拥有）
};

class VulkanApp {
public:
    void run() {
        // 窗口由成员初始值构造（见成员区），Vulkan 在这里初始化
        initVulkan();
        mainLoop();
        // 没有 cleanup()！mainLoop 返回后 ~VulkanApp() 自动按成员声明逆序
        // 销毁所有资源——这正是本章的核心成果
    }

private:
    // =========================================================================
    // 成员声明区 —— 本章最重要的设计：
    //   C++ 保证成员按【声明的逆序】析构；Vulkan 要求按【创建的逆序】销毁。
    //   两个逆序对消 ⇒ 声明顺序 = 创建顺序，就是天然正确的销毁顺序：
    //
    //   声明(=创建)  window → instance → debugMessenger → surface → device →
    //                sync 对象 → commandPool → descriptorPool → buffers →
    //                swapChain → renderPass → pipelineLayout →
    //                imageViews → framebuffers → pipeline
    //   析构(=销毁)  完全反过来，与 lessons/22 手写的 cleanup() 顺序一致
    //
    // 不包装（明确不拥有）的句柄：
    //   VkPhysicalDevice / VkQueue      枚举/获取而来，无需销毁
    //   swapChainImages                 归交换链所有，随交换链销毁
    //   commandBuffers / descriptorSets 随各自的池销毁
    //   imagesInFlight                  只是 fence 的"观测记录"，不拥有
    // =========================================================================
    Window window{WIDTH, HEIGHT, "advanced 01 - RAII framework"};

    UniqueVkHandle<VkInstance> instance;
    UniqueVkHandle<VkDebugUtilsMessengerEXT> debugMessenger;   // 依赖 instance
    UniqueVkHandle<VkSurfaceKHR> surface;                      // 依赖 instance

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;          // 不拥有
    VkQueue graphicsQueue = VK_NULL_HANDLE;                    // 不拥有
    VkQueue presentQueue = VK_NULL_HANDLE;                     // 不拥有
    UniqueVkHandle<VkDevice> device;

    // ---- 以下全部依赖 device，都在 device 之后声明（先于 device 析构）----

    // 同步对象（lessons/22 的结构）：imageAvailable/fence 按帧，双缓冲；
    // renderFinished 按交换链图像（呈现引擎可能还拿着信号量，只有重新采集到
    // 同一张图才允许再 signal）；imagesInFlight 记录每张图被哪帧占用
    std::array<UniqueVkHandle<VkSemaphore>, MAX_FRAMES_IN_FLIGHT> imageAvailableSemaphores;
    std::vector<UniqueVkHandle<VkSemaphore>> renderFinishedSemaphores;
    std::array<UniqueVkHandle<VkFence>, MAX_FRAMES_IN_FLIGHT> inFlightFences;
    std::vector<VkFence> imagesInFlight;

    // 命令池（池销毁连带释放所有命令缓冲，故 commandBuffers 不包装）
    UniqueVkHandle<VkCommandPool> commandPool;
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers{};

    // 描述符：布局声明 + 池（集合随池释放，故 descriptorSets 不包装）
    UniqueVkHandle<VkDescriptorSetLayout> descriptorSetLayout;
    UniqueVkHandle<VkDescriptorPool> descriptorPool;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> descriptorSets{};

    // 几何与 uniform 数据
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

    uint32_t currentFrame = 0;   // 帧轮转下标

    // -----------------------------------------------------------------
    // deleter 工厂：生成"依赖 device 的销毁函数"。
    // 注意捕获的是 device 句柄的**值**而非 this：句柄本质是个不透明标识符，
    // 拷贝值不影响销毁目标；这样包装对象可以自由移动，不受 app 牵连。
    // 使用前提：device 已创建（所有 device 子对象的创建都在其之后）。
    // -----------------------------------------------------------------
    template <typename T>
    std::function<void(T)> deviceDeleter(
            void (*destroy)(VkDevice, T, const VkAllocationCallbacks*)) const {
        VkDevice dev = device;
        return [dev, destroy](T handle) { destroy(dev, handle, nullptr); };
    }

    // 同上，依赖 instance 的版本（surface 用；debug messenger 的销毁函数
    // 要动态加载，签名不匹配，不走这个模板）
    template <typename T>
    std::function<void(T)> instanceDeleter(
            void (*destroy)(VkInstance, T, const VkAllocationCallbacks*)) const {
        VkInstance inst = instance;
        return [inst, destroy](T handle) { destroy(inst, handle, nullptr); };
    }

    void initVulkan() {
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createImageViews();
        createRenderPass();
        createDescriptorSetLayout();   // 先声明布局，管线创建要用
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createVertexBuffer();          // staging 模式上显存
        createIndexBuffer();
        createUniformBuffers();        // 每帧一份的全局数据
        createDescriptorPool();
        createDescriptorSets();
        createCommandBuffers();
        createSyncObjects();
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
        appInfo.pApplicationName = "Advanced 01 - RAII Framework";
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

    void createSwapChain() {
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
        createInfo.oldSwapchain = VK_NULL_HANDLE;

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

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());

        for (size_t i = 0; i < swapChainImages.size(); i++) {
            VkImageViewCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            VkImageView rawImageView;
            VK_CHECK(vkCreateImageView(device, &createInfo, nullptr, &rawImageView));
            swapChainImageViews[i] = UniqueVkHandle<VkImageView>(rawImageView, deviceDeleter(vkDestroyImageView));
        }
    }

    // -----------------------------------------------------------------
    // 渲染通道：一张颜色附件（交换链图），加载时清屏、存储后用于呈现
    // -----------------------------------------------------------------
    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;           // 单采样（MSAA 是入门课收尾，本系列后议）
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
    // 描述符布局：声明"着色器将使用哪些资源"。这里只有一项：
    // binding 0 = uniform 缓冲，仅顶点阶段可见。
    // 管线创建时要按这份声明校验 shader，所以必须先于 createGraphicsPipeline
    // -----------------------------------------------------------------
    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;                       // 对应 shader 的 binding = 0
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;  // 顶点着色器专用
        uboLayoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &uboLayoutBinding;

        VkDescriptorSetLayout rawLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &rawLayout));
        descriptorSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawLayout, deviceDeleter(vkDestroyDescriptorSetLayout));
    }

    // -----------------------------------------------------------------
    // 装配图形管线。顺序：着色器 → 顶点装配 → 视口 → 光栅化 →
    //                      多重采样 → 颜色混合 → 管线布局 → 渲染通道
    // -----------------------------------------------------------------
    void createGraphicsPipeline() {
        auto vertShaderCode = readFile("shaders/advanced/01/vert.spv");
        auto fragShaderCode = readFile("shaders/advanced/01/frag.spv");

        // shader module 是"临时工"：管线装配完成后就没用了。
        // 入门课在函数末尾手动 vkDestroyShaderModule；这里交给 RAII，
        // 函数 return（哪怕中途抛异常）都会自动销毁
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

        // 管线布局：资源接口。挂上描述符布局，
        // 管线才知道"binding 0 有个 uniform 缓冲"
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        VkDescriptorSetLayout setLayout = descriptorSetLayout;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 0;

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

        std::cout << "渲染通道 + 图形管线已创建" << std::endl;
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
    // 顶点缓冲：staging 两级搬运（与 lessons/15 相同的渲染资源策略）。
    //   ① staging 缓冲（CPU 可写）  ← memcpy 顶点数据
    //   ② 顶点缓冲（GPU 专用显存）  ← GPU 队列异步拷贝
    // RAII 版的差异：staging 是局部变量，函数结束自动销毁——
    // 对照 lessons/15 末尾手动的 vkDestroyBuffer + vkFreeMemory
    // （而且那段手写代码在异常路径上是泄漏的）
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

    // uniform 缓冲：每帧一份（CPU 写下一帧时 GPU 可能还在读上一帧）。
    // 每帧都要 CPU 写的数据保持 HOST_VISIBLE，创建后持续映射
    // （persistently mapped），省每帧 map/unmap 开销
    void createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);

        for (auto& uniformBuffer : uniformBuffers) {
            uniformBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkMapMemory(device, uniformBuffer.memory, 0, bufferSize, 0, &uniformBuffer.mapped));
        }
    }

    // 描述符池：描述符集合的"分配器"。按类型和数量申报库存
    void createDescriptorPool() {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        VkDescriptorPool rawPool;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &rawPool));
        descriptorPool = UniqueVkHandle<VkDescriptorPool>(rawPool, deviceDeleter(vkDestroyDescriptorPool));
    }

    // 分配描述符集合，并把每帧的 uniform 缓冲"写"进对应的 binding 0。
    // 集合本身只是"接线表"：记录着"binding 0 → 哪个缓冲的哪段内存"。
    // 集合不单独包装：池销毁时自动释放（spec 保证）
    void createDescriptorSets() {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
        layouts.fill(descriptorSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocInfo.pSetLayouts = layouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, descriptorSets.data()));

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = descriptorSets[i];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    // 每帧更新：算 model/view/proj 三个矩阵，直接写进已映射的内存。
    // HOST_COHERENT 保证 memcpy 后 GPU 立即可见（无需 flush）
    void updateUniformBuffer(uint32_t frame) {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>
            (currentTime - startTime).count();

        UniformBufferObject ubo{};
        ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f),
                                glm::vec3(0.0f, 0.0f, 1.0f));          // 绕 Z 轴匀速旋转
        ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f),          // 相机位置
                               glm::vec3(0.0f, 0.0f, 0.0f),          // 看向原点
                               glm::vec3(0.0f, 0.0f, 1.0f));         // Z 轴朝上
        ubo.proj = glm::perspective(glm::radians(45.0f),
                                    swapChainExtent.width / static_cast<float>(swapChainExtent.height),
                                    0.1f, 10.0f);
        ubo.proj[1][1] *= -1;   // GLM 面向 OpenGL 的 clip 空间（Y 向上），
                                // Vulkan NDC Y 向下——翻回来，否则画面上下颠倒

        memcpy(uniformBuffers[frame].mapped, &ubo, sizeof(ubo));
    }

    // -----------------------------------------------------------------
    // 一次性命令缓冲：分配 → 开始(ONE_TIME_SUBMIT) → [录命令] → 结束
    // → 提交 → 等队列排空 → 释放。staging 拷贝用
    // -----------------------------------------------------------------
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
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

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VK_CHECK(vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(graphicsQueue));   // 粗粒度等待：搬完再走（staging 马上就拆）

        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
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
    // 同步对象：信号量 + 栅栏（lessons/22 的结构）。
    // 信号量只同步 GPU（acquire→submit→present 的接力）；
    // 栅栏同步 CPU 与 GPU（防止 CPU 跑太快把在飞的帧覆盖掉）
    // -----------------------------------------------------------------
    void createSyncObjects() {
        imagesInFlight.assign(swapChainImages.size(), VK_NULL_HANDLE);

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

        renderFinishedSemaphores.resize(swapChainImages.size());
        for (auto& semaphore : renderFinishedSemaphores) {
            VkSemaphore rawSemaphore;
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rawSemaphore));
            semaphore = UniqueVkHandle<VkSemaphore>(rawSemaphore, deviceDeleter(vkDestroySemaphore));
        }
    }

    // -----------------------------------------------------------------
    // 录制渲染命令。一帧的完整指令序列：
    // begin → beginRenderPass(清屏) → 视口/裁剪 → 绑定管线 → draw → end → end
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

        // 绑定顶点缓冲：告诉 GPU "接下来的 draw 从这个缓冲读顶点"
        VkBuffer vertexBuffers[] = { vertexBuffer.buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(buffer, 0, 1, vertexBuffers, offsets);

        // 索引缓冲（uint16 下标）：6 个下标画出 2 个三角形
        vkCmdBindIndexBuffer(buffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);

        // 绑定本帧的描述符集合（"这次绘制，binding 0 用这份接线表"）
        vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout, 0, 1, &descriptorSets[currentFrame], 0, nullptr);

        vkCmdDrawIndexed(buffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

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
    //
    // 同步关系：栅栏管 CPU 不超前排；信号量管 GPU 内部接力
    //   acquire --imageAvailable--> submit --renderFinished--> present
    // =========================================================================
    void drawFrame() {
        // 1. 等当前帧的栅栏：CPU 最多超前 MAX_FRAMES_IN_FLIGHT 帧
        VK_CHECK(vkWaitForFences(device, 1, inFlightFences[currentFrame].ptr(), VK_TRUE, UINT64_MAX));

        // 2. 采集交换链图像。
        // 窗口缩放会让交换链过期（VK_ERROR_OUT_OF_DATE_KHR）/ 次优——
        // 正确的应对是重建交换链，那是【第 02 章】的主题。
        // 本章窗口禁止缩放，把 SUBOPTIMAL 当成功继续画，其余非 SUCCESS 直接抛
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX,
            imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
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

        // 录制完、提交前更新本帧的 uniform 缓冲（写已映射的内存，
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

        // 5. 呈现。与采集同理：OUT_OF_DATE / SUBOPTIMAL 的重建处理留给
        // 【第 02 章】，本章 SUBOPTIMAL 继续，其余错误直接抛
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;      // 等渲染完成才上屏
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue, &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
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
