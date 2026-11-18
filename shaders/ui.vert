#version 460
struct Item { vec4 bounds; vec4 color; };
layout(std430, set=1, binding=0) readonly buffer Interface { Item items[]; };
layout(push_constant) uniform Frame {
    layout(offset=80) vec4 screen;
} pc;
layout(location=0) out vec4 color;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                                    vec2(0, 1), vec2(1, 0), vec2(1, 1));
    Item item = items[gl_InstanceIndex];
    vec2 p = item.bounds.xy + item.bounds.zw * corners[gl_VertexIndex];
    gl_Position = vec4(p / pc.screen.xy * 2.0 - 1.0, 0.0, 1.0);
    color = item.color;
}

