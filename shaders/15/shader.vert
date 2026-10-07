#version 450

// 第15课：顶点数据来自缓冲，坐标再乘上 uniform 缓冲里的三个变换矩阵
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

// Uniform Buffer Object：C++ 侧每帧更新的"全局变量"，binding = 0
layout(binding = 0) uniform UniformBufferObject {
    mat4 model;   // 模型变换（本课：绕 Z 轴旋转）
    mat4 view;    // 视图变换（相机）
    mat4 proj;    // 投影变换（透视）
} ubo;

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
