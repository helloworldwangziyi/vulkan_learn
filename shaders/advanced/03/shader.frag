#version 450

// advanced/03：tint 从 push constants 来——同一份顶点色，
// 每个物体乘上自己的色调（8 个四边形 8 种颜色，零额外缓冲）
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

// 与 shader.vert 中一模一样的块：push constant 内存在各着色器阶段间共享，
// 管线布局里那一个 range 的 stageFlags 同时点了 VERTEX 和 FRAGMENT。
// model 在片元阶段不用，但块布局必须保持一致（偏移要对得上）
layout(push_constant) uniform PushConstants {
    mat4 model;
    vec3 tint;
} pc;

void main() {
    outColor = vec4(fragColor * pc.tint, 1.0);
}
