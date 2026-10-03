#pragma once
// ===========================================================================
// 插件协议 —— 相当于 Vulkan 的 vulkan_core.h：
// 宿主和插件都包含它，约定"函数指针该长什么样"。
//
// 对照 Vulkan：
//   PFN_Compute          ≈ PFN_vkCreateDebugUtilsMessengerEXT（函数指针类型）
//   PLUGIN_API WINAPI    ≈ VKAPI_PTR（调用约定，双方必须一致）
// ===========================================================================

#include <windows.h>

#define PLUGIN_API WINAPI

// 每个插件都要实现的两个函数。注意用 typedef 而不是直接声明——
// 宿主拿到地址后要靠这些类型把 void* 变成能调用的函数指针。
typedef const char* (PLUGIN_API* PFN_GetPluginName)();
typedef int         (PLUGIN_API* PFN_Compute)(int x);
