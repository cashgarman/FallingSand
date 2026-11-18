#version 460
#extension GL_GOOGLE_include_directive : require
#define WORLD_READ_ONLY
#include "world.glsl"

layout(location=0) out vec4 outColor;

vec3 srgbToLinear(vec3 x)
{
    return mix(x / 12.92, pow((x + 0.055) / 1.055, vec3(2.4)), greaterThan(x, vec3(0.04045)));
}

vec3 field(ivec2 p)
{
    return inside(p) ? readCell(indexOf(p)).liquid : vec3(0);
}

float grain(ivec2 p)
{
    return random01(uint(p.x) * 1877u + uint(p.y) * 31247u);
}

vec3 backdrop(vec2 p)
{
    vec2 uv = p / vec2(pc.size);
    vec2 grid = mod(p, 16.0);
    float dots = 1.0 - smoothstep(0.35, 0.85, length(min(grid, 16.0 - grid)));
    float light = exp(-2.8 * dot(uv - vec2(0.40, 0.18), uv - vec2(0.40, 0.18)));
    float edge = min(min(p.x, p.y), min(pc.size.x - p.x, pc.size.y - p.y));
    return mix(vec3(0.055, 0.068, 0.089), vec3(0.088, 0.114, 0.149), light)
           + dots * vec3(0.034, 0.043, 0.052)
           + (1.0 - smoothstep(0.0, 3.0, edge)) * vec3(0.016, 0.025, 0.040);
}

vec3 terrain(ivec2 p, vec2 g, Cell cell)
{
    uint id = material(cell);
    float n = grain(p);
    float top = solid(readCell(p - DOWN)) ? 0.0 : 1.0;
    float left = solid(readCell(p - RIGHT)) ? 0.0 : 1.0;
    if (id == SAND) {
        bool soaked = wet(readCell(p + RIGHT)) || wet(readCell(p - RIGHT)) ||
                      wet(readCell(p - DOWN));
        vec3 c = mix(vec3(0.49, 0.33, 0.18), vec3(0.92, 0.73, 0.43), n * 0.75 + top * 0.25);
        return c * (soaked ? 0.76 : 1.0);
    }
    if (id == STONE) {
        float strata = sin(g.y * 0.75 + sin(g.x * 0.22) * 1.4);
        vec3 c = mix(vec3(0.135, 0.177, 0.231), vec3(0.26, 0.33, 0.40), n * 0.32 + strata * 0.12 + 0.25);
        return c + vec3(0.075, 0.093, 0.111) * top + left * 0.019 + heat(cell) * vec3(0.3, 0.11, 0.02);
    }
    if (id == WOOD) {
        float ring = sin(g.y * 2.15 + sin(g.x * 0.07) * 1.4 + sin(g.x * 0.19) * 0.6);
        return mix(vec3(0.29, 0.15, 0.099), vec3(0.65, 0.37, 0.22), ring * 0.16 + n * 0.22 + top * 0.30 + 0.25);
    }
    float vein = pow(max(0.0, cos(g.x * 0.59 + g.y * 0.95 + sin(g.y * 0.8))), 12.0);
    return mix(vec3(0.26, 0.52, 0.66), vec3(0.68, 0.88, 0.93),
               clamp(vein * 0.34 + n * 0.14 + top * 0.45 + 0.20, 0.0, 1.0));
}

float noiseField(vec2 p)
{
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(grain(base), grain(base + RIGHT), f.x),
               mix(grain(base + DOWN), grain(base + RIGHT + DOWN), f.x), f.y);
}

vec3 lightLiquid(vec2 g, vec3 phase, vec2 gradient, vec2 motion, float coverage)
{
    vec3 fractions = phase / max(dot(phase, vec3(1)), 0.001);
    float t = pc.screen.z;
    vec2 flow = g - motion * 2.0;
    float warp = sin(flow.x * 0.108 + sin(flow.y * 0.119 + t * 0.37) - t * 0.4);
    float a = sin(flow.x * 0.46 + flow.y * 0.24 + warp * 1.57 - t * 1.3);
    float b = sin(flow.x * 0.21 - flow.y * 0.37 + sin(flow.x * 0.12) * 1.12 + t * 0.9);
    float ripple = a * b;
    float motionLight = clamp(length(motion) * 1.2 + 0.14, 0.0, 1.0);
    vec3 normal = normalize(vec3(-gradient * 1.75 + motion * 0.30 +
                                 vec2(a, b) * (0.045 + motionLight * 0.06), 1.0));
    const vec3 light = normalize(vec3(-0.52, -0.69, 0.79));
    float ndl = max(dot(normal, light), 0.0);
    float fresnel = 0.040 + 0.96 * pow(1.0 - normal.z, 5.0);
    float spec = pow(max(dot(normal, normalize(light + vec3(0, 0, 1))), 0.0), 31.0);
    float edge = min(length(gradient) * 1.24, 1.0);
    vec3 shaded = vec3(0);

    if (fractions.x > 0.0) {
        vec3 under = backdrop(g - normal.xy * 2.8);
        vec3 transmission = exp(-vec3(1.95, 0.82, 0.41) * (coverage * 0.58 + 0.15));
        float scatter = pow(max(0.0, 1.0 - abs(a + b * 0.46)), 9.0);
        vec3 water = vec3(0.076, 0.34, 0.51) * (0.65 + ndl * 0.40) + under * transmission;
        water += vec3(0.31, 0.62, 0.74) * (edge * (0.22 + ndl * 0.42) + fresnel * 0.62);
        water += vec3(0.65, 0.90, 0.96) * (spec * 0.67 + scatter * motionLight * 0.078);
        shaded += fractions.x * water;
    }
    if (fractions.y > 0.0) {
        vec3 oil = mix(vec3(0.30, 0.19, 0.09), vec3(0.64, 0.42, 0.20), ndl);
        oil += vec3(0.32, 0.21, 0.16) * edge + vec3(0.35, 0.32, 0.43) * spec;
        oil += 0.042 * cos(vec3(0, 2, 4) + ripple * 3.2 + fresnel * 7.0 + warp * 1.3);
        shaded += fractions.y * oil;
    }
    if (fractions.z > 0.0) {
        vec2 convection = vec2(warp, a) * 0.76;
        float coarse = noiseField(flow * vec2(0.125, 0.102) + convection + vec2(0, -t * 0.56));
        float detail = noiseField(flow * 0.28 + vec2(b * 0.79, -t * 0.83));
        float heat = smoothstep(0.33, 0.68, coarse * 0.76 + detail * 0.24 + edge * 0.13);
        vec3 lava = mix(vec3(0.43, 0.097, 0.040), vec3(1.0, 0.54, 0.12), heat);
        lava += vec3(0.26, 0.17, 0.048) * (edge * 0.79 + spec + heat * heat * 0.37);
        shaded += fractions.z * lava;
    }
    return shaded;
}

void main()
{
    vec2 uv = (gl_FragCoord.xy - pc.canvas.xy) / pc.canvas.zw;
    vec2 g = uv * vec2(pc.size);
    ivec2 p = ivec2(g);
    Cell cell = readCell(indexOf(p));
    uint id = material(cell);
    vec3 color = backdrop(g);
    if (solid(cell)) {
        color = terrain(p, g, cell);
    } else {
        // Reconstruct the interface from cell-centered volume fractions.
        ivec2 base = ivec2(floor(g - 0.5));
        vec2 f = fract(g - 0.5);
        vec3 a = field(base), b = field(base + RIGHT);
        vec3 c = field(base + DOWN), d = field(base + RIGHT + DOWN);
        vec3 phase = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
        float coverage = dot(phase, vec3(1));
        if (coverage > 0.045) {
            vec2 gradient = vec2(dot(mix(b - a, d - c, f.y), vec3(1)),
                                 dot(mix(c - a, d - b, f.x), vec3(1)));
            float alpha = smoothstep(0.07, 0.57, coverage);
            color = mix(color, lightLiquid(g, phase, gradient, velocity[indexOf(p)], coverage), alpha);
        }
        if (gas(id)) {
            float life = float(age(cell));
            float n = grain(p);
            float halo = 0.64 + 0.36 * sin((g.x + g.y) * 2.4 + n * 5.6 - pc.screen.z * 7.0);
            if (id == FIRE) {
                vec3 flame = mix(vec3(0.89, 0.23, 0.05), vec3(1.0, 0.82, 0.36), halo * (1.0 - life * 0.014));
                color = mix(color, flame, 0.75 + n * 0.25);
            } else {
                vec3 cloud = id == STEAM ? vec3(0.56, 0.70, 0.74) : vec3(0.29, 0.31, 0.39);
                color = mix(color, cloud, (0.30 + 0.30 * halo) * (1.0 - life / 255.0));
            }
        }
    }
    float glow = max(max(field(p + RIGHT).z, field(p - RIGHT).z),
                     max(field(p + DOWN).z, field(p - DOWN).z));
    color += vec3(0.15, 0.041, 0.009) * glow;

    if (pc.cursor.w > 0.0) {
        float distanceToRing = abs(length(g - pc.cursor.xy) - pc.cursor.z);
        float line = 1.0 - smoothstep(0.18, 0.55, distanceToRing);
        color = mix(color, vec3(0.89, 0.98, 0.94), line * 0.85);
    }
    outColor = vec4(srgbToLinear(clamp(color, 0.0, 1.0)), 1.0);
}

