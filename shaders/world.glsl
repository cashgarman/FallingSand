#ifndef WORLD_GLSL
#define WORLD_GLSL

const uint EMPTY = 0u, SAND = 1u, WATER = 2u, OIL = 3u, STONE = 4u, WOOD = 5u;
const uint FIRE = 6u, SMOKE = 7u, STEAM = 8u, LAVA = 9u, ICE = 10u;

struct Cell
{
    vec3 liquid;
    uint matter;
};
struct Equation
{
    vec4 weights;
    float rhs;
};

#ifdef WORLD_READ_ONLY
#define WORLD_ACCESS readonly
#else
#define WORLD_ACCESS
#endif
layout(std430, set = 0, binding = 0) WORLD_ACCESS buffer CellsA
{
    Cell cellsA[];
};
layout(std430, set = 0, binding = 1) WORLD_ACCESS buffer CellsB
{
    Cell cellsB[];
};
layout(std430, set = 0, binding = 2) WORLD_ACCESS buffer Velocity
{
    vec2 velocity[];
};
#ifndef WORLD_READ_ONLY
layout(std430, set = 0, binding = 3) buffer Prediction
{
    vec2 prediction[];
};
layout(std430, set = 0, binding = 4) buffer Equations
{
    Equation equation[];
};
layout(std430, set = 0, binding = 5) buffer PressureA
{
    float pressureA[];
};
layout(std430, set = 0, binding = 6) buffer PressureB
{
    float pressureB[];
};
layout(std430, set = 0, binding = 7) buffer Flux
{
    vec2 flux[];
};
layout(std430, set = 0, binding = 8) buffer LimitA
{
    float limitA[];
};
layout(std430, set = 0, binding = 9) buffer LimitB
{
    float limitB[];
};
#endif

layout(push_constant) uniform Frame
{
    ivec2 size;
    uint state;
    uint tick;
    uvec4 operation; // mode, phase, material, spare
    vec4 brush;      // stroke endpoints in grid coordinates
    vec4 canvas;     // drawable x, y, width, height in pixels
    vec4 cursor;     // world-space x, y, brush radius, visibility
    vec4 screen;     // drawable width, height, visual time, spare
}
pc;

const ivec2 RIGHT = ivec2(1, 0), DOWN = ivec2(0, 1);

bool inside(ivec2 p)
{
    return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, pc.size));
}
int indexOf(ivec2 p)
{
    return p.y * pc.size.x + p.x;
}
uint material(Cell c)
{
    return c.matter & 255u;
}
float volume(Cell c)
{
    return dot(c.liquid, vec3(1));
}
uint age(Cell c)
{
    return (c.matter >> 8u) & 255u;
}
float heat(Cell c)
{
    return float((c.matter >> 16u) & 1023u) / 1023.0;
}
uint packMatter(uint id, uint life, float temperature)
{
    return id | (min(life, 255u) << 8u) |
           (uint(round(clamp(temperature, 0.0, 1.0) * 1023.0)) << 16u);
}
bool solid(Cell c)
{
    uint id = material(c);
    return id == STONE || id == WOOD || id == ICE || id == SAND;
}
bool wet(Cell c)
{
    return volume(c) > 0.028;
}
bool gas(uint id)
{
    return id == FIRE || id == SMOKE || id == STEAM;
}

Cell readCell(int i)
{
    return pc.state == 0u ? cellsA[i] : cellsB[i];
}
Cell readCell(ivec2 p)
{
    return inside(p) ? readCell(indexOf(p)) : Cell(vec3(0), STONE);
}
#ifndef WORLD_READ_ONLY
void writeCell(int i, Cell c)
{
    if (pc.state == 0u)
        cellsA[i] = c;
    else
        cellsB[i] = c;
}
void writeOther(int i, Cell c)
{
    if (pc.state == 0u)
        cellsB[i] = c;
    else
        cellsA[i] = c;
}
#endif

float inverseDensity(Cell c)
{
    return volume(c) / max(dot(c.liquid, vec3(1.0, 0.78, 2.3)), 0.001);
}

float faceWeight(Cell a, Cell b)
{
    if (solid(a) || solid(b))
        return 0.0;
    if (wet(a) && wet(b))
        return 0.5 * (inverseDensity(a) + inverseDensity(b));
    if (wet(a))
        return inverseDensity(a);
    if (wet(b))
        return inverseDensity(b);
    return 0.0;
}

uint hash32(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

float random01(uint x)
{
    return float(hash32(x) >> 8u) * (1.0 / 16777216.0);
}
#endif
