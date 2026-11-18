// UI vertex pass: place colored rectangles for brushes, palettes, and HUD panels.
// Each instance is one axis-aligned box; six vertices draw it as two triangles.
#version 460

// Screen-space bounds (xy origin, zw size) and tint for one drawable.
struct Item
{
    vec4 bounds;
    vec4 color;
};
layout(std430, set = 1, binding = 0) readonly buffer Interface
{
    Item items[];
};
layout(push_constant) uniform Frame
{
    layout(offset = 80) vec4 screen;
}
pc;

// Per-vertex color forwarded to the fragment shader.
layout(location = 0) out vec4 color;

// Expand one UI rectangle instance into clip-space position and vertex color.
void main()
{
    // Two triangles worth of unit-square corners for this instance.
    const vec2 corners[6] =
        vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));

    // Which panel or widget this draw call belongs to.
    Item item = items[gl_InstanceIndex];

    // Pixel position inside the box from normalized corner coordinates.
    vec2 p = item.bounds.xy + item.bounds.zw * corners[gl_VertexIndex];

    // Map screen pixels to normalized device coordinates.
    gl_Position = vec4(p / pc.screen.xy * 2.0 - 1.0, 0.0, 1.0);
    color = item.color;
}