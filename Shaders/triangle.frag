#version 450

layout(binding = 0) uniform sampler2D Input;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 color = texture(Input, uv);
    outColor = vec4(uv.x, uv.y, 0.0f, 1.0f);
}
