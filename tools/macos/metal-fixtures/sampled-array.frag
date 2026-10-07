// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(set=1,binding=3) uniform sampler2D images[2];
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
void main() { color = texture(images[1], uv); }
