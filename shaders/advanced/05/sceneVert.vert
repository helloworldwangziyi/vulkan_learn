#version 450

// ==========================================================================
// advanced/05 场景顶点着色器 —— 数据三层分层的着色器侧体现（第 03/04 章）：
//   set = 0, binding = 0  帧级：view/proj 矩阵，UBO 每帧更新一次
//   set = 1, binding = 0  材质级：纹理（片元阶段才用，这里不声明）
//   push_constant         物体级：model 矩阵 + tint 色调，逐 quad 推送
// ==========================================================================
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(set = 0, binding = 0) uniform FrameUniforms {
    mat4 view;
    mat4 proj;
} frame;

// push constant 块 = 64(mat4) + 16(vec4) = 80 字节，
// 低于规范保证的 128 字节下限，任何实现都接受
layout(push_constant) uniform QuadPush {
    mat4 model;
    vec4 tint;
} quad;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    gl_Position = frame.proj * frame.view * quad.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor * quad.tint.rgb;   // 顶点色 × 物体色调
    fragTexCoord = inTexCoord;
}
