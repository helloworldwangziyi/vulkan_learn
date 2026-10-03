#version 450

// 硬编码的三角形顶点（NDC 坐标）和顶点色
// gl_VertexIndex: 当前顶点的序号（0/1/2），不用顶点缓冲也能画图

layout(location = 0) out vec3 fragColor;

// 顶点顺序注意：Vulkan NDC 的 Y 轴向下，这个顺序在帧缓冲空间里
// 是逆时针（正面）；写反了会被背面剔除整个裁掉（经典翻车点）
vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(-0.5, 0.5),
    vec2(0.5, 0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
