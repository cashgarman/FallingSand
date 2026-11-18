// Fullscreen pass: one oversized triangle covers the entire drawable area.
// Cheaper than a quad — no shared edge and no second triangle setup.
#version 460

// Emit clip-space corners for the giant triangle covering the framebuffer.
void main()
{
    // Vertices extend past the [-1,1] box so the whole screen stays inside the triangle.
    const vec2 triangle[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(triangle[gl_VertexIndex], 0, 1);
}