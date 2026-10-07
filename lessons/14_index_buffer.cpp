// ===========================================================================
// 第十四课：索引缓冲（Index Buffer）—— 顶点复用
//
// 新增内容：
//   indices 数组        —— 用下标而非顶点序列描述三角形
//   createIndexBuffer() —— 与顶点缓冲同款 staging 模式，usage 换成 INDEX_BUFFER
//   vkCmdDrawIndexed    —— 绑定索引缓冲 + 按下标绘制
//
// 知识点：
//   * 矩形 = 2 个三角形 = 6 个顶点位置，但左下/右上两个角被两个三角形
//     重复引用——索引缓冲让"4 个顶点 + 6 个下标"替代"6 个顶点"
//   * 顶点带的数据越多（位置/法线/UV/颜色），省显存省带宽越明显
//   * 绕序课复习：本项目实测 front = NDC 顺时针（Y 轴向下翻折所致），
//     顶点按下表 [上左,下左,下右,上右] 排列，索引 0,1,2 / 0,2,3 即安全
//   * VK_INDEX_TYPE_UINT16：16 位下标（6 万+ 顶点够用；更大模型用 UINT32）
//
// 着色器沿用 shaders/12/（本课只改数据组织，不改渲染）。
// ===========================================================================
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

// 数学库：Vulkan SDK 自带（Include/glm），无需额外安装
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <vector>

const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

// 允许同时在 GPU 上执行的帧数：2 = 绘制第 N+1 帧时第 N 帧还在光栅化
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

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {

    std::cerr << "[vulkan] " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

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
// 第十二课主角：顶点数据结构。
// 一个顶点 = 位置(vec2) + 颜色(vec3)，在内存里连续排 20 字节。
// 下面两个静态函数负责"翻译"——把 C++ 的布局告诉 GPU：
//   getBindingDescription()    缓冲整体怎么读（步长 = sizeof(Vertex)）
//   getAttributeDescriptions() 每个顶点内部怎么拆（哪段是位置、哪段是颜色）
// 与 shaders/12/shader.vert 里的 layout(location=0/1) 一一对应
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
// 顺序按"屏幕空间"记 [上左, 下左, 下右, 上右] —— 本项目实测的正面绕序
// （NDC 顺时针，Y 轴向下翻折所致；第 11 课踩过的坑）
const std::vector<Vertex> vertices = {
    {{-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},   // 0: 上左 红
    {{-0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}},   // 1: 下左 绿
    {{ 0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}},   // 2: 下右 蓝
    {{ 0.5f, -0.5f}, {1.0f, 1.0f, 1.0f}},   // 3: 上右 白
};

// 两个三角形共用顶点 0 和 2：4 顶点 + 6 下标，替代 6 顶点
const std::vector<uint16_t> indices = {
    0, 1, 2,   // 左下三角
    0, 2, 3,   // 右上三角
};

class HelloTriangleApplication {
public:
    void run() {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;

    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    std::vector<VkImageView> swapChainImageViews;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;

    // 第八课三件套
    VkRenderPass renderPass;          // 渲染通道：目标怎么用
    VkPipelineLayout pipelineLayout;  // 管线布局：着色器资源接口（本课为空壳）
    VkPipeline graphicsPipeline;      // 装配完成的管线本体

    std::vector<VkFramebuffer> swapChainFramebuffers;

    // 第十课：命令池；第十一课升级为每帧一条命令缓冲
    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;

    // 第十二课：顶点缓冲（对象）+ 它背后的显存。注意是分开的两个实体：
    // VkBuffer 只是"缓冲的描述符"，VkDeviceMemory 才是装字节的内存
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;

    // 第十四课：索引缓冲（对象 + 显存）。装的是 uint16 下标而非顶点
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;

    // 第十一课：同步对象（每帧一份）+ 帧轮转下标
    std::vector<VkSemaphore> imageAvailableSemaphores;   // 图像到手通知（每帧一个）
    // 渲染完成通知：按"交换链图像"索引而非帧索引——
    // 呈现引擎可能还拿着信号量，只有重新采集到同一张图才允许再 signal
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;                 // 该帧是否已提交（CPU 侧等待）
    std::vector<VkFence> imagesInFlight;                 // 每张交换链图像当前被哪帧占用
    uint32_t currentFrame = 0;
    bool framebufferResized = false;                     // 窗口尺寸变化标记

    void initWindow() {
        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);
        // 尺寸变化只立旗标，重建推迟到下一次 drawFrame（那时交换链可能正被用）
        glfwSetWindowUserPointer(window, this);   // 让静态回调能拿到 this
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
    }

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<HelloTriangleApplication*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
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
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createVertexBuffer();      // 第十二/十三课：顶点数据上显存（staging 模式）
        createIndexBuffer();       // 第十四课：顶点复用下标
        createCommandBuffers();    // 第十一课：每帧一条
        createSyncObjects();
    }

    // -----------------------------------------------------------------
    // 命令池：命令缓冲的内存分配器。flag 带 RESET_COMMAND_BUFFER_BIT，
    // 允许单独重置缓冲（为第 11 课每帧重录做准备）
    // -----------------------------------------------------------------
    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            throw std::runtime_error("创建命令池失败");
        }
    }

    // -----------------------------------------------------------------
    // 通用缓冲创建：对象 + 显存 + 绑定，三步合一。
    // size/usage/properties 由调用方定——staging 缓冲和真正的顶点缓冲
    // 只有这三个参数不同，抽出来避免写两遍
    // -----------------------------------------------------------------
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            throw std::runtime_error("创建缓冲失败");
        }

        // 查这个缓冲"需要"什么样的内存（类型位是驱动的内部编码）
        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("为缓冲分配显存失败");
        }

        // 对象和内存是分开的，必须手动绑定
        vkBindBufferMemory(device, buffer, bufferMemory, 0);
    }

    // -----------------------------------------------------------------
    // 第十三课核心：两级缓冲搬运。
    //
    // 第 12 课把顶点缓冲直接建在 CPU 可写内存里（HOST_VISIBLE）——能用，
    // 但 GPU 读它走的是 PCIe 慢速通道。正确姿势：
    //   ① staging 缓冲（CPU 可写）  ← memcpy 顶点数据
    //   ② 顶点缓冲（GPU 专用显存 DEVICE_LOCAL）← vkCmdCopyBuffer 由 GPU 自己搬
    // GPU 读 ② 是全速的；搬运指令也交给 GPU 队列异步执行
    // -----------------------------------------------------------------
    void createVertexBuffer() {
        VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

        // ① 中转站：CPU 可写，允许作为拷贝的"源"
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(bufferSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingBufferMemory);

        // memcpy 进 staging（映射 → 拷 → 解映射）
        void* data;
        vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, vertices.data(), (size_t)bufferSize);
        vkUnmapMemory(device, stagingBufferMemory);

        // ② 真正的顶点缓冲：GPU 专用显存，允许作为拷贝的"目的"
        createBuffer(bufferSize,
                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     vertexBuffer, vertexBufferMemory);

        // GPU 自己搬：staging → vertexBuffer，搬完立即销毁中转站
        copyBuffer(stagingBuffer, vertexBuffer, bufferSize);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingBufferMemory, nullptr);
    }

    // 索引缓冲：和顶点缓冲同款 staging 模式，只有用途不同
    // （INDEX_BUFFER | TRANSFER_DST），数据是 uint16 下标数组
    void createIndexBuffer() {
        VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(bufferSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingBufferMemory);

        void* data;
        vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
        memcpy(data, indices.data(), (size_t)bufferSize);
        vkUnmapMemory(device, stagingBufferMemory);

        createBuffer(bufferSize,
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     indexBuffer, indexBufferMemory);

        copyBuffer(stagingBuffer, indexBuffer, bufferSize);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingBufferMemory, nullptr);
    }

    // 录一条"拷贝缓冲"命令并立即提交执行（一次性命令缓冲的用法：
    // 分配 → 开始(ONE_TIME_SUBMIT) → 录 → 结束 → 提交 → 等队列排空 → 释放）
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;  // 只交一次
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, src, dst, 1, &copyRegion);

        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);   // 粗粒度等待：搬完再走（staging 马上就拆）

        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    // 按"需求位 + 属性要求"在设备的内存类型表里找第一个匹配项。
    // 显存有多种：DEVICE_LOCAL（GPU 专用，快，CPU 摸不着）/ HOST_VISIBLE
    // （CPU 可写，慢）。staging 模式两种都用——各取所需
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

    // 分配 MAX_FRAMES_IN_FLIGHT 条命令缓冲（每帧各用一条，互不干扰）
    void createCommandBuffers() {
        commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

        if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            throw std::runtime_error("分配命令缓冲失败");
        }
    }

    // -----------------------------------------------------------------
    // 同步对象：信号量 × 2 组/帧 + 栅栏 × 1/帧。
    // 信号量只同步 GPU（acquire→submit→present 的接力）；
    // 栅栏同步 CPU 与 GPU（防止 CPU 跑太快把在飞的帧覆盖掉）
    // -----------------------------------------------------------------
    void createSyncObjects() {
        imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.resize(swapChainImages.size());   // 按图像索引
        inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
        imagesInFlight.resize(swapChainImages.size(), VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 初始即"完成"，首帧 wait 不卡死

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建同步对象失败");
            }
        }
        for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建同步对象失败");
            }
        }
    }

    // 交换链重建后图像数量可能变化，renderFinished 信号量跟着重建
    void recreateRenderFinishedSemaphores() {
        for (auto sem : renderFinishedSemaphores) {
            vkDestroySemaphore(device, sem, nullptr);
        }
        renderFinishedSemaphores.resize(swapChainImages.size());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS) {
                throw std::runtime_error("重建 renderFinished 信号量失败");
            }
        }
        imagesInFlight.assign(swapChainImages.size(), VK_NULL_HANDLE);
    }

    // -----------------------------------------------------------------
    // 交换链重建：窗口尺寸变了，图/视图/帧缓冲都得按新尺寸重来。
    // 必须先 vkDeviceWaitIdle——旧交换链可能还有在飞的呈现操作
    // -----------------------------------------------------------------
    void recreateSwapChain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {       // 最小化时等待恢复
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device);

        cleanupSwapChain();

        createSwapChain();
        createImageViews();
        createFramebuffers();
        recreateRenderFinishedSemaphores();   // 图像数量可能变了
    }

    void cleanupSwapChain() {
        for (auto framebuffer : swapChainFramebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        for (auto imageView : swapChainImageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        auto destroyFunc = (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(instance, "vkDestroySwapchainKHR");
        if (destroyFunc != nullptr) {
            destroyFunc(device, swapChain, nullptr);
        }
    }

    // -----------------------------------------------------------------
    // 录制渲染命令。一帧的完整指令序列：
    // begin → beginRenderPass(清屏) → 视口/裁剪 → 绑定管线 → draw → end → end
    // -----------------------------------------------------------------
    void recordCommandBuffer(VkCommandBuffer buffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;                  // 单次提交，不并发
        beginInfo.pInheritanceInfo = nullptr;

        if (vkBeginCommandBuffer(buffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("开始录制命令缓冲失败");
        }

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

        // 第十二课：绑定顶点缓冲——告诉 GPU "接下来的 draw 从这个缓冲读顶点"。
        // 绑的是 VkBuffer（描述符）而非 VkDeviceMemory；offset 0 = 从头读
        VkBuffer vertexBuffers[] = { vertexBuffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(buffer, 0, 1, vertexBuffers, offsets);

        // 第十四课：再绑索引缓冲（uint16 下标），绘制改走"下标模式"：
        // 6 个下标画出 2 个三角形，顶点缓冲里只存了 4 个顶点
        vkCmdBindIndexBuffer(buffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(buffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

        vkCmdEndRenderPass(buffer);

        if (vkEndCommandBuffer(buffer) != VK_SUCCESS) {
            throw std::runtime_error("录制命令缓冲失败");
        }
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

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建帧缓冲失败");
            }
        }
        std::cout << swapChainFramebuffers.size() << " 个帧缓冲已创建" << std::endl;
    }

    // -----------------------------------------------------------------
    // 读二进制文件（SPIR-V 字节码）
    // -----------------------------------------------------------------
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

    // SPIR-V 字节码 → VkShaderModule（OpenGL 时代的着色器是在运行时编译的，
    // Vulkan 统一离线编译好，驱动只做加载和轻微转译）
    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        // 字节对齐要求：SPIR-V 是 4 字节码，强转一下
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("创建着色器模块失败");
        }
        return shaderModule;
    }

    // -----------------------------------------------------------------
    // 渲染通道：告诉 Vulkan"我们有一张颜色附件（交换链图），
    // 加载时清屏、存储时用于呈现"
    // -----------------------------------------------------------------
    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;           // 单采样
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;      // 开始时清屏
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;    // 结果保留
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   // 没用到模板
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;         // 不关心之前内容
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;     // 呈现前的最终布局

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;              // 着色器里 layout(location=0) 对应的附件
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;

        // 子通道依赖：交换链图刚从"呈现"转来，要等它可用再开始渲染
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;                 // 隐式的前置
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

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            throw std::runtime_error("创建渲染通道失败");
        }
    }

    // -----------------------------------------------------------------
    // 装配图形管线。顺序：着色器 → 顶点装配 → 视口 → 光栅化 →
    //                      多重采样 → 颜色混合 → 管线布局 → 渲染通道
    // -----------------------------------------------------------------
    void createGraphicsPipeline() {
        // ① 加载 SPIR-V（第 12 课专用着色器：in-attribute 版，见 shaders/12/）
        auto vertShaderCode = readFile("shaders/12/vert.spv");
        auto fragShaderCode = readFile("shaders/12/frag.spv");

        VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
        VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

        // ② 两个着色器阶段
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

        // ③ 顶点输入：第 12 课核心升级——把 Vertex 的内存布局告诉管线。
        // binding = 一份缓冲的读法；attribute = 顶点内部各字段的对号入座表，
        // 与 shader.vert 里的 layout(location=0/1) 严格对应
        auto bindingDescription = Vertex::getBindingDescription();
        auto attributeDescriptions = Vertex::getAttributeDescriptions();

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        // ④ 图元装配：每 3 个顶点一个三角形
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // ⑤ 视口与裁剪：声明为"动态状态"，录制命令时才给具体值（第 10 课）
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // ⑥ 光栅化：填充模式，背面剔除
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        // ⑦ 多重采样：关闭
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // ⑧ 颜色混合：直接覆盖（不做透明混合）
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        // ⑨ 动态状态列表
        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // ⑩ 管线布局：uniform 等资源的接口，本课不用，空布局
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 0;
        pipelineLayoutInfo.pushConstantRangeCount = 0;

        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("创建管线布局失败");
        }

        // ⑪ 总装配
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

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("创建图形管线失败");
        }

        // 模块只在装配时需要，管线建完即可销毁
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);

        std::cout << "渲染通道 + 图形管线已创建" << std::endl;
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

            if (vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建图像视图失败");
            }
        }
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

        auto func = (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(instance, "vkCreateSwapchainKHR");
        if (func == nullptr || func(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            throw std::runtime_error("创建交换链失败");
        }

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
            glfwGetFramebufferSize(window, &width, &height);
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

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        QueueFamilyIndices indices = findQueueFamilies(device);
        bool extensionsSupported = checkDeviceExtensionSupport(device);
        return indices.isComplete() && extensionsSupported;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }
        return requiredExtensions.empty();
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

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
            throw std::runtime_error("创建逻辑设备失败");
        }

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("找不到任何支持 Vulkan 的显卡");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(device, &props);
            std::cout << "候选设备: " << props.deviceName << std::endl;

            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                std::cout << ">> 选中: " << props.deviceName << std::endl;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("没有满足要求的显卡");
        }
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.graphicsFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
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

    void createSurface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("创建窗口表面失败");
        }
    }

    void createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("请求的校验层不可用");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
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
        // MoltenVK 是可移植性驱动，不显式声明不会被枚举（macOS 必需，其他平台无此扩展）
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

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("创建 Vulkan 实例失败");
        }
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

        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            throw std::runtime_error("创建 debug messenger 失败");
        }
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
    // （第 11 课的参考实现，本课原样沿用——练习仍留在 11 课文件里）
    //
    // 同步关系：栅栏管 CPU 不超前排；信号量管 GPU 内部接力
    //   acquire --imageAvailable--> submit --renderFinished--> present
    // =========================================================================
    void drawFrame() {
        // 1. 等当前帧的栅栏：CPU 最多超前 MAX_FRAMES_IN_FLIGHT 帧
        vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

        // 2. 采集交换链图像（设备扩展函数，动态加载）
        auto acquireNextImage = (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(instance, "vkAcquireNextImageKHR");
        if (acquireNextImage == nullptr) {
            throw std::runtime_error("vkAcquireNextImageKHR 不可用（swapchain 扩展没启用？）");
        }
        uint32_t imageIndex;
        VkResult result = acquireNextImage(device, swapChain, UINT64_MAX,
            imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {   // 交换链已过期，重建后下帧再来
            recreateSwapChain();
            return;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("采集交换链图像失败");
        }

        // 3. 这张图若还被更早的帧占用，先等那帧交还（imagesInFlight 追踪）
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = inFlightFences[currentFrame];

        // 4. 重建并录制本帧命令（录制前重置；栅栏也将随提交重新"挂起"）
        vkResetFences(device, 1, &inFlightFences[currentFrame]);
        vkResetCommandBuffer(commandBuffers[currentFrame], 0);
        recordCommandBuffer(commandBuffers[currentFrame], imageIndex);

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

        if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
            throw std::runtime_error("提交渲染命令失败");
        }

        // 5. 呈现（同为设备扩展函数，动态加载）
        auto queuePresent = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(instance, "vkQueuePresentKHR");
        if (queuePresent == nullptr) {
            throw std::runtime_error("vkQueuePresentKHR 不可用");
        }
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;      // 等渲染完成才上屏
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;

        result = queuePresent(presentQueue, &presentInfo);

        // 交换链过期/次优，或期间窗口变过尺寸 → 重建
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapChain();
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("呈现失败");
        }

        // 帧轮转
        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void mainLoop() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            drawFrame();      // ← 你实现的函数将在这里每帧被调用
        }

        // 退出前等 GPU 收尾，否则销毁资源时可能还有在飞命令
        vkDeviceWaitIdle(device);
    }

    void cleanup() {
        // mainLoop 已保证设备空闲，这里直接拆
        cleanupSwapChain();                              // 帧缓冲 + 视图 + 交换链

        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);

        for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
        }
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(device, commandPool, nullptr);   // 池连带释放命令缓冲

        // 第十二课：缓冲对象和它的内存在 cleanupSwapChain 之后、设备销毁之前拆
        vkDestroyBuffer(device, vertexBuffer, nullptr);
        vkFreeMemory(device, vertexBufferMemory, nullptr);
        vkDestroyBuffer(device, indexBuffer, nullptr);          // 第十四课
        vkFreeMemory(device, indexBufferMemory, nullptr);

        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        if (enableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
    }
};

int main() {
    HelloTriangleApplication app;
    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
