#version 450

// advanced/04：三层数据流的顶点着色器
//
// 三层数据各就各位：
//   set 0（帧级）  ：view + proj，每帧更新一次，所有物体共享
//   push constants ：model + tint，逐物体推送（第 03 章）
//   set 1（材质级）：在片段着色器里（纹理 + 采样器）

layout(location = 0) in vec2 inPosition;   // 单位四边形局部坐标
layout(location = 1) in vec2 inTexCoord;   // 纹理坐标

layout(location = 0) out vec2 fragTexCoord;

// set 0 = 帧级 UBO：相机 VP。C++ 侧 FrameUniformBuffer 布局必须一致
layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 view;
    mat4 proj;
} frame;

// 物体级 push constants：model 在本阶段用，tint 透传给片段阶段
// （两个阶段的块声明必须一致；80 字节 ≤ 128 字节下限保证）
layout(push_constant) uniform ObjectPushConstants {
    mat4 model;
    vec4 tint;
} pc;

void main() {
    // model 来自 push constants（逐物体），view/proj 来自 set 0（每帧）
    gl_Position = frame.proj * frame.view * pc.model * vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}
