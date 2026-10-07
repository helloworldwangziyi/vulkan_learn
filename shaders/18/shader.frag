#version 450

// 第18课：binding 1 = 组合图像采样器（纹理视图 + 采样器打包成一个描述符）
layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    // 纹理颜色 × 顶点插值颜色（本课顶点色是白色角，近似直接显示纹理）
    outColor = texture(texSampler, fragTexCoord) * vec4(fragColor, 1.0);
}
