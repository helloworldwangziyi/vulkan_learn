#version 450

// advanced/04：三层数据流的片段着色器
//
// set 1（材质级）：combined image sampler——纹理视图 + 采样器打包成
// 一个描述符。切换材质 = 换绑一个 set 1 集合，帧级的 set 0 不动

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

// set 1 = 材质级：纹理 + 采样器。两种材质（texture.png / 棋盘格）各一套集合
layout(set = 1, binding = 0) uniform sampler2D texSampler;

// 物体级 push constants：声明与顶点着色器一致；本阶段只用 tint
layout(push_constant) uniform ObjectPushConstants {
    mat4 model;
    vec4 tint;
} pc;

void main() {
    // 纹理采样结果 × 物体染色（tint）
    outColor = texture(texSampler, fragTexCoord) * pc.tint;
}
