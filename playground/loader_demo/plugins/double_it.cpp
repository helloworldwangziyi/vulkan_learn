// 插件示例 1：把输入翻倍
// ===========================================================================
// 对照 Vulkan：
//   __declspec(dllexport)  ≈ Vulkan 层/ICD 把函数"导出"到 DLL 导出表，
//                            vkGetInstanceProcAddr 才能按名字搜到它
//   extern "C"             ≈ 禁止 C++ 名字修饰，保证 DLL 导出表里的名字
//                            就是 "Compute"，而不是 ?Compute@@YAHH@Z
// ===========================================================================
#include "../plugin_api.h"

extern "C" __declspec(dllexport) const char* PLUGIN_API GetPluginName() {
    return "double_it";
}

extern "C" __declspec(dllexport) int PLUGIN_API Compute(int x) {
    return x * 2;
}
