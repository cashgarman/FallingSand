// UI fragment pass: pass through the vertex tint unchanged.
// Flat HUD quads skip lighting and texturing — color comes straight from the vertex stage.
#version 460
layout(location = 0) in vec4 color;
layout(location = 0) out vec4 outColor;

// Write the same rgba the vertex shader interpolated across each UI rectangle.
void main()
{
    outColor = color;
}