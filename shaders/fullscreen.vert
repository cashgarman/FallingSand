#version 460
void main()
{
    const vec2 triangle[3] = vec2[3](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
    gl_Position = vec4(triangle[gl_VertexIndex], 0, 1);
}

