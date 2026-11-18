// World fragment pass: draw the sand grid — solids, liquids, gas, and a subtle backdrop.
// Reads simulation cells in read-only mode and shades each screen pixel from grid data.
#version 460
#extension GL_GOOGLE_include_directive : require
#define WORLD_READ_ONLY
#include "world.glsl"

layout(location=0) out vec4 outColor;

// Convert final display color from srgb-encoded mixing to linear for the framebuffer.
// Implementation: linear segment below 0.04045; power-law gamma 2.4 above (standard srgb decode)
vec3 srgbToLinear(vec3 x)
{
    return mix(x / 12.92, pow((x + 0.055) / 1.055, vec3(2.4)), greaterThan(x, vec3(0.04045))); // 12.92 / 0.055 / 1.055 / 2.4 / 0.04045: srgb knee and gamma
}

// Liquid channels at a grid corner — zero off-world.
vec3 field(ivec2 p)
{
    return inside(p) ? readCell(indexOf(p)).liquid : vec3(0);
}

// Stable per-tile noise for grain and variation without storing textures.
float grain(ivec2 p)
{
    return random01(uint(p.x) * 1877u + uint(p.y) * 31247u); // 1877u / 31247u: spatial hash salts for random01
}

// Dark panel behind the playfield — vignette, grid dots, and edge falloff.
vec3 backdrop(vec2 p)
{
    // Normalized position across the world — 0 at one corner, 1 at the far corner.
    vec2 uv = p / vec2(pc.size);

    // Repeating 16-pixel catalog grid for subtle dot pattern.
    vec2 grid = mod(p, 16.0); // 16.0: backdrop tile period in grid pixels

    // Bright dot near center of each 16×16 tile, soft falloff toward tile edges.
    float dots = 1.0 - smoothstep(0.35, 0.85, length(min(grid, 16.0 - grid))); // 0.35 / 0.85: dot core vs rim

    // Soft spotlight toward upper-left of the panel.
    float light = exp(-2.8 * dot(uv - vec2(0.40, 0.18), uv - vec2(0.40, 0.18))); // 2.8: falloff; 0.40,0.18: hotspot in uv space

    // Distance in pixels from the world rectangle edge.
    float edge = min(min(p.x, p.y), min(pc.size.x - p.x, pc.size.y - p.y));

    // Base fill, hotspot lift, dot pattern, and darkening near the world border.
    return mix(vec3(0.055, 0.068, 0.089), vec3(0.088, 0.114, 0.149), light) // dark / light backdrop rgb
           + dots * vec3(0.034, 0.043, 0.052) // dot tint
           + (1.0 - smoothstep(0.0, 3.0, edge)) * vec3(0.016, 0.025, 0.040); // 3.0: edge darken width in pixels
}

// Color solid grains and rocks from material id, neighbors, and procedural detail.
vec3 terrain(ivec2 p, vec2 g, Cell cell)
{
    uint id = material(cell);
    float n = grain(p);

    // 1 when the cell above or to the left is open — highlights top/left edges of piles.
    float top = solid(readCell(p - DOWN)) ? 0.0 : 1.0;
    float left = solid(readCell(p - RIGHT)) ? 0.0 : 1.0;

    // Dry or damp sand piles with a little grain sparkle.
    if (id == SAND) {
        bool soaked = wet(readCell(p + RIGHT)) || wet(readCell(p - RIGHT)) ||
                      wet(readCell(p - DOWN));
        vec3 c = mix(vec3(0.49, 0.33, 0.18), vec3(0.92, 0.73, 0.43), n * 0.75 + top * 0.25); // 0.75 / 0.25: noise vs edge highlight weights
        return c * (soaked ? 0.76 : 1.0); // 0.76: darken when neighbors are wet
    }

    // Layered stone with strata and heat glow when warm.
    if (id == STONE) {
        float strata = sin(g.y * 0.75 + sin(g.x * 0.22) * 1.4); // 0.75 / 0.22 / 1.4: horizontal layers wobble
        vec3 c = mix(vec3(0.135, 0.177, 0.231), vec3(0.26, 0.33, 0.40), n * 0.32 + strata * 0.12 + 0.25); // stone dark/light mix weights
        return c + vec3(0.075, 0.093, 0.111) * top + left * 0.019 + heat(cell) * vec3(0.3, 0.11, 0.02); // top/left rim; heat tints orange
    }

    // Ringy wood grain from repeating sine ripples.
    if (id == WOOD) {
        float ring = sin(g.y * 2.15 + sin(g.x * 0.07) * 1.4 + sin(g.x * 0.19) * 0.6); // ring frequencies along g
        return mix(vec3(0.29, 0.15, 0.099), vec3(0.65, 0.37, 0.22), ring * 0.16 + n * 0.22 + top * 0.30 + 0.25); // wood tone blend weights
    }

    // Default mineral tint when the cell is solid but not sand, stone, or wood.
    float vein = pow(max(0.0, cos(g.x * 0.59 + g.y * 0.95 + sin(g.y * 0.8))), 12.0); // 12.0: sharp vein ridges
    return mix(vec3(0.26, 0.52, 0.66), vec3(0.68, 0.88, 0.93),
               clamp(vein * 0.34 + n * 0.14 + top * 0.45 + 0.20, 0.0, 1.0)); // default mineral palette mix
}

// Smooth-ish noise built from the same grain hash at four corners.
// Implementation: Hermite smoothstep on fract; bilinear mix of grain() at four integer corners
float noiseField(vec2 p)
{
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // 3.0: smoothstep hermite coefficients
    return mix(mix(grain(base), grain(base + RIGHT), f.x),
               mix(grain(base + DOWN), grain(base + RIGHT + DOWN), f.x), f.y);
}

// Shade water, oil, and lava with motion, normals, and fake lighting.
vec3 lightLiquid(vec2 g, vec3 phase, vec2 gradient, vec2 motion, float coverage)
{
    // Share of water / oil / lava in the local mix — avoids divide by zero on empty phase.
    vec3 fractions = phase / max(dot(phase, vec3(1)), 0.001); // 0.001: epsilon for empty phase

    float t = pc.screen.z; // animation time from push constants

    // Advect shading coordinates opposite flow so highlights slide with liquid motion.
    vec2 flow = g - motion * 2.0; // 2.0: motion parallax scale on flow coords

    // Layered sines build ripples that scroll with time.
    float warp = sin(flow.x * 0.108 + sin(flow.y * 0.119 + t * 0.37) - t * 0.4); // 0.108 / 0.119 / 0.37 / 0.4: warp frequencies and time rates
    float a = sin(flow.x * 0.46 + flow.y * 0.24 + warp * 1.57 - t * 1.3);
    float b = sin(flow.x * 0.21 - flow.y * 0.37 + sin(flow.x * 0.12) * 1.12 + t * 0.9);
    float ripple = a * b;

    // Faster flow brightens the normal hack slightly.
    float motionLight = clamp(length(motion) * 1.2 + 0.14, 0.0, 1.0); // 1.2 / 0.14: speed-to-brightness

    // Fake surface normal from gradient, flow, and ripple bumps.
    vec3 normal = normalize(vec3(-gradient * 1.75 + motion * 0.30 +
                                 vec2(a, b) * (0.045 + motionLight * 0.06), 1.0)); // 1.75 / 0.30 / 0.045 / 0.06: normal composition weights

    const vec3 light = normalize(vec3(-0.52, -0.69, 0.79)); // key light direction
    float ndl = max(dot(normal, light), 0.0);
    float fresnel = 0.040 + 0.96 * pow(1.0 - normal.z, 5.0); // 0.040 / 0.96 / 5.0: fresnel bias, strength, exponent
    float spec = pow(max(dot(normal, normalize(light + vec3(0, 0, 1))), 0.0), 31.0); // 31.0: specular tightness
    float edge = min(length(gradient) * 1.24, 1.0); // 1.24: edge emphasis from phase gradient
    vec3 shaded = vec3(0);

    // Water: see-through blue with highlights and ripples.
    if (fractions.x > 0.0) {
        vec3 under = backdrop(g - normal.xy * 2.8); // 2.8: refractive offset into backdrop sample
        vec3 transmission = exp(-vec3(1.95, 0.82, 0.41) * (coverage * 0.58 + 0.15)); // rgb absorption; 0.58 / 0.15: coverage terms
        float scatter = pow(max(0.0, 1.0 - abs(a + b * 0.46)), 9.0); // 0.46 / 9.0: scatter shape from ripples
        vec3 water = vec3(0.076, 0.34, 0.51) * (0.65 + ndl * 0.40) + under * transmission;
        water += vec3(0.31, 0.62, 0.74) * (edge * (0.22 + ndl * 0.42) + fresnel * 0.62);
        water += vec3(0.65, 0.90, 0.96) * (spec * 0.67 + scatter * motionLight * 0.078);
        shaded += fractions.x * water;
    }

    // Oil: darker fluid with oily specular sheen.
    if (fractions.y > 0.0) {
        vec3 oil = mix(vec3(0.30, 0.19, 0.09), vec3(0.64, 0.42, 0.20), ndl);
        oil += vec3(0.32, 0.21, 0.16) * edge + vec3(0.35, 0.32, 0.43) * spec;
        oil += 0.042 * cos(vec3(0, 2, 4) + ripple * 3.2 + fresnel * 7.0 + warp * 1.3); // 0.042 / 3.2 / 7.0 / 1.3: iridescent shimmer scales
        shaded += fractions.y * oil;
    }

    // Lava: bright heat from animated noise and glowing edges.
    if (fractions.z > 0.0) {
        vec2 convection = vec2(warp, a) * 0.76; // 0.76: convection displacement strength
        float coarse = noiseField(flow * vec2(0.125, 0.102) + convection + vec2(0, -t * 0.56)); // coarse lava flow scale
        float detail = noiseField(flow * 0.28 + vec2(b * 0.79, -t * 0.83)); // 0.28 / 0.79 / 0.83: fine detail scales
        float heat = smoothstep(0.33, 0.68, coarse * 0.76 + detail * 0.24 + edge * 0.13); // 0.33 / 0.68: heat ramp; blend weights 0.76 / 0.24 / 0.13
        vec3 lava = mix(vec3(0.43, 0.097, 0.040), vec3(1.0, 0.54, 0.12), heat);
        lava += vec3(0.26, 0.17, 0.048) * (edge * 0.79 + spec + heat * heat * 0.37); // 0.79 / 0.37: rim and core glow weights
        shaded += fractions.z * lava;
    }
    return shaded;
}

// Shade one pixel of the sandbox view inside the drawable canvas.
void main()
{
    // Map fragment position to grid coordinates inside the letterboxed world.
    // Implementation: subtract canvas origin, divide by drawable zw, scale by world size
    vec2 uv = (gl_FragCoord.xy - pc.canvas.xy) / pc.canvas.zw;
    vec2 g = uv * vec2(pc.size);
    ivec2 p = ivec2(g);
    Cell cell = readCell(indexOf(p));
    uint id = material(cell);
    vec3 color = backdrop(g);

    // Solids use terrain materials; open cells can show interpolated liquid.
    if (solid(cell)) {
        color = terrain(p, g, cell);
    } else {
        // Reconstruct the interface from cell-centered volume fractions.
        // Implementation: bilinear mix of liquid at four cell corners in unit-square centered on cells
        ivec2 base = ivec2(floor(g - 0.5)); // 0.5: shift to cell-centered grid
        vec2 f = fract(g - 0.5);
        vec3 a = field(base), b = field(base + RIGHT);
        vec3 c = field(base + DOWN), d = field(base + RIGHT + DOWN);
        vec3 phase = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
        float coverage = dot(phase, vec3(1));

        // Only shade as liquid when total fill is visibly above noise.
        if (coverage > 0.045) { // 0.045: minimum coverage before drawing liquid
            // Central differences on interpolated corners estimate surface slope.
            vec2 gradient = vec2(dot(mix(b - a, d - c, f.y), vec3(1)),
                                 dot(mix(c - a, d - b, f.x), vec3(1)));
            float alpha = smoothstep(0.07, 0.57, coverage); // 0.07 / 0.57: liquid blend in vs full opacity
            color = mix(color, lightLiquid(g, phase, gradient, velocity[indexOf(p)], coverage), alpha);
        }

        // Fire, steam, and smoke draw on top of liquids and backdrop.
        if (gas(id)) {
            float life = float(age(cell));
            float n = grain(p);
            float halo = 0.64 + 0.36 * sin((g.x + g.y) * 2.4 + n * 5.6 - pc.screen.z * 7.0); // 0.64 / 0.36 / 2.4 / 5.6 / 7.0: halo flicker
            // Flickering flame tinted by age.
            if (id == FIRE) {
                vec3 flame = mix(vec3(0.89, 0.23, 0.05), vec3(1.0, 0.82, 0.36), halo * (1.0 - life * 0.014)); // 0.014: young fire burns brighter
                color = mix(color, flame, 0.75 + n * 0.25); // 0.75 / 0.25: base vs noise flame opacity
            } else {
                // Steam reads lighter; smoke reads as a soft gray puff.
                vec3 cloud = id == STEAM ? vec3(0.56, 0.70, 0.74) : vec3(0.29, 0.31, 0.39);
                color = mix(color, cloud, (0.30 + 0.30 * halo) * (1.0 - life / 255.0)); // 0.30 / 255.0: cloud opacity vs lifetime
            }
        }
    }

    // Neighbor lava warms the pixel even when this cell is not full lava.
    float glow = max(max(field(p + RIGHT).z, field(p - RIGHT).z),
                     max(field(p + DOWN).z, field(p - DOWN).z));
    color += vec3(0.15, 0.041, 0.009) * glow; // 0.15 / 0.041 / 0.009: lava emissive tint rgb

    // Brush ring overlay when the cursor is visible.
    if (pc.cursor.w > 0.0) {
        float distanceToRing = abs(length(g - pc.cursor.xy) - pc.cursor.z); // ring distance: |r - radius|
        float line = 1.0 - smoothstep(0.18, 0.55, distanceToRing); // 0.18 / 0.55: ring line inner/outer thickness in grid units
        color = mix(color, vec3(0.89, 0.98, 0.94), line * 0.85); // 0.85: cursor ring opacity
    }

    // Clamp and convert to linear rgb for correct blending on screen.
    outColor = vec4(srgbToLinear(clamp(color, 0.0, 1.0)), 1.0);
}