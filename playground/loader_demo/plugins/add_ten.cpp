// 插件示例 2：给输入加 10
#include "../plugin_api.h"

extern "C" __declspec(dllexport) const char* PLUGIN_API GetPluginName() {
    return "add_ten";
}

extern "C" __declspec(dllexport) int PLUGIN_API Compute(int x) {
    return x + 10;
}
