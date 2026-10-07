#version 450

// 第12课：顶点数据不再硬编码，改从顶点缓冲按 location 接收
layout(location = 0) in vec2 inPosition;   // 第 0 路：位置（来自 VkBuffer）
layout(location = 1) in vec3 inColor;      // 第 1 路：颜色（来自 VkBuffer）

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
