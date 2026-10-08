#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
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


// 窗口大小
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation" // 官方提供的校验层
};


#ifdef NOBUG
const bool enableValidationLayers = false;
#else 
const bool enableValidationLayers = true;
#endif

class HelloTriangleApplication {
public:
    void run(){
        initWindow(); 
        initVulkan();
        mainLoop();
        cleanUp();
    }

private:
    GLFWwindow* window; // GLFW 窗口句柄

    VkInstance instance; // Vulkan 实例

    bool initWindow(){
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // 不创建 OpenGL 上下文)
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); // 禁止窗口缩放

        window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);

        if(window == nullptr)
        {
            std::cout << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            return false;
        }

        return true;
    }

    bool initVulkan(){
        createInstance(); // 创建实例
        setupDebugMessenger(); // 启用验证层
        createSurface();// 创建窗口
        pickPhysicalDevice(); // 物理设备
        createLogicalDevice(); // 逻辑设备
        createSwapChain(); // 交换链
        createImageViews(); // 交换链中的图像视图
        createRenderPass(); // renderpass
        createGraphicsPipline();// 图形管线
        createFrameBuffers();        // 帧缓冲 
        createCommandPool();// 命令池
        createCommandBuffers();// 命令缓冲
        return true;
    }

    void mainLoop(){
        while(!glfwWindowShouldClose(window)){
            glfwPollEvents();
        }

    }

    void cleanUp(){

    }

    void createInstance(){
        if(enableValidationLayers && !checkValidationLayerSupport()){
            throw std::runtime_error("validation layers requested, but not available!");
        
        }
    }

    void setupDebugMessenger(){

    }

    void createSurface(){

    }

    void pickPhysicalDevice(){

    }

    void createLogicalDevice(){

    }

    void createSwapChain(){

    }

    void createImageViews(){

    }

    void createRenderPass(){

    }

    void createGraphicsPipline(){

    }

    void createFrameBuffers(){

    }

    void createCommandPool(){

    }

    void createCommandBuffers(){

    }

    bool checkValidationLayerSupport(){
        uint32_t LayerCount;
        vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(LayerCount);
        vkEnumerateInstanceLayerProperties(&LayerCount, availableLayers.data());

        for(const char* LayerName : validationLayers)
        {
            bool LayerFound = false;

            for(const auto& LayerProperties : availableLayers)
            {
                std::cout << "LayerProperties name : " << LayerProperties.layerName << std::endl;
                if(strcmp(LayerName, LayerProperties.layerName) == 0)
                {
                    LayerFound = true;
                    break;
                }
            }

            if(!LayerFound)
            {
                return false;
            }
        }
        return true;
    }

};


int main(){
    
    HelloTriangleApplication app;

    try{
        app.run();
    }catch(const std::exception& e){
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
