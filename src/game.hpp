#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sand {

/// <summary>Gets the simulation grid width in cells.</summary>
inline constexpr int worldWidth = 384;
/// <summary>Gets the simulation grid height in cells.</summary>
inline constexpr int worldHeight = 224;
/// <summary>Gets the total number of cells in the fixed world grid.</summary>
inline constexpr int worldCells = worldWidth * worldHeight;
/// <summary>Gets the fixed simulation tick rate used during ordinary play.</summary>
inline constexpr int simulationHz = 90;

/// <summary>Identifies a paintable material or empty cell in the world grid.</summary>
enum class Material : std::uint32_t
{
    empty,
    sand,
    water,
    oil,
    stone,
    wood,
    fire,
    smoke,
    steam,
    lava,
    ice
};

/// <summary>UI metadata for a <see cref="Material"/> entry in the material palette.</summary>
struct MaterialInfo
{
    /// <summary>Palette material id used when painting the grid.</summary>
    Material id;
    /// <summary>Short uppercase label shown in the UI.</summary>
    std::string_view name;
    /// <summary>One-line behavior hint for the material picker.</summary>
    std::string_view behavior;
    /// <summary>sRGB color as 0xRRGGBB for swatches and previews.</summary>
    std::uint32_t color;
};

/// <summary>Gets display names, behavior labels, and sRGB hex colors for all paintable materials.</summary>
inline constexpr std::array materials{
    MaterialInfo{Material::sand, "SAND", "GRANULAR / SINKS", 0xe7be78},
    MaterialInfo{Material::water, "WATER", "LIQUID / COOLANT", 0x51c4ef},
    MaterialInfo{Material::oil, "OIL", "LIQUID / FLOATS", 0xe39f54},
    MaterialInfo{Material::stone, "STONE", "SOLID / BARRIER", 0x8094a6},
    MaterialInfo{Material::wood, "WOOD", "SOLID / FUEL", 0xc68e6d},
    MaterialInfo{Material::fire, "FIRE", "HEAT / IGNITION", 0xff7e58},
    MaterialInfo{Material::smoke, "SMOKE", "GAS / RISES", 0x998ca7},
    MaterialInfo{Material::steam, "STEAM", "GAS / CONDENSES", 0xb5d9e2},
    MaterialInfo{Material::lava, "LAVA", "LIQUID / HOT", 0xf06036},
    MaterialInfo{Material::ice, "ICE", "SOLID / MELTS", 0x93dcf2},
};

/// <summary>Single grid cell stored on the GPU: three liquid fractions plus packed matter and heat.</summary>
struct Cell
{
    /// <summary>Water, oil, and lava fill levels in [0, 1] for the solver and shader.</summary>
    std::array<float, 3> liquid{};
    /// <summary>Packed material id (low byte) and optional heat or flags in upper bits.</summary>
    std::uint32_t matter{};
};
static_assert(sizeof(Cell) == 16 && std::is_trivially_copyable_v<Cell>);
static_assert(offsetof(Cell, matter) == 12);

/// <summary>Reads the material kind encoded in the low byte of <paramref name="cell"/>.matter.</summary>
/// <param name="cell">Cell whose packed matter word is decoded.</param>
/// <returns>The <see cref="Material"/> stored in the cell.</returns>
[[nodiscard]] constexpr Material kind(const Cell& cell)
{
    return static_cast<Material>(cell.matter & 255u);
}

/// <summary>Builds a default cell for <paramref name="id"/> with optional fill quantity.</summary>
/// <param name="id">Material to encode.</param>
/// <param name="quantity">Liquid fill level in [0, 1] for water, oil, and lava; ignored for solids and gases.</param>
/// <returns>A cell with liquid channels or matter/heat fields set for the shader and solver.</returns>
[[nodiscard]] constexpr Cell makeCell(Material id, float quantity = 1.0F)
{
    Cell result{};
    switch (id)
    {
    case Material::water:
        // Store fill in the water liquid channel.
        result.liquid[0] = quantity;
        break;
    case Material::oil:
        // Store fill in the oil liquid channel.
        result.liquid[1] = quantity;
        break;
    case Material::lava:
        // Lava uses the lava channel plus a hot matter temperature field.
        result.liquid[2] = quantity;
        result.matter = 1023u << 16;
        break;
    default:
        // Solids and gases pack the material id and optional heat in matter.
        result.matter = static_cast<std::uint32_t>(id);
        result.matter |= (id == Material::fire ? 950u : id == Material::steam ? 480u : 0u) << 16;
        break;
    }
    return result;
}

/// <summary>Point in world or UI space using top-left pixel coordinates.</summary>
struct Point
{
    /// <summary>Horizontal coordinate in pixels, origin at the left edge.</summary>
    float x{};
    /// <summary>Vertical coordinate in pixels, origin at the top edge.</summary>
    float y{};
};

/// <summary>Axis-aligned rectangle with a minimum corner and size in the same units as <see cref="Point"/>.</summary>
struct Rect
{
    /// <summary>Left edge of the rectangle in the same units as <see cref="Point"/>.</summary>
    float x{};
    /// <summary>Top edge of the rectangle.</summary>
    float y{};
    /// <summary>Width of the rectangle; must be non-negative for meaningful <see cref="contains"/> tests.</summary>
    float w{};
    /// <summary>Height of the rectangle; must be non-negative for meaningful <see cref="contains"/> tests.</summary>
    float h{};

    /// <summary>Tests whether <paramref name="p"/> lies inside this rectangle.</summary>
    /// <param name="p">Point to test.</param>
    /// <returns><see langword="true"/> when the point is inside [x, x + w) × [y, y + h).</returns>
    [[nodiscard]] constexpr bool contains(Point p) const
    {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
};

/// <summary>Brush stroke passed to the GPU for painting or stirring the grid.</summary>
struct Stroke
{
    /// <summary>Brush start position in grid pixel coordinates.</summary>
    Point from;
    /// <summary>Brush end position in grid pixel coordinates.</summary>
    Point to;
    /// <summary>Brush radius in pixels for the GPU paint pass.</summary>
    float radius;
    /// <summary>Material deposited or stirred along the stroke.</summary>
    Material material;
    /// <summary>When <see langword="true"/>, mixes existing cells instead of only painting.</summary>
    bool stir;
};

/// <summary>Built-in experiment layout loaded into a fresh world grid.</summary>
enum class Scene : std::uint8_t
{
    waterworks,
    thermal,
    refinery,
    empty
};

/// <summary>Gets tab labels aligned with <see cref="Scene"/> enum order.</summary>
inline constexpr std::array<std::string_view, 4> sceneNames{"WATERWORKS", "THERMAL VENT",
                                                            "REFINERY", "EMPTY"};

/// <summary>Fills a new world buffer with geometry and materials for <paramref name="scene"/>.</summary>
/// <param name="scene">Preset to construct.</param>
/// <returns>A vector of <see cref="worldCells"/> default-initialized cells with scene content painted in.</returns>
[[nodiscard]] std::vector<Cell> makeScene(Scene scene);

/// <summary>Writes a validated world grid to a binary sandbox file at <paramref name="path"/>.</summary>
/// <param name="path">Destination file; written atomically via a sibling .tmp file.</param>
/// <param name="cells">Full world grid; must match <c>worldCells</c> and pass cell validation.</param>
/// <returns>Success, or an error message when validation or the filesystem operation fails.</returns>
[[nodiscard]] std::expected<void, std::string> saveWorld(const std::filesystem::path& path,
                                                         std::span<const Cell> cells);

/// <summary>Reads a sandbox save from <paramref name="path"/> into a world grid.</summary>
/// <param name="path">Save file created by <see cref="saveWorld"/>.</param>
/// <returns>The loaded cells, or an error message when the header, size, or payload is invalid.</returns>
[[nodiscard]] std::expected<std::vector<Cell>, std::string> loadWorld(
    const std::filesystem::path& path);

} // namespace sand