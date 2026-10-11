#version 450

// advanced/05 场景片元着色器：顶点色 × 纹理（与 lessons/18 同款结构）
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;

// set = 1：材质级描述符（第 04 章）。本场景 8 个 quad 共用这一张纹理
layout(set = 1, binding = 0) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0) * texture(texSampler, fragTexCoord);
}
