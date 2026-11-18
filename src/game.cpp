#include "game.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <mdspan>
#include <numbers>
#include <ranges>

#include <windows.h>

namespace sand {

// Helpers in this unnamed namespace have internal linkage. They belong to this
// translation unit only, so they cannot collide with the same name in another file.
namespace {

// mdspan maps a row and a column onto one flat buffer. The view owns nothing;
// the vector that supplies cells.data() keeps the cells alive.
using Grid = std::mdspan<Cell, std::extents<std::size_t, worldHeight, worldWidth>>;

// Fill an axis-aligned rectangle with a single material.
void block(Grid cells, int x0, int y0, int x1, int y1, Material material)
{
    // Visit each row in the inclusive-vertical, exclusive-horizontal rectangle.
    for (int y = y0; y < y1; ++y)
    {
        // Stamp the material across the row span.
        for (int x = x0; x < x1; ++x)
        {
            cells[y, x] = makeCell(material);
        }
    }
}

// Paint a filled ellipse by testing normalized distance from the center.
void ellipse(Grid cells, int cx, int cy, int rx, int ry, Material material)
{
    // Scan the ellipse bounding box.
    for (int y = cy - ry; y <= cy + ry; ++y)
    {
        for (int x = cx - rx; x <= cx + rx; ++x)
        {
            // Keep pixels inside the normalized unit circle.
            const auto dx = static_cast<float>(x - cx) / static_cast<float>(rx);
            const auto dy = static_cast<float>(y - cy) / static_cast<float>(ry);
            if (dx * dx + dy * dy <= 1.0F)
            {
                cells[y, x] = makeCell(material);
            }
        }
    }
}

// Stroke a thick line between two points by interpolating along x and stamping vertical blocks.
void beam(Grid cells, int x0, int y0, int x1, int y1, int thick, Material material)
{
    // views::iota is a view of x coordinates. The loop reads it in place and does not
    // build a container of indexes. The source of the view is the iterator pair itself.
    // Step along x and stamp a vertical slab at the interpolated y.
    for (int x : std::views::iota(x0, x1))
    {
        const auto t = static_cast<float>(x - x0) / static_cast<float>(x1 - x0);
        const int y =
            static_cast<int>(std::lerp(static_cast<float>(y0), static_cast<float>(y1), t));
        block(cells, x, y, x + 1, y + thick, material);
    }
}

void waterworks(Grid cells)
{
    // using enum brings this function's Material enumerators into scope by name.
    using enum Material;

    // Terraces, holding cups, a splash baffle, and a deep catch basin.
    beam(cells, 20, 98, 148, 124, 7, stone);
    block(cells, 20, 36, 26, 101, stone);
    block(cells, 91, 91, 97, 113, stone);
    block(cells, 25, 66, 91, 99, water);
    for (int x = 26; x < 91; ++x)
    {
        int bottom = 98 + (x - 20) * 26 / 128;
        block(cells, x, 98, x + 1, bottom, water);
    }
    block(cells, 91, 90, 97, 112, stone);
    ellipse(cells, 121, 53, 17, 9, water);

    // Upper-right terraces, spillway, and mixed oil–water channel.
    beam(cells, 131, 154, 214, 170, 8, stone);
    beam(cells, 205, 171, 269, 146, 8, stone);
    block(cells, 266, 132, 272, 154, stone);
    block(cells, 174, 154, 181, 164, wood);
    block(cells, 205, 164, 234, 167, water);

    beam(cells, 265, 109, 360, 83, 8, stone);
    block(cells, 354, 27, 360, 88, stone);
    block(cells, 305, 74, 310, 99, stone);

    // Layer oil above water along the sloped run.
    for (int x = 310; x < 354; ++x)
    {
        const int bottom = 109 + (x - 265) * -26 / 95;
        block(cells, x, 58 + (x % 6 == 0 ? 1 : 0), x + 1, 69, oil);
        block(cells, x, 69, x + 1, bottom - 1, water);
    }
    ellipse(cells, 311, 34, 18, 8, sand);
    ellipse(cells, 76, 25, 13, 5, sand);

    // Shared catch basin with sand traps, ice plugs, and side walls.
    block(cells, 14, 180, 21, 221, stone);
    block(cells, 350, 174, 358, 221, stone);
    block(cells, 14, 215, 358, 221, stone);
    block(cells, 21, 193, 350, 215, water);
    block(cells, 64, 210, 105, 215, sand);
    block(cells, 109, 212, 144, 215, sand);
    block(cells, 281, 209, 326, 215, sand);
    block(cells, 318, 186, 350, 193, ice);
    block(cells, 328, 181, 350, 187, ice);
    block(cells, 341, 177, 350, 186, ice);
}

// Lava pour, water runoff, wood fuel, and a shared catch basin.
void thermal(Grid cells)
{
    // using enum, as in waterworks, so stone and lava name Material enumerators.
    using enum Material;
    beam(cells, 20, 112, 152, 133, 9, stone);
    block(cells, 20, 41, 28, 115, stone);
    block(cells, 28, 78, 112, 112, lava);
    ellipse(cells, 139, 90, 18, 9, lava);

    // Coolant runway and ice source on the right.
    beam(cells, 201, 146, 361, 95, 10, stone);
    block(cells, 353, 28, 361, 103, stone);
    for (int x = 252; x < 353; ++x)
    {
        const int y = 146 + (x - 201) * -51 / 160;
        block(cells, x, 80, x + 1, y, water);
    }
    ellipse(cells, 342, 67, 15, 8, ice);
    block(cells, 353, 28, 361, 103, stone);

    // Wood fuel columns and flooded floor pan.
    block(cells, 70, 209, 314, 221, stone);
    block(cells, 70, 171, 79, 209, stone);
    block(cells, 305, 173, 314, 209, stone);
    block(cells, 86, 189, 150, 192, wood);
    block(cells, 90, 192, 94, 209, wood);
    block(cells, 145, 192, 149, 209, wood);
    block(cells, 79, 200, 305, 209, water);
}

// Oil and water separation demo with a burner and settling pool.
void refinery(Grid cells)
{
    // using enum, as in waterworks, so oil, water, and fire name Material enumerators.
    using enum Material;
    beam(cells, 24, 100, 161, 125, 8, stone);
    block(cells, 24, 31, 31, 107, stone);
    block(cells, 31, 70, 105, 89, oil);
    block(cells, 31, 89, 105, 100, water);
    ellipse(cells, 123, 72, 14, 9, oil);

    // Burner deck, filter sand bed, and separation pool.
    beam(cells, 185, 150, 357, 108, 8, stone);
    block(cells, 350, 50, 357, 114, stone);
    ellipse(cells, 307, 97, 24, 9, sand);
    block(cells, 232, 110, 263, 116, wood);
    block(cells, 298, 49, 331, 54, wood);
    block(cells, 308, 44, 315, 49, fire);
    block(cells, 62, 209, 323, 220, stone);
    block(cells, 62, 161, 70, 213, stone);
    block(cells, 315, 163, 323, 213, stone);
    block(cells, 70, 191, 315, 209, water);
    block(cells, 70, 184, 315, 191, oil);
    block(cells, 191, 191, 196, 209, stone);
}

// The magic bytes are a constexpr array, so the file signature is fixed at compile time.
constexpr std::array<char, 8> magic{'S', 'A', 'N', 'D', '0', '0', '0', '1'};

// bit_cast copies host bits. The save format is little-endian, so this build checks that
// the host order matches the file order before those bits are written out.
static_assert(std::endian::native == std::endian::little);

void putWord(std::ostream& output, std::uint32_t word)
{
    // bit_cast forms a char array from the word's object representation. Both types
    // are trivially copyable and the same size, which is what the operation requires.
    const auto bytes = std::bit_cast<std::array<char, 4>>(word);
    output.write(bytes.data(), bytes.size());
}

std::uint32_t getWord(std::istream& input)
{
    std::array<char, 4> bytes{};
    input.read(bytes.data(), bytes.size());

    // The reverse of putWord: bit_cast rebuilds the integer from the four file bytes.
    return std::bit_cast<std::uint32_t>(bytes);
}

// Reject cells with reserved matter bits, unknown materials, or invalid liquid sums.
bool validCell(const Cell& c)
{
    // to_underlying reads the enum class as its integer without a hand-written cast.
    // all_of applies a capture-free lambda to each liquid channel. The lambda lives
    // only for this call, so it does not need to own or borrow any surrounding state.
    return (c.matter >> 26) == 0 && (c.matter & 255u) <= std::to_underlying(Material::ice) &&
           std::ranges::all_of(c.liquid, [](float v) { return v >= 0.0F && v <= 1.001F; }) &&
           c.liquid[0] + c.liquid[1] + c.liquid[2] <= 1.001F;
}

} // namespace

std::vector<Cell> makeScene(Scene scene)
{
    // The vector owns the cells. The mdspan is a view of that storage and must not outlive it.
    // Allocate an empty world buffer and view it as a height × width grid.
    std::vector<Cell> cells(worldCells);
    Grid grid(cells.data());

    // Dispatch the preset scene builder.
    switch (scene)
    {
    case Scene::waterworks:
        waterworks(grid);
        break;
    case Scene::thermal:
        thermal(grid);
        break;
    case Scene::refinery:
        refinery(grid);
        break;
    case Scene::empty:
        break;
    }

    // Return the named local directly so the compiler can construct it in the caller.
    return cells;
}

std::expected<void, std::string> saveWorld(const std::filesystem::path& path,
                                           std::span<const Cell> cells)
{
    // The span borrows the caller's cells for this call. expected<void, string> carries
    // success, or a reason when validation or the filesystem operation fails.
    // Refuse saves that are the wrong size or contain invalid cells.
    if (cells.size() != worldCells || !std::ranges::all_of(cells, validCell))
    {
        return std::unexpected("INVALID WORLD DATA");
    }

    // Stage writes beside the destination so a partial file never overwrites the save.
    auto temporary = path;
    temporary += L".tmp";

    // ofstream owns the temporary file. close() finishes the write before the replace below.
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

    // Write header and little-endian cell payload.
    file.write(magic.data(), magic.size());
    putWord(file, worldWidth);
    putWord(file, worldHeight);

    // Serialize each cell as three liquid floats followed by packed matter.
    for (const Cell& cell : cells)
    {
        for (float v : cell.liquid)
        {
            // Same-size trivially copyable types: the float's bits travel as a uint32_t.
            putWord(file, std::bit_cast<std::uint32_t>(v));
        }
        putWord(file, cell.matter);
    }
    file.close();

    // Atomically replace the destination file on success.
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        // The error_code overload stores the result in error. Cleanup stays best-effort.
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return std::unexpected("COULD NOT SAVE WORLD");
    }

    return {};
}

std::expected<std::vector<Cell>, std::string> loadWorld(const std::filesystem::path& path)
{
    // ifstream owns the file handle and closes it when this function returns.
    std::ifstream file(path, std::ios::binary);
    std::array<char, magic.size()> header{};

    // Read the file magic and grid dimensions.
    file.read(header.data(), header.size());

    // Validate magic and grid dimensions before reading cells.
    if (header != magic || getWord(file) != worldWidth || getWord(file) != worldHeight)
    {
        return std::unexpected("NO COMPATIBLE SAVE");
    }

    std::vector<Cell> cells(worldCells);

    // Deserialize liquid channels and packed matter for every grid cell.
    for (Cell& cell : cells)
    {
        for (float& v : cell.liquid)
        {
            // A float and a uint32_t are the same size and trivially copyable, so bit_cast
            // rebuilds the liquid value from the bits that putWord stored.
            v = std::bit_cast<float>(getWord(file));
        }
        cell.matter = getWord(file);
    }

    // Reject truncated files or payloads that fail cell validation.
    if (!file || file.peek() != std::char_traits<char>::eof() ||
        !std::ranges::all_of(cells, validCell))
    {
        return std::unexpected("SAVE DATA IS DAMAGED");
    }

    return cells;
}

} // namespace sand