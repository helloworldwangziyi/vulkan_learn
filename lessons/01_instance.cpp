// ===========================================================================
// 第一课：Vulkan 实例 + 扩展检查练习
//
// 内容：
//   * createInstance() —— 创建 Vulkan 实例（应用与 Vulkan 库的连接）
//   * 练习：枚举本机实例扩展，逐一核对 GLFW 要求的扩展是否可用，
//     缺失时抛出异常并指名道姓（遍历"需求清单"查"全集"的模式）
//
// 知识点：
//   * VkApplicationInfo 是可选元数据；VkInstanceCreateInfo 才是主参数
//   * 扩展 = Vulkan 的可选功能模块；GLFW 通过 glfwGetRequiredInstanceExtensions
//     亲口告诉你窗口系统需要哪些
//   * 两段式枚举：先问数量，再取数据（Vulkan C API 的通用模式）
// ===========================================================================
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <vector>


const uint32_t WIDTH = 1600;
const uint32_t HEIGHT = 1600;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else 
const bool enableValidationLayers = true;
#endif


VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pMessenger)
{
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;  
    }
}



class HelloTriangleApplication{
public:
    void run(){
        initWindow();
        initVulkan();
        mainLoop();
        cleanUp();
    }

private:
    GLFWwindow* window;
    VkInstance instance;

    void initWindow(){
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

        window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);
    }

    void initVulkan(){
        createInstance();
    }

    void mainLoop(){
        while(!glfwWindowShouldClose(window)){
            glfwPollEvents();
        }
    }

    void cleanUp(){
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    void createInstance(){
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.pEngineName = "NO Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1,0,0);
        appInfo.apiVersion = VK_API_VERSION_1_0;


        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        // 获取本机支持的扩展列表
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());

        // 遍历 GLFW 的需求清单，逐个到本机可用列表里查（方向不能反）
        for (uint32_t i = 0; i < glfwExtensionCount; i++) {
            bool found = false;
            for (const auto& ext : extensions) {
                if (strcmp(ext.extensionName, glfwExtensions[i]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error(
                std::string("缺少必需的实例扩展: ") + glfwExtensions[i]);
            }
            std::cout << "扩展可用: " << glfwExtensions[i] << std::endl;
        }


        createInfo.enabledExtensionCount = glfwExtensionCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;
#ifdef __APPLE__
        // MoltenVK 是可移植性驱动，不显式声明不会被枚举（macOS 必需，其他平台无此扩展）
        {
            std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            createInfo.ppEnabledExtensionNames = extensions.data();
        }
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        createInfo.enabledLayerCount = 0;

        if(vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS){
            throw std::runtime_error("failed to create instance!");
        }
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