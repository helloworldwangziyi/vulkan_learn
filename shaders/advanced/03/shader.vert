#version 450

// advanced/03：MVP 拆成两条数据通道——
//   view/proj 走 UBO（binding = 0，每帧更新一次，8 个物体共享）
//   model     走 push constants（每次 draw 前由 vkCmdPushConstants 推送）
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

// 瘦身版 UBO：只剩相机相关的两个矩阵（C++ 侧 UniformBufferObject 同布局）
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

// push constant 块：数据直接来自命令缓冲，没有缓冲对象、没有描述符。
// 布局与 C++ 侧 PushConstantData 一致（std430：mat4 偏移 0，vec3 偏移 64）。
// 顶点着色器只用 model，片元着色器只用 tint——但块在两个着色器里
// 必须声明得一模一样（同一块内存，各取所需）
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec3 tint;
} pc;

void main() {
    gl_Position = ubo.proj * ubo.view * pc.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
