// ===========================================================================
// 迷你 Vulkan 加载器 —— 宿主程序
//
// 流程：扫描 plugins/ 目录的 DLL → LoadLibrary 装进内存 →
//       GetProcAddress 按名字查函数 → 存入"插件表" → 依次调用（插件链）
//
// 对照 Vulkan：
//   扫 plugins/*.dll        ≈ 加载器读注册表找 ICD/层 DLL
//   LoadLibrary             ≈ 加载器把 VkLayer_*.dll 加载进进程
//   GetProcAddress          ≈ vkGetInstanceProcAddr（按名字查函数地址）
//   查不到 → 跳过该插件     ≈ 返回 VK_ERROR_EXTENSION_NOT_PRESENT
//   依次调用每个插件        ≈ Vulkan 的"层链"（调用一层层穿过校验层）
//   FreeLibrary             ≈ 实例销毁时卸载层
// ===========================================================================
#include "plugin_api.h"

#include <iostream>
#include <string>
#include <vector>

struct Plugin {
    HMODULE dll;                 // DLL 句柄（相当于 VkInstance 的引用）
    std::string name;
    PFN_Compute compute;         // 查到的函数指针
};

int main() {
    std::vector<Plugin> plugins;

    // ---- 1. 扫描插件目录（相当于加载器盘点 ICD）----
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("plugins\\*.dll", &fd);
    if (find == INVALID_HANDLE_VALUE) {
        std::cerr << "plugins/ 目录下没有 DLL" << std::endl;
        return 1;
    }
    do {
        std::string path = std::string("plugins\\") + fd.cFileName;

        // ---- 2. 装进内存（相当于加载层 DLL）----
        HMODULE dll = LoadLibraryA(path.c_str());
        if (!dll) {
            std::cerr << "[加载器] 无法加载 " << path << std::endl;
            continue;
        }

        // ---- 3. 按名字查函数（相当于 vkGetInstanceProcAddr）----
        auto nameFn  = (PFN_GetPluginName)GetProcAddress(dll, "GetPluginName");
        auto compute = (PFN_Compute)     GetProcAddress(dll, "Compute");

        // ---- 4. 缺函数就当"不支持此扩展"，跳过 ----
        if (!nameFn || !compute) {
            std::cerr << "[加载器] " << path << " 缺少必需函数，跳过"
                      << "（相当于 VK_ERROR_EXTENSION_NOT_PRESENT）" << std::endl;
            FreeLibrary(dll);
            continue;
        }

        plugins.push_back({dll, nameFn(), compute});
        std::cout << "[加载器] 已注册插件: " << nameFn() << std::endl;
    } while (FindNextFileA(find, &fd));
    FindClose(find);

    if (plugins.empty()) {
        std::cerr << "没有可用插件" << std::endl;
        return 1;
    }

    // ---- 5. 插件链：一个插件的输出是下一个的输入 ----
    // 相当于层链：应用 → 校验层 → 驱动，每层都能看到/修改请求
    int value = 5;
    std::cout << "\n输入: " << value << std::endl;
    for (auto& p : plugins) {
        value = p.compute(value);
        std::cout << "经过 " << p.name << " -> " << value << std::endl;
    }

    // ---- 6. 卸载（相当于 vkDestroyInstance 清理）----
    for (auto& p : plugins) FreeLibrary(p.dll);

    return 0;
}
