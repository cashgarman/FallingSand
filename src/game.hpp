#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Declarations shared by the simulation, the window, and the GPU renderer.
//
// Ordinary functions and tables are declared here and defined once in game.cpp. That
// keeps a single external definition for each of them. inline constexpr scalars may be
// included from several translation units; inline gives those copies one identity.
//
// A constexpr function can run at compile time or at runtime. enum class keeps
// material and scene ids in their own scope. string_view and span borrow text and
// cells that someone else owns, and they stay valid only while that storage does.
// array holds the fixed palette without a heap allocation, and class template
// argument deduction fills in the element type from the initializers.
// Cell is trivially copyable so its bytes can be copied into a GPU buffer.
// expected carries success or a reason the UI can show after a save, load, or scene layout.
// nodiscard marks results the caller needs to look at.

namespace sand {

// Simulation grid width in cells.
inline constexpr int worldWidth = 384;

// Simulation grid height in cells.
inline constexpr int worldHeight = 224;

// Total number of cells in the fixed world grid.
inline constexpr int worldCells = worldWidth * worldHeight;

// Fixed simulation tick rate used during ordinary play.
inline constexpr int simulationHz = 90;

/// <summary>Identifies a paintable material or empty cell in the world grid.</summary>
enum class Material : std::uint32_t
{
    empty, // No matter in the cell.
    sand,  // Granular solid that sinks and piles.
    water, // Coolant liquid.
    oil,   // Liquid that floats on water.
    stone, // Solid barrier.
    wood,  // Solid fuel.
    fire,  // Heat source and ignition.
    smoke, // Rising gas.
    steam, // Condensing gas.
    lava,  // Hot liquid.
    ice    // Melting solid.
};

/// <summary>UI metadata for a Material entry in the material palette.</summary>
struct MaterialInfo
{
    Material id;               // Palette material id used when painting the grid.
    std::string_view name;     // Short uppercase label shown in the UI.
    std::string_view behavior; // One-line behavior hint for the material picker.
    std::uint32_t color;       // sRGB color as 0xRRGGBB for swatches and previews.
};

// Number of paintable materials in the palette (excludes empty).
constexpr std::size_t materialCount = 10;

// Display names, behavior labels, and sRGB hex colors for all paintable materials.
extern const std::array<MaterialInfo, materialCount> materials;

/// <summary>Single grid cell stored on the GPU: three liquid fractions plus packed matter and
/// heat.</summary>
struct Cell
{
    std::array<float, 3> liquid{}; // Water, oil, and lava fill levels in [0, 1].
    std::uint32_t matter{};        // Packed material id (low byte) and heat or flags in upper bits.
};

// Alignment, size, and trivial copyability are separate facts. These checks lock
// the ones the GPU path needs: 16 bytes, safe to memcpy, matter at byte 12.
static_assert(sizeof(Cell) == 16 && std::is_trivially_copyable_v<Cell>);
static_assert(offsetof(Cell, matter) == 12);

/// <summary>Reads the material kind encoded in the low byte of the cell matter word.</summary>
/// <param name="cell">Cell whose packed matter word is decoded.</param>
/// <returns>The Material stored in the cell.</returns>
[[nodiscard]] Material kind(const Cell& cell);

/// <summary>Resolves the dominant material in a cell for UI labels.</summary>
/// <param name="cell">Cell to classify from liquid channels and matter.</param>
/// <returns>Dominant paintable material for tooltip and palette lookup.</returns>
[[nodiscard]] Material displayMaterial(const Cell& cell);

/// <summary>Maps a Material to its index in the materials table.</summary>
/// <param name="id">Material to look up.</param>
/// <returns>Palette index, or empty when id is empty or not in the palette.</returns>
[[nodiscard]] std::optional<std::size_t> paletteIndex(Material id);

/// <summary>Builds a default cell for a material with optional fill quantity.</summary>
/// <param name="id">Material to encode.</param>
/// <param name="quantity">Liquid fill in [0, 1] for water, oil, and lava; ignored for solids and
/// gases.</param> <returns>A cell with liquid channels or matter and heat fields set for the shader
/// and solver.</returns>
[[nodiscard]] Cell makeCell(Material id, float quantity = 1.0F);

/// <summary>Point in world or UI space using top-left pixel coordinates.</summary>
struct Point
{
    float x{}; // Horizontal coordinate in pixels, origin at the left edge.
    float y{}; // Vertical coordinate in pixels, origin at the top edge.
};

/// <summary>Axis-aligned rectangle with a minimum corner and size in the same units as
/// Point.</summary>
struct Rect
{
    float x{}; // Left edge of the rectangle.
    float y{}; // Top edge of the rectangle.
    float w{}; // Width; non-negative for meaningful contains tests.
    float h{}; // Height; non-negative for meaningful contains tests.

    /// <summary>Tests whether a point lies inside this rectangle.</summary>
    /// <param name="p">Point to test.</param>
    /// <returns>True when the point is inside [x, x + w) × [y, y + h).</returns>
    [[nodiscard]] bool contains(Point p) const;
};

/// <summary>Brush stroke passed to the GPU for painting or stirring the grid.</summary>
struct Stroke
{
    Point from;        // Brush start position in grid pixel coordinates.
    Point to;          // Brush end position in grid pixel coordinates.
    float radius;      // Brush radius in pixels for the GPU paint pass.
    Material material; // Material deposited or stirred along the stroke.
    bool stir;         // When true, mixes existing cells instead of only painting.

    [[nodiscard]] constexpr bool poursLiquid() const
    {
        return !stir && (material == Material::water || material == Material::oil ||
                         material == Material::lava);
    }
};

/// <summary>Built-in experiment layout loaded into a fresh world grid.</summary>
enum class Scene : std::uint8_t
{
    waterworks, // Default water and machinery preset.
    thermal,    // Thermal vent preset.
    refinery,   // Refinery preset.
    empty       // Blank canvas.
};

// Number of built-in scene presets.
constexpr std::size_t sceneCount = 4;

// Tab labels aligned with Scene enum order.
extern const std::array<std::string_view, sceneCount> sceneNames;

/// <summary>Fills a new world buffer from the JSON layout for a scene preset.</summary>
/// <param name="scene">Preset whose file is read from the layouts directory beside the
/// executable.</param> <returns>A vector of worldCells cells, or an error when that layout is
/// missing or invalid.</returns>
[[nodiscard]] std::expected<std::vector<Cell>, std::string> makeScene(Scene scene);

/// <summary>Writes a validated world grid to a binary sandbox file.</summary>
/// <param name="path">Destination file; written atomically via a sibling .tmp file.</param>
/// <param name="cells">Full world grid; must match worldCells and pass cell validation.</param>
/// <returns>Success, or an error message when validation or the filesystem operation
/// fails.</returns>
[[nodiscard]] std::expected<void, std::string> saveWorld(const std::filesystem::path& path,
                                                         std::span<const Cell> cells);

/// <summary>Reads a sandbox save into a world grid.</summary>
/// <param name="path">Save file created by saveWorld.</param>
/// <returns>The loaded cells, or an error message when the header, size, or payload is
/// invalid.</returns>
[[nodiscard]] std::expected<std::vector<Cell>, std::string> loadWorld(
    const std::filesystem::path& path);

} // namespace sand
