#version 450

layout(binding = 0) uniform sampler2D Input;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 color = texture(Input, uv);

    for(int i = -10; i <= 10; ++i)
    for(int j = -10; j <= 10; ++j)
    {
        color += texture(Input, uv + vec2(i, j) * 0.001f);
    }
    outColor = vec4(color.x, color.y, (color.z + uv.x)*.5, 1.0f)/401.0;
}
