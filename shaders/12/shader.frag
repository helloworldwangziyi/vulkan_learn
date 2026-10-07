#version 450

// 顶点着色器传下来的插值颜色，直接输出
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
