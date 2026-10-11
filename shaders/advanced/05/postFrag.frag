#version 450

// ==========================================================================
// 后处理片元着色器：采样上一 pass 渲染完成的离屏画面，按 push constant
// 选择效果（键盘 1/2/3/4 切换）。每个像素只采样一次，效果全是逐像素数学。
// ==========================================================================
layout(location = 0) in vec2 fragUV;

// 离屏颜色附件，此时已是 SHADER_READ_ONLY_OPTIMAL 布局
layout(set = 0, binding = 0) uniform sampler2D sceneTexture;

layout(push_constant) uniform PostParams {
    int effect;   // 0 原样 / 1 灰度 / 2 反色 / 3 暗角
} params;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(sceneTexture, fragUV).rgb;

    if (params.effect == 1) {
        // 灰度：Rec.601 亮度加权（人眼对绿色最敏感，权重最高）
        float gray = dot(color, vec3(0.299, 0.587, 0.114));
        color = vec3(gray);
    } else if (params.effect == 2) {
        color = 1.0 - color;                       // 反色
    } else if (params.effect == 3) {
        // 暗角（vignette）：离画面中心越远越暗。
        // UV 中心 (0.5,0.5) 距离为 0，四角约 0.707
        float dist = distance(fragUV, vec2(0.5));
        color *= smoothstep(0.85, 0.35, dist);     // 中心 1.0 → 边缘平滑压暗
    }

    outColor = vec4(color, 1.0);
}
