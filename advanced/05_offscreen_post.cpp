// ===========================================================================
// 中高级 第 05 章：离屏渲染与后处理入门（Offscreen Rendering + Post-Processing）
//
// 累积实现 = 01 基座（RAII）+ 02 交换链重建 + 03 push constants 多物体 +
//            04 三层描述符体系 + 本章的离屏与后处理。
//
// 本章起画面管线变成两段：
//
//   ┌─ Pass 1（离屏）─────────────────────────────────────────────┐
//   │  场景管线（第 03/04 章的场景原样保留）：8 个纹理 quad 各自      │
//   │  旋转/变色。渲染目标是自建的 color image（+ 深度附件），        │
//   │  不再是交换链图像                                            │
//   └──────────────┬──────────────────────────────────────────────┘
//                  ▼  color image（render pass 结束时转到 SHADER_READ_ONLY）
//   ┌─ Pass 2（上屏）─────────────────────────────────────────────┐
//   │  后处理管线：全屏三角形（无顶点输入，gl_VertexIndex 生成）       │
//   │  采样离屏图，按 push constant 的 int effect 做效果，           │
//   │  画进交换链帧缓冲                                            │
//   └─────────────────────────────────────────────────────────────┘
//
// 本章核心知识点：
//   1. 一张图两种用法：usage = COLOR_ATTACHMENT | SAMPLED，
//      身份切换靠 render pass 的 layout 转换（finalLayout = SHADER_READ_ONLY）
//   2. external subpass dependency 表达"写完才能读"
//      （COLOR_ATTACHMENT_OUTPUT/WRITE → FRAGMENT_SHADER/READ）
//   3. 两条管线两种布局：场景管线 = set0(帧)+set1(材质)+push(物体)；
//      后处理管线 = set0(离屏纹理)+push(int effect)，顶点输入态留空
//   4. 键盘 1/2/3/4 切换后处理效果（原样/灰度/反色/暗角）
//   5. 离屏资源组与交换链同尺寸——交换链重建时它们必须一并重建
//
// 操作：拖动窗口边框测试交换链重建（第 02 章成果）；
//       按 1/2/3/4 切换后处理效果（本章成果）。
// ===========================================================================
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// 数学库：Vulkan SDK 自带（Include/glm），无需额外安装
// GLM_FORCE_RADIANS：杜绝"角度制悄悄混进来"的历史包袱
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 图片解码：stb 单头文件（tools/include/stb_image.h，IMPLEMENTATION 宏全程序只此一处）
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

// 场景规模：8 个 quad（第 03 章的多物体渲染原样保留）
const uint32_t QUAD_COUNT = 8;

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

// 后处理效果表：下标 = effect 值（按键盘 1/2/3/4 得到 0/1/2/3）
const std::array<const char*, 4> effectNames = {
    "原样", "灰度", "反色", "暗角 vignette"
};

// ---------------------------------------------------------------------------
// VK_CHECK：统一检查 VkResult，失败抛异常（带文件/行号/表达式/错误码）。
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
// UniqueVkHandle<T>：Vulkan 句柄的 RAII 包装（第 01 章成果，原样沿用）。
//   * 默认构造 = 空句柄（VK_NULL_HANDLE），析构安全
//   * 禁拷贝、noexcept 移动：独占资源语义
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
// 第 02 章起不再禁止缩放（本章删掉 GLFW_RESIZABLE FALSE 这行），
// 交换链重建是本框架的基础能力。
// ---------------------------------------------------------------------------
struct Window {
    GLFWwindow* handle = nullptr;

    Window(uint32_t width, uint32_t height, const char* title) {
        if (!glfwInit()) {
            throw std::runtime_error("GLFW 初始化失败");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // 不创建 OpenGL 上下文
        // 第 02 章：允许缩放——resize 走 framebufferResized 旗标 + 交换链重建
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
// 顶点数据结构（lessons/18 版：位置 + 颜色 + 纹理坐标）。
// 与 shaders/advanced/05/sceneVert.vert 里的 layout(location=0/1/2) 一一对应
// ---------------------------------------------------------------------------
struct Vertex {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;                   // 绑定点编号（绑定缓冲时用）
        bindingDescription.stride = sizeof(Vertex);       // 相邻两顶点的字节间距
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;  // 逐顶点取数
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
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
        // location 2 → inTexCoord：vec2，跳过 pos+color 后(offset 20)取
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord);
        return attributeDescriptions;
    }
};

// 单位正方形四个顶点（半边长 0.5），位置/颜色/UV。
// 第 03 章起"8 个 quad"共享这一份几何：大小和位置全由 push constant 的
// model 矩阵给出（平移 + 旋转 + 缩放），顶点缓冲本身只有一张卡片
const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},   // 0: 上左，白
    {{-0.5f,  0.5f}, {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f}},   // 1: 下左，白
    {{ 0.5f,  0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f}},   // 2: 下右，白
    {{ 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},   // 3: 上右，白
};

// 两个三角形共用顶点 0 和 2：4 顶点 + 6 下标。
// 注意绕序与无 UBO 的课程相反：proj 的 Y 翻转让 NDC 里方向倒转，
// 下标若不跟着反转，整个矩形会被背面剔除（lessons/15 实测黑屏的教训）
const std::vector<uint16_t> indices = {
    0, 2, 1,   // 左下三角
    0, 3, 2,   // 右上三角
};

// ---------------------------------------------------------------------------
// 第 04 章的三层数据体系，C++ 侧结构：
//   帧级（set 0）   FrameUniforms    —— view/proj，每帧更新一次（UBO）
//   材质级（set 1） 纹理 + sampler   —— 分配一次终生不改（描述符集合）
//   物体级          ScenePushConstant —— 逐 quad 推送（push constants）
// ---------------------------------------------------------------------------
struct FrameUniforms {
    glm::mat4 view;
    glm::mat4 proj;
};

// 64 + 16 = 80 字节 < 128 字节下限保证（maxPushConstantsSize 至少 128）
struct ScenePushConstant {
    glm::mat4 model;
    glm::vec4 tint;
};

// 后处理 push constant：就一个 int——效果编号
struct PostPushConstant {
    int effect;
};

// 8 个 quad 的色调表（下标 = quad 编号）
const std::array<glm::vec4, QUAD_COUNT> quadTints = {
    glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},   // 白（原色）
    glm::vec4{1.0f, 0.4f, 0.4f, 1.0f},   // 红
    glm::vec4{0.4f, 1.0f, 0.4f, 1.0f},   // 绿
    glm::vec4{0.4f, 0.5f, 1.0f, 1.0f},   // 蓝
    glm::vec4{1.0f, 1.0f, 0.4f, 1.0f},   // 黄
    glm::vec4{1.0f, 0.4f, 1.0f, 1.0f},   // 品红
    glm::vec4{0.4f, 1.0f, 1.0f, 1.0f},   // 青
    glm::vec4{1.0f, 0.7f, 0.3f, 1.0f},   // 橙
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

// ---------------------------------------------------------------------------
// VkImage 版连体婴：对象 + 显存 + 视图三件套。
// 声明顺序 memory → image → view，析构时逆序 = view → image → memory，
// 正是 Vulkan 要求的销毁顺序。纹理、离屏 color、离屏 depth 都用它。
// ---------------------------------------------------------------------------
struct ImageResource {
    UniqueVkHandle<VkDeviceMemory> memory;
    UniqueVkHandle<VkImage> image;
    UniqueVkHandle<VkImageView> view;

    // 中途重建（交换链重建路径）时手动按正确顺序销毁。
    // 析构本身按声明逆序来、天然正确；此方法只是让"中途释放"也用同一顺序
    void reset() {
        view.reset();
        image.reset();
        memory.reset();
    }
};

class VulkanApp {
public:
    void run() {
        // 窗口事件回调（第 02 章 resize 旗标 + 本章按键切换效果）。
        // window 成员在 run() 之前已构造好，这里只装回调
        glfwSetWindowUserPointer(window.handle, this);
        glfwSetFramebufferSizeCallback(window.handle, framebufferResizeCallback);
        glfwSetKeyCallback(window.handle, keyCallback);
        std::cout << "按 1/2/3/4 切换后处理效果：原样 / 灰度 / 反色 / 暗角" << std::endl;

        initVulkan();
        mainLoop();
        // 没有 cleanup()！mainLoop 返回后 ~VulkanApp() 自动按成员声明逆序
        // 销毁所有资源（第 01 章的核心成果）
    }

private:
    // =========================================================================
    // 成员声明区 —— 声明顺序 = 创建顺序 = 析构（销毁）的逆序：
    //
    //   声明(=创建)  window → instance → debugMessenger → surface → device →
    //                sync 对象 → commandPool → 描述符布局/池 → buffers →
    //                纹理 → swapChain → renderPass × 2 → pipelineLayout × 2 →
    //                imageViews/framebuffers → 离屏资源组 → pipeline × 2
    //   析构(=销毁)  完全反过来，与入门课手写的 cleanup() 顺序一致
    //
    // 不包装（明确不拥有）的句柄：
    //   VkPhysicalDevice / VkQueue      枚举/获取而来，无需销毁
    //   swapChainImages                 归交换链所有，随交换链销毁
    //   commandBuffers / descriptorSets 随各自的池销毁
    //   imagesInFlight                  只是 fence 的"观测记录"，不拥有
    // =========================================================================
    Window window{WIDTH, HEIGHT, "advanced 05 - offscreen & post-processing"};

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
    // 同一张图才允许再 signal）；imagesInFlight 记录每张图被哪帧占用
    std::array<UniqueVkHandle<VkSemaphore>, MAX_FRAMES_IN_FLIGHT> imageAvailableSemaphores;
    std::vector<UniqueVkHandle<VkSemaphore>> renderFinishedSemaphores;
    std::array<UniqueVkHandle<VkFence>, MAX_FRAMES_IN_FLIGHT> inFlightFences;
    std::vector<VkFence> imagesInFlight;

    // 命令池（池销毁连带释放所有命令缓冲，故 commandBuffers 不包装）
    UniqueVkHandle<VkCommandPool> commandPool;
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> commandBuffers{};

    // 描述符体系（第 04 章分层）：三个布局 + 一个池。
    // 集合随池释放，故三个 descriptorSet 不包装
    UniqueVkHandle<VkDescriptorSetLayout> frameSetLayout;     // set 0：帧级 UBO
    UniqueVkHandle<VkDescriptorSetLayout> materialSetLayout;  // set 1：材质纹理
    UniqueVkHandle<VkDescriptorSetLayout> postSetLayout;      // 后处理：离屏纹理
    UniqueVkHandle<VkDescriptorPool> descriptorPool;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> frameDescriptorSets{};
    VkDescriptorSet materialDescriptorSet = VK_NULL_HANDLE;
    VkDescriptorSet postDescriptorSet = VK_NULL_HANDLE;

    // 几何与 uniform 数据
    BufferResource vertexBuffer;
    BufferResource indexBuffer;
    std::array<BufferResource, MAX_FRAMES_IN_FLIGHT> uniformBuffers;   // 每帧一份

    // 纹理（第 04 章；上传流程 = lessons/16~18 的 RAII 版）
    ImageResource textureImage;
    UniqueVkHandle<VkSampler> textureSampler;

    // 交换链：图像本身不包装（交换链拥有），format/extent 是纯数据
    UniqueVkHandle<VkSwapchainKHR> swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;

    // 两张渲染通道：交换链上屏（原 01 的）+ 离屏场景（本章新增）。
    // 都不依赖 extent（只关心 format），重建交换链时不用动
    UniqueVkHandle<VkRenderPass> renderPass;
    UniqueVkHandle<VkRenderPass> offscreenRenderPass;

    // 两套管线布局：场景（set0+set1+push）与后处理（set0+push）
    UniqueVkHandle<VkPipelineLayout> scenePipelineLayout;
    UniqueVkHandle<VkPipelineLayout> postPipelineLayout;

    // 依附交换链的视图与帧缓冲（重建交换链时重来）
    std::vector<UniqueVkHandle<VkImageView>> swapChainImageViews;
    std::vector<UniqueVkHandle<VkFramebuffer>> swapChainFramebuffers;

    // ---- 本章主角：离屏资源组 ----
    // 与交换链同尺寸（extent 变则必须重建），但不随交换链图像数量变化：
    // 只有一份，双帧共用（同步见 offscreenRenderPass 的进场依赖注释）
    ImageResource offscreenColor;    // 颜色：COLOR_ATTACHMENT | SAMPLED 双用途
    ImageResource offscreenDepth;    // 深度：只当附件，不采样
    UniqueVkHandle<VkSampler> offscreenSampler;   // 与尺寸无关，一次创建终生用
    UniqueVkHandle<VkFramebuffer> offscreenFramebuffer;

    // 两条管线：场景（画进离屏）+ 后处理（画进交换链）
    UniqueVkHandle<VkPipeline> scenePipeline;
    UniqueVkHandle<VkPipeline> postPipeline;

    uint32_t currentFrame = 0;          // 帧轮转下标
    bool framebufferResized = false;    // 第 02 章：resize 旗标
    int effect = 0;                     // 本章：后处理效果（键盘 1/2/3/4）

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
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createCommandPool();           // 提前：纹理/深度图的布局迁移都要用
        createSwapChain();
        createImageViews();
        createRenderPass();            // 交换链上屏通道
        createOffscreenRenderPass();   // 本章：离屏通道
        createDescriptorSetLayouts();  // 先声明布局，管线创建要用
        createOffscreenResources();    // 本章：离屏 color+depth（尺寸=extent）
        createOffscreenSampler();
        createOffscreenFramebuffer();
        createFramebuffers();
        createTextureImage();          // staging 模式上显存
        createTextureSampler();
        createVertexBuffer();
        createIndexBuffer();
        createUniformBuffers();        // 每帧一份的全局数据
        createDescriptorPool();
        createDescriptorSets();
        createScenePipeline();         // 场景管线 → 离屏
        createPostPipeline();          // 后处理管线 → 交换链
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
        appInfo.pApplicationName = "Advanced 05 - Offscreen Post";
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
                // 第 03 章：push constants 的大小上限，规范保证至少 128 字节。
                // 本章 ScenePushConstant 只用 80 字节，任何实现都装得下
                std::cout << "   maxPushConstantsSize = "
                          << props.limits.maxPushConstantsSize << " 字节" << std::endl;
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

        // 各向异性过滤（纹理采样器会查它；与 lessons/17 一致）
        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.samplerAnisotropy = VK_TRUE;

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
    // 交换链。第 02 章起支持重建：recreateSwapChain 把旧句柄传进来当
    // oldSwapchain，让驱动平滑过渡（入门课那个 VK_NULL_HANDLE 的坑在这填上）
    // -----------------------------------------------------------------
    void createSwapChain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE) {
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
        // 第 02 章：重建时传旧句柄，驱动可复用内部资源、切换不闪屏；
        // 首建时传 VK_NULL_HANDLE
        createInfo.oldSwapchain = oldSwapchain;

        VkSwapchainKHR rawSwapChain;
        VK_CHECK(vkCreateSwapchainKHR(device, &createInfo, nullptr, &rawSwapChain));
        // move-assign：若成员里还持有旧交换链，在这一步才被销毁——
        // 旧句柄已作为 oldSwapchain 完成"接力"，创建新的再销毁旧的，
        // 顺序符合 spec 要求
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

    // 通用图像视图创建：aspectMask 区分颜色/深度用途
    UniqueVkHandle<VkImageView> createImageView(VkImage image, VkFormat format,
                                                VkImageAspectFlags aspectMask) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = aspectMask;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VkImageView rawImageView;
        VK_CHECK(vkCreateImageView(device, &createInfo, nullptr, &rawImageView));
        return UniqueVkHandle<VkImageView>(rawImageView, deviceDeleter(vkDestroyImageView));
    }

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            swapChainImageViews[i] = createImageView(swapChainImages[i],
                swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    // -----------------------------------------------------------------
    // 上屏渲染通道（原 01 章的）：一张颜色附件（交换链图），
    // 加载时清屏、存储后用于呈现。后处理 pass 用它
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
        // 注意：没有深度附件——后处理只画一个全屏三角形，深度测试无意义

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
    // 本章核心①：离屏渲染通道。
    //
    // 与上屏通道的结构差异就三处，处处是知识点：
    //
    // 1. color 附件的 finalLayout = SHADER_READ_ONLY_OPTIMAL
    //    （上屏通道是 PRESENT_SRC_KHR）。渲染完这张图马上要被后处理 pass
    //    当纹理采样，render pass 结束时驱动自动做
    //    COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL 的布局转换，
    //    不用手写 vkCmdPipelineBarrier。
    //    为什么转换"发生在 render pass 结束时"：subpass 内部渲染要求
    //    COLOR_ATTACHMENT_OPTIMAL 布局，而 finalLayout 与这个内部布局不同，
    //    spec 规定 render pass 收尾时必须把附件迁到 finalLayout——这条
    //    隐式转换由下面的出场依赖（dependency[1]）排序与生效。
    //
    // 2. external subpass dependency 表达"写完才能读"：
    //    出场依赖 srcStage=COLOR_ATTACHMENT_OUTPUT / srcAccess=COLOR_ATTACHMENT_WRITE
    //         → dstStage=FRAGMENT_SHADER / dstAccess=SHADER_READ
    //    读法："本 render pass 在颜色附件输出阶段的写全部完成后，后续
    //    （本命令缓冲里的下一个 render pass）的片元着色器才允许采样它"。
    //    没有这条，后处理 pass 可能读到写了一半的画面（竞态/花屏）。
    //
    // 3. 多一条"进场依赖"：离屏图全程序只有一份，双帧并行共用。
    //    第 N+1 帧要重写它时，第 N 帧的后处理 pass 可能还在采样——
    //    进场依赖声明"上一帧读完才能写"（WAR 冒险），同一队列内按
    //    提交顺序匹配到之前的渲染工作。深度附件同理（帧间写-写冒险）。
    // -----------------------------------------------------------------
    void createOffscreenRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;    // 与交换链同格式
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 结果必须保留——下一个 pass 要采样它（上屏通道的 STORE 是为了呈现，
        // 这里的 STORE 是为了被读，理由不同、要求相同）
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // 每帧整幅重写，不要旧内容
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // ★ 本章关键

        // 深度附件（与 lessons/19 相同）：内容逐帧重建，store=丢弃
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = findDepthFormat();
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        std::array<VkSubpassDependency, 2> dependencies{};

        // 进场依赖（EXTERNAL → subpass 0）："读完才能写"
        // 上一帧后处理 pass 在 FRAGMENT_SHADER 阶段采样这张离屏图
        // （SHADER_READ），本帧要把它当颜色附件重写；深度附件则是
        // 帧间写-写。首帧没有"上一帧"，srcAccess 匹配不到任何操作，
        // 退化为纯布局转换——无害
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                     | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // 出场依赖（subpass 0 → EXTERNAL）："写完才能读"——本章主角
        // src：本 pass 颜色附件阶段的写；dst：之后片元着色器的采样读。
        // render pass 结束时的 finalLayout 隐式转换也由这条依赖排序
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();

        VkRenderPass rawRenderPass;
        VK_CHECK(vkCreateRenderPass(device, &renderPassInfo, nullptr, &rawRenderPass));
        offscreenRenderPass = UniqueVkHandle<VkRenderPass>(rawRenderPass, deviceDeleter(vkDestroyRenderPass));
    }

    // -----------------------------------------------------------------
    // 描述符布局 ×3（第 04 章分层 + 本章后处理）：
    //   frameSetLayout    set 0：帧级 UBO（view/proj），每帧更新
    //   materialSetLayout set 1：材质纹理，分配一次终生不改
    //   postSetLayout     后处理专用：离屏颜色纹理
    // -----------------------------------------------------------------
    void createDescriptorSetLayouts() {
        // set 0（场景）：binding 0 = uniform 缓冲，仅顶点阶段可见
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        uboLayoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo frameLayoutInfo{};
        frameLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        frameLayoutInfo.bindingCount = 1;
        frameLayoutInfo.pBindings = &uboLayoutBinding;

        VkDescriptorSetLayout rawFrameLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &frameLayoutInfo, nullptr, &rawFrameLayout));
        frameSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawFrameLayout, deviceDeleter(vkDestroyDescriptorSetLayout));

        // set 1（场景）：binding 0 = combined image sampler，片元阶段采样
        VkDescriptorSetLayoutBinding samplerLayoutBinding{};
        samplerLayoutBinding.binding = 0;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplerLayoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo materialLayoutInfo{};
        materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        materialLayoutInfo.bindingCount = 1;
        materialLayoutInfo.pBindings = &samplerLayoutBinding;

        VkDescriptorSetLayout rawMaterialLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &materialLayoutInfo, nullptr, &rawMaterialLayout));
        materialSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawMaterialLayout, deviceDeleter(vkDestroyDescriptorSetLayout));

        // 后处理：binding 0 = 离屏颜色纹理，片元阶段采样（结构与材质集合同款，
        // 但生命周期不同——交换链重建后要重写指向新离屏视图，所以单独一个布局）
        VkDescriptorSetLayoutCreateInfo postLayoutInfo{};
        postLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        postLayoutInfo.bindingCount = 1;
        postLayoutInfo.pBindings = &samplerLayoutBinding;

        VkDescriptorSetLayout rawPostLayout;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &postLayoutInfo, nullptr, &rawPostLayout));
        postSetLayout = UniqueVkHandle<VkDescriptorSetLayout>(rawPostLayout, deviceDeleter(vkDestroyDescriptorSetLayout));
    }

    // -----------------------------------------------------------------
    // 本章核心②：离屏资源组。
    //   color image：usage = COLOR_ATTACHMENT | SAMPLED —— 一张图两种用法，
    //     "既当渲染目标又当纹理"，layout 转换是正确性的关键（交给 render pass）
    //   depth image：与 lessons/19 的深度附件创建完全相同
    // 尺寸 = 交换链 extent ⇒ 交换链重建时本组必须一并重建（见 recreateSwapChain）
    // -----------------------------------------------------------------
    void createOffscreenResources() {
        // 颜色：格式与交换链相同（sRGB——写入时硬件编码、采样时硬件解码，
        // 两个 pass 走同一格式，往返无损）
        offscreenColor = createImage(swapChainExtent.width, swapChainExtent.height,
            swapChainImageFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        offscreenColor.view = createImageView(offscreenColor.image,
            swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        // 不做手动布局迁移：离屏 render pass 的 initialLayout=UNDEFINED +
        // finalLayout=SHADER_READ_ONLY 会在首次渲染时自动完成全部转换

        // 深度：只当附件（usage 没有 SAMPLED），逐帧重建、用完即弃
        VkFormat depthFormat = findDepthFormat();
        offscreenDepth = createImage(swapChainExtent.width, swapChainExtent.height,
            depthFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        offscreenDepth.view = createImageView(offscreenDepth.image,
            depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
        // 布局迁移（lessons/19 同款）：undefined → 深度附件
        transitionImageLayout(offscreenDepth.image, depthFormat,
                              VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    // 离屏采样器：与纹理采样器几乎同款，两处差别——
    //   寻址 CLAMP_TO_EDGE：采样的是"上一 pass 的画面"，UV 恒在 [0,1]，
    //     钳位防止边缘采样回绕出残影
    //   关各向异性：后处理按 1:1 屏幕比例采样，用不上
    // 采样器与尺寸无关，一次创建终生使用（交换链重建不动它）
    void createOffscreenSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = VK_FALSE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;    // 离屏图只有一层 mip
        samplerInfo.mipLodBias = 0.0f;

        VkSampler rawSampler;
        VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &rawSampler));
        offscreenSampler = UniqueVkHandle<VkSampler>(rawSampler, deviceDeleter(vkDestroySampler));
    }

    // 离屏帧缓冲：只有一份（不随交换链图像数变化），附件 = color + depth
    void createOffscreenFramebuffer() {
        std::array<VkImageView, 2> attachments = {
            offscreenColor.view,
            offscreenDepth.view
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = offscreenRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapChainExtent.width;
        framebufferInfo.height = swapChainExtent.height;
        framebufferInfo.layers = 1;

        VkFramebuffer rawFramebuffer;
        VK_CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &rawFramebuffer));
        offscreenFramebuffer = UniqueVkHandle<VkFramebuffer>(rawFramebuffer, deviceDeleter(vkDestroyFramebuffer));
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
        std::cout << swapChainFramebuffers.size() << " 个交换链帧缓冲已创建" << std::endl;
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

    // 通用图像创建（RAII 版）：与 createBuffer 同构，多了 tiling（像素排布）。
    // view 由调用方按需另建（交换链图像不走这条路径，它们的视图建在外部）
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
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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
    // 纹理图像（第 04 章引入；流程 = lessons/16 的 RAII 版）：
    // stb_image 读 PNG → staging 缓冲 → VkImage，两次布局迁移
    // -----------------------------------------------------------------
    void createTextureImage() {
        int texWidth, texHeight, texChannels;
        // 强制 4 通道（RGBA），与 VK_FORMAT_R8G8B8A8_SRGB 对齐
        stbi_uc* pixels = stbi_load("textures/texture.png",
                                    &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error("加载纹理 textures/texture.png 失败");
        }
        VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) * texHeight * 4;

        // ① staging 缓冲：CPU 可写，作为拷贝源（局部变量，出作用域自动销毁）
        BufferResource staging = createBuffer(imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* data;
        VK_CHECK(vkMapMemory(device, staging.memory, 0, imageSize, 0, &data));
        memcpy(data, pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(device, staging.memory);
        stbi_image_free(pixels);

        // ② 图像对象：TILING_OPTIMAL（GPU 专用排布）+ 用途"拷贝目的|可采样"
        textureImage = createImage(static_cast<uint32_t>(texWidth),
            static_cast<uint32_t>(texHeight), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        textureImage.view = createImageView(textureImage.image,
            VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT);

        // ③ 布局迁移 + 像素拷贝 + 再迁移（详见 transitionImageLayout 注释）
        transitionImageLayout(textureImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copyBufferToImage(staging.buffer, textureImage.image,
                          static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
        // 拷完 → 给 shader 读：GPU 采样器只认 SHADER_READ_ONLY 布局
        transitionImageLayout(textureImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // staging 是局部 BufferResource：函数结束自动销毁（对照 lessons/16
        // 末尾手动的 vkDestroyBuffer + vkFreeMemory）
    }

    // 纹理采样器（lessons/17）：过滤、寻址、各向异性
    void createTextureSampler() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;   // 放大：线性插值
        samplerInfo.minFilter = VK_FILTER_LINEAR;   // 缩小：同样线性
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // 各向异性：斜视角时沿压扁方向多采样。需设备特性支持（逻辑设备已开启）
        VkPhysicalDeviceFeatures supportedFeatures;
        vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
        samplerInfo.anisotropyEnable = supportedFeatures.samplerAnisotropy ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 1.0f;
        samplerInfo.mipLodBias = 0.0f;

        VkSampler rawSampler;
        VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &rawSampler));
        textureSampler = UniqueVkHandle<VkSampler>(rawSampler, deviceDeleter(vkDestroySampler));
    }

    // -----------------------------------------------------------------
    // 顶点缓冲：staging 两级搬运。
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

    // uniform 缓冲：每帧一份（CPU 写下一帧时 GPU 可能还在读上一帧）。
    // 每帧都要 CPU 写的数据保持 HOST_VISIBLE，创建后持续映射
    void createUniformBuffers() {
        VkDeviceSize bufferSize = sizeof(FrameUniforms);

        for (auto& uniformBuffer : uniformBuffers) {
            uniformBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkMapMemory(device, uniformBuffer.memory, 0, bufferSize, 0, &uniformBuffer.mapped));
        }
    }

    // 描述符池：按"类型 × 数量"申报库存。
    //   UBO × 每帧一份；COMBINED_IMAGE_SAMPLER × 2（材质 + 后处理各一）
    void createDescriptorPool() {
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 2;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) + 2;

        VkDescriptorPool rawPool;
        VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &rawPool));
        descriptorPool = UniqueVkHandle<VkDescriptorPool>(rawPool, deviceDeleter(vkDestroyDescriptorPool));
    }

    // 分配全部描述符集合并写入。集合 = "接线表"：记录 binding → 哪份资源。
    // 集合不单独包装：池销毁时自动释放（spec 保证）
    void createDescriptorSets() {
        // ---- set 0：每帧一份，写各自的 uniform 缓冲 ----
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> frameLayouts{};
        frameLayouts.fill(frameSetLayout);

        VkDescriptorSetAllocateInfo frameAllocInfo{};
        frameAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        frameAllocInfo.descriptorPool = descriptorPool;
        frameAllocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        frameAllocInfo.pSetLayouts = frameLayouts.data();

        VK_CHECK(vkAllocateDescriptorSets(device, &frameAllocInfo, frameDescriptorSets.data()));

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = uniformBuffers[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(FrameUniforms);

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

        // ---- set 1：材质集合，一份终生（第 04 章"按更新频率分层"的体现：
        // 每帧调用的 vkUpdate* 里根本没有它）----
        VkDescriptorSetAllocateInfo materialAllocInfo{};
        materialAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        materialAllocInfo.descriptorPool = descriptorPool;
        materialAllocInfo.descriptorSetCount = 1;
        VkDescriptorSetLayout materialLayout = materialSetLayout;
        materialAllocInfo.pSetLayouts = &materialLayout;

        VK_CHECK(vkAllocateDescriptorSets(device, &materialAllocInfo, &materialDescriptorSet));

        VkDescriptorImageInfo textureInfo{};
        textureInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        textureInfo.imageView = textureImage.view;
        textureInfo.sampler = textureSampler;

        VkWriteDescriptorSet materialWrite{};
        materialWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        materialWrite.dstSet = materialDescriptorSet;
        materialWrite.dstBinding = 0;
        materialWrite.dstArrayElement = 0;
        materialWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        materialWrite.descriptorCount = 1;
        materialWrite.pImageInfo = &textureInfo;

        vkUpdateDescriptorSets(device, 1, &materialWrite, 0, nullptr);

        // ---- 后处理集合：指向离屏颜色纹理 ----
        VkDescriptorSetAllocateInfo postAllocInfo{};
        postAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        postAllocInfo.descriptorPool = descriptorPool;
        postAllocInfo.descriptorSetCount = 1;
        VkDescriptorSetLayout postLayout = postSetLayout;
        postAllocInfo.pSetLayouts = &postLayout;

        VK_CHECK(vkAllocateDescriptorSets(device, &postAllocInfo, &postDescriptorSet));

        writePostDescriptorSet();
    }

    // 把后处理集合的 binding 0 接到离屏颜色视图上。
    // 抽成独立函数的原因：交换链重建后离屏视图是新建的，这个集合要重写
    // 一次（集合本身不失效，只换指向——比销毁重分便宜得多）
    void writePostDescriptorSet() {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // 离屏 pass 结束后的布局
        imageInfo.imageView = offscreenColor.view;
        imageInfo.sampler = offscreenSampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = postDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    // 每帧更新：view/proj 写进已映射的内存（HOST_COHERENT，无需 flush）。
    // 第 03 章起 model 不在这里——它逐物体走 push constants；
    // view/proj 才是"全帧共享、每帧一次"的数据
    void updateUniformBuffer(uint32_t frame) {
        FrameUniforms ubo{};
        // 俯视一点看 4×2 的 quad 墙（相机位置/朝向在整个程序里不变，
        // 但每帧照写——UBO 的价值在视角会动的时候才体现）
        ubo.view = glm::lookAt(glm::vec3(0.0f, 2.6f, 4.5f),    // 相机位置
                               glm::vec3(0.0f, 0.0f, 0.0f),    // 看向原点
                               glm::vec3(0.0f, 1.0f, 0.0f));   // Y 轴朝上
        ubo.proj = glm::perspective(glm::radians(45.0f),
                                    swapChainExtent.width / static_cast<float>(swapChainExtent.height),
                                    0.1f, 20.0f);
        ubo.proj[1][1] *= -1;   // GLM 面向 OpenGL 的 clip 空间（Y 向上），
                                // Vulkan NDC Y 向下——翻回来，否则画面上下颠倒

        memcpy(uniformBuffers[frame].mapped, &ubo, sizeof(ubo));
    }

    // -----------------------------------------------------------------
    // 一次性命令缓冲：分配 → 开始(ONE_TIME_SUBMIT) → [录命令] → 结束
    // → 提交 → 等队列排空 → 释放。staging 拷贝 / 布局迁移共用
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
        VK_CHECK(vkQueueWaitIdle(graphicsQueue));   // 粗粒度等待：搬完再走

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
    // （本章的离屏 color 不用它——身份切换由 render pass 自动完成，
    //   对比一下两种方式的手写量）
    // -----------------------------------------------------------------
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;   // 不做所有权转移
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        // 深度格式要用 DEPTH（带模板的加 STENCIL），颜色图才是 COLOR
        if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.subresourceRange.aspectMask = hasStencilComponent(format)
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_DEPTH_BIT;
        } else {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
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
        } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                   newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            // 深度缓冲首次使用。写深度发生在"早期片元测试"阶段
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
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

    // 深度格式三选一：优先纯 32 位浮点深度；带模板位的格式排最后
    VkFormat findDepthFormat() {
        return findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                 VkFormatFeatureFlags features) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
                return format;
            }
            if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
                return format;
            }
        }
        throw std::runtime_error("找不到满足要求的格式");
    }

    // 带 S8（模板）位的格式：涉及模板操作时 aspectMask 要加 STENCIL
    bool hasStencilComponent(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    // -----------------------------------------------------------------
    // 场景管线（第 03/04 章的管线，渲染目标改为离屏 render pass）。
    // 布局：set 0（帧级 UBO）+ set 1（材质纹理）+ push constant（物体级）
    // -----------------------------------------------------------------
    void createScenePipeline() {
        // 着色器命名约定：sceneVert.vert → sceneVert.spv（CMake 规则是
        // "去掉 .vert/.frag 再加 .spv"——同目录下 scene.vert 与 scene.frag
        // 会撞成同一个 scene.spv 互相覆盖，所以两个阶段都带阶段后缀）
        auto vertShaderCode = readFile("shaders/advanced/05/sceneVert.spv");
        auto fragShaderCode = readFile("shaders/advanced/05/sceneFrag.spv");

        // shader module 是"临时工"：管线装配完成后就没用了，
        // RAII 负责函数 return（哪怕中途抛异常）时自动销毁
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

        // 顶点输入：Vertex（pos+color+texCoord）布局
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

        // 深度模板状态（lessons/19）：开深度测试——更近者胜，写深度。
        // 8 个 quad 有前后层次（z 错开），没有它会按提交顺序互相覆盖
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

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

        // 管线布局：set 0（帧级）+ set 1（材质）+ push constant range（物体级）。
        // 第 03 章知识点：一个布局可以同时挂描述符集合和 push constant range
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ScenePushConstant);   // 80 字节

        std::array<VkDescriptorSetLayout, 2> setLayouts = { frameSetLayout, materialSetLayout };
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
        pipelineLayoutInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        VkPipelineLayout rawPipelineLayout;
        VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &rawPipelineLayout));
        scenePipelineLayout = UniqueVkHandle<VkPipelineLayout>(rawPipelineLayout, deviceDeleter(vkDestroyPipelineLayout));

        // 总装配：renderPass 是【离屏】通道——这条管线的输出进离屏图
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = scenePipelineLayout;
        pipelineInfo.renderPass = offscreenRenderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        VkPipeline rawPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline));
        scenePipeline = UniqueVkHandle<VkPipeline>(rawPipeline, deviceDeleter(vkDestroyPipeline));

        std::cout << "场景管线已创建（目标：离屏帧缓冲）" << std::endl;
    }

    // -----------------------------------------------------------------
    // 本章核心③：后处理管线。
    // 与场景管线的三处关键差异：
    //   1. 顶点输入态整个留空——全屏三角形的顶点由 gl_VertexIndex 在
    //      着色器里生成，不绑任何顶点缓冲
    //   2. 关背面剔除 + 关深度测试（上屏通道没有深度附件，
    //      开深度测试会被校验层拦下）
    //   3. 独立管线布局：set 0 = 离屏纹理，push constant = int effect
    // -----------------------------------------------------------------
    void createPostPipeline() {
        // 命名同场景管线：postVert.vert → postVert.spv
        auto vertShaderCode = readFile("shaders/advanced/05/postVert.spv");
        auto fragShaderCode = readFile("shaders/advanced/05/postFrag.spv");

        UniqueVkHandle<VkShaderModule> vertShaderModule(
            createShaderModule(vertShaderCode), deviceDeleter(vkDestroyShaderModule));
        UniqueVkHandle<VkShaderModule> fragShaderModule(
            createShaderModule(fragShaderCode), deviceDeleter(vkDestroyShaderModule));

        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        // ★ 顶点输入态留空：binding/attribute 计数全 0。
        // draw(3) 产生的三个顶点不读任何缓冲，位置全由 gl_VertexIndex 算出
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 0;
        vertexInputInfo.pVertexBindingDescriptions = nullptr;
        vertexInputInfo.vertexAttributeDescriptionCount = 0;
        vertexInputInfo.pVertexAttributeDescriptions = nullptr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        // 全屏三角形只有一个面，剔除开着也行——但"后处理永远不该被剔除"
        // 是更稳的默认（比如以后把三角形绕序写反了也不会黑屏）
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // 显式关掉深度测试：上屏 render pass 没有深度附件，
        // 开着会被校验层报"管线与 render pass 不兼容"
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // 后处理布局：set 0 = 离屏纹理；push constant = int effect（仅片元用）
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(PostPushConstant);   // 4 字节

        VkDescriptorSetLayout postSet = postSetLayout;
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &postSet;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

        VkPipelineLayout rawPipelineLayout;
        VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &rawPipelineLayout));
        postPipelineLayout = UniqueVkHandle<VkPipelineLayout>(rawPipelineLayout, deviceDeleter(vkDestroyPipelineLayout));

        // 总装配：renderPass 是【上屏】通道——输出进交换链帧缓冲
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = postPipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        VkPipeline rawPipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rawPipeline));
        postPipeline = UniqueVkHandle<VkPipeline>(rawPipeline, deviceDeleter(vkDestroyPipeline));

        std::cout << "后处理管线已创建（目标：交换链帧缓冲）" << std::endl;
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

    // 第 02 章：交换链重建后图像数量可能变化，renderFinished 信号量跟着重建
    // （clear() 即逐个销毁——RAII 版的"先删再建"）
    void recreateRenderFinishedSemaphores() {
        renderFinishedSemaphores.clear();
        renderFinishedSemaphores.resize(swapChainImages.size());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (auto& semaphore : renderFinishedSemaphores) {
            VkSemaphore rawSemaphore;
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &rawSemaphore));
            semaphore = UniqueVkHandle<VkSemaphore>(rawSemaphore, deviceDeleter(vkDestroySemaphore));
        }
        imagesInFlight.assign(swapChainImages.size(), VK_NULL_HANDLE);
    }

    // -----------------------------------------------------------------
    // 第 02 章：交换链重建。窗口尺寸变了，图/视图/帧缓冲都得按新尺寸重来。
    // 本章追加：离屏资源组与交换链同尺寸（createOffscreenResources 用的
    // 就是 swapChainExtent），必须一并重建——否则新窗口尺寸下采样的是
    // 旧尺寸的图（花屏/校验层报错）。
    // 哪些不用重建：render pass 与 pipeline（只依赖 format 不依赖尺寸）、
    // 采样器、纹理、顶点/uniform 缓冲、命令/同步对象（数量不变的那些）。
    // -----------------------------------------------------------------
    void recreateSwapChain() {
        // 最小化时 extent 为 0：挂起事件循环等恢复，而不是带着 0 尺寸硬建
        int width = 0, height = 0;
        glfwGetFramebufferSize(window.handle, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window.handle, &width, &height);
            glfwWaitEvents();
        }

        // 旧交换链可能还有在飞的呈现/渲染，先等 GPU 排空再动刀
        VK_CHECK(vkDeviceWaitIdle(device));

        // 1. 清掉依赖旧交换链/旧尺寸的一切（析构顺序由 RAII 保证）：
        //    帧缓冲 → 离屏帧缓冲/离屏图 → 交换链视图。交换链本身留着——
        //    下一步要拿它当 oldSwapchain
        swapChainFramebuffers.clear();
        offscreenFramebuffer.reset();
        offscreenColor.reset();
        offscreenDepth.reset();
        swapChainImageViews.clear();

        // 2. 旧句柄传入当 oldSwapchain（驱动可复用资源）；
        //    createSwapChain 里 move-assign 那一刻旧交换链才被销毁
        createSwapChain(swapChain);
        createImageViews();

        // 3. 本章：离屏资源组随交换链尺寸重建，并重写后处理描述符指向新视图
        createOffscreenResources();
        createOffscreenFramebuffer();
        writePostDescriptorSet();

        // 4. 交换链帧缓冲 + 按图像数量的信号量
        createFramebuffers();
        recreateRenderFinishedSemaphores();
    }

    // -----------------------------------------------------------------
    // 录制渲染命令（本章改写为一帧两段）：
    //   Pass 1（离屏）：场景管线画 8 个 quad → 离屏 color/depth
    //   Pass 2（上屏）：后处理管线画全屏三角形 → 交换链帧缓冲
    // -----------------------------------------------------------------
    void recordCommandBuffer(VkCommandBuffer buffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        VK_CHECK(vkBeginCommandBuffer(buffer, &beginInfo));

        // 视口/裁剪：两个 pass 尺寸相同（离屏与交换链同 extent），设一次即可
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

        // ===================== Pass 1：场景 → 离屏 =====================
        VkRenderPassBeginInfo offscreenPassInfo{};
        offscreenPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        offscreenPassInfo.renderPass = offscreenRenderPass;
        offscreenPassInfo.framebuffer = offscreenFramebuffer;
        offscreenPassInfo.renderArea.offset = {0, 0};
        offscreenPassInfo.renderArea.extent = swapChainExtent;
        // clear 值数组索引与附件数组对应：0=颜色，1=深度
        std::array<VkClearValue, 2> offscreenClearValues{};
        offscreenClearValues[0].color = {{0.02f, 0.02f, 0.06f, 1.0f}};  // 深蓝底
        offscreenClearValues[1].depthStencil = {1.0f, 0};               // 深度清为最远
        offscreenPassInfo.clearValueCount = static_cast<uint32_t>(offscreenClearValues.size());
        offscreenPassInfo.pClearValues = offscreenClearValues.data();

        vkCmdBeginRenderPass(buffer, &offscreenPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipeline);

        // 绑定顶点/索引缓冲：8 个 quad 共享同一份几何
        VkBuffer vertexBuffers[] = { vertexBuffer.buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(buffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(buffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);

        // 一次调用绑两层集合：set 0 本帧 UBO + set 1 材质纹理
        std::array<VkDescriptorSet, 2> sceneSets = {
            frameDescriptorSets[currentFrame], materialDescriptorSet
        };
        vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                scenePipelineLayout, 0,
                                static_cast<uint32_t>(sceneSets.size()),
                                sceneSets.data(), 0, nullptr);

        // 第 03 章：逐物体 push constants 后 draw。8 个 quad 排成 4×2 的墙，
        // 各自绕自己的中心旋转（速度/方向/色调各异），z 错开制造前后遮挡
        float time = appTime();
        for (uint32_t i = 0; i < QUAD_COUNT; i++) {
            float col = static_cast<float>(i % 4);
            float row = static_cast<float>(i / 4);
            ScenePushConstant pc{};
            pc.model = glm::translate(glm::mat4(1.0f),
                glm::vec3((col - 1.5f) * 1.3f, 0.75f - row * 1.5f,
                          (i % 2 == 0) ? 0.35f : -0.35f));
            float dir = (i % 3 == 0) ? -1.0f : 1.0f;
            pc.model = glm::rotate(pc.model, time * (0.5f + 0.17f * i) * dir,
                                   glm::vec3(0.0f, 0.0f, 1.0f));
            pc.model = glm::scale(pc.model, glm::vec3(1.2f));
            pc.tint = quadTints[i];

            vkCmdPushConstants(buffer, scenePipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(ScenePushConstant), &pc);
            vkCmdDrawIndexed(buffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        }

        // render pass 结束 = 离屏 color 在此刻隐式转到 SHADER_READ_ONLY_OPTIMAL
        // （由 offscreenRenderPass 的出场依赖排序，"写完才能读"）
        vkCmdEndRenderPass(buffer);

        // ===================== Pass 2：后处理 → 交换链 =====================
        VkRenderPassBeginInfo presentPassInfo{};
        presentPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        presentPassInfo.renderPass = renderPass;
        presentPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        presentPassInfo.renderArea.offset = {0, 0};
        presentPassInfo.renderArea.extent = swapChainExtent;
        // 清黑：全屏三角形会覆盖每个像素，这个颜色理论上永远看不到，
        // 但 loadOp=CLEAR 仍要求提供 clear 值
        VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        presentPassInfo.clearValueCount = 1;
        presentPassInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(buffer, &presentPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline);

        // set 0 = 离屏颜色纹理（此刻它已是 SHADER_READ_ONLY 布局）
        vkCmdBindDescriptorSets(buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                postPipelineLayout, 0, 1, &postDescriptorSet, 0, nullptr);

        // 后处理效果选择：键盘 1/2/3/4 写入 effect，逐帧推给片元着色器
        PostPushConstant postPc{ effect };
        vkCmdPushConstants(buffer, postPipelineLayout,
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PostPushConstant), &postPc);

        // 全屏三角形：3 个顶点，无顶点缓冲、无索引缓冲
        vkCmdDraw(buffer, 3, 1, 0, 0);

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

    // 程序运行时长（秒）：场景旋转动画的时间源
    static float appTime() {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float, std::chrono::seconds::period>
            (currentTime - startTime).count();
    }

    // -----------------------------------------------------------------
    // 窗口回调（static：GLFW 是 C 库，回调不能带 this，靠 user pointer 找回）
    // -----------------------------------------------------------------

    // 第 02 章：尺寸变化只立旗标，重建推迟到下一次 drawFrame
    // （回调里直接重建会踩坑——交换链可能正被用）
    static void framebufferResizeCallback(GLFWwindow* w, int, int) {
        auto* app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(w));
        app->framebufferResized = true;
    }

    // 本章：按 1/2/3/4 切换后处理效果
    static void keyCallback(GLFWwindow* w, int key, int, int action, int) {
        auto* app = reinterpret_cast<VulkanApp*>(glfwGetWindowUserPointer(w));
        if (action == GLFW_PRESS && key >= GLFW_KEY_1 && key <= GLFW_KEY_4) {
            app->effect = key - GLFW_KEY_1;
            std::cout << "后处理效果 → " << effectNames[app->effect] << std::endl;
        }
    }

    // =========================================================================
    // drawFrame：每帧一遍 [等栅栏 → 采集 → 录制 → 提交 → 呈现] 流水线
    //
    // 同步关系：栅栏管 CPU 不超前排；信号量管 GPU 内部接力
    //   acquire --imageAvailable--> submit --renderFinished--> present
    // 两个 render pass 在同一命令缓冲内顺序执行，它们之间的同步由
    // offscreenRenderPass 的 subpass dependency 负责（不需要额外信号量）
    // =========================================================================
    void drawFrame() {
        // 1. 等当前帧的栅栏：CPU 最多超前 MAX_FRAMES_IN_FLIGHT 帧
        VK_CHECK(vkWaitForFences(device, 1, inFlightFences[currentFrame].ptr(), VK_TRUE, UINT64_MAX));

        // 2. 采集交换链图像。第 02 章：OUT_OF_DATE = 交换链已过期，
        // 重建后本帧直接放弃（下帧再来）；SUBOPTIMAL 当成功继续画，
        // present 时再触发重建
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

        // 5. 呈现。第 02 章：OUT_OF_DATE / SUBOPTIMAL / resize 旗标 → 重建
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
        // 它不属于"销毁"，而是"销毁的前置条件"，RAII 无法代替
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
