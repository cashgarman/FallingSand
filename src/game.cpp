#include "game.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mdspan>
#include <numbers>
#include <ranges>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <windows.h>

namespace sand {

// Palette metadata table shared by the UI and the layout JSON loader.
const std::array<MaterialInfo, materialCount> materials{
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

// Scene tab labels in the same order as Scene and layoutFiles.
const std::array<std::string_view, sceneCount> sceneNames{"WATERWORKS", "THERMAL VENT", "REFINERY",
                                                          "EMPTY"};

Material kind(const Cell& cell)
{
    // The low byte holds the material id. The cast back to the enum class is explicit.
    return static_cast<Material>(cell.matter & 255u);
}

Material displayMaterial(const Cell& cell)
{
    // Read the three liquid channels before falling back to packed matter.
    const float water = cell.liquid[0];
    const float oil = cell.liquid[1];
    const float lava = cell.liquid[2];
    if (water > 0.01F || oil > 0.01F || lava > 0.01F)
    {
        // Prefer the dominant liquid when any channel holds visible fill.
        if (water >= oil && water >= lava)
        {
            return Material::water;
        }
        if (oil >= water && oil >= lava)
        {
            return Material::oil;
        }
        return Material::lava;
    }

    // No significant liquid: use the solid or gas id in matter.
    return kind(cell);
}

std::optional<std::size_t> paletteIndex(Material id)
{
    if (id == Material::empty)
    {
        return std::nullopt;
    }

    // Scan the palette table for the first matching material id.
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        if (materials[i].id == id)
        {
            return i;
        }
    }
    return std::nullopt;
}

Cell makeCell(Material id, float quantity)
{
    // Value-initialize every field, then fill only the channels this material uses.
    // constexpr lets a caller build the cell at compile time when the arguments are constant.
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

bool Rect::contains(Point p) const
{
    // Half-open test: include the minimum edge, exclude the maximum edge.
    return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
}

// Helpers in this unnamed namespace have internal linkage. They belong to this
// translation unit only, so they cannot collide with the same name in another file.
// Scene geometry is loaded from JSON. These painters only stamp a grid view.
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

        // Stamp a one-cell-wide slab at the interpolated height.
        block(cells, x, y, x + 1, y + thick, material);
    }
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

// Layout files are JSON objects { "shapes": [ ... ] } read from the layouts directory
// beside the executable. Each shape's "op" is block, ellipse, beam, or columns.
// block and beam use an exclusive x1 and y1. ellipse uses a center and radii.
// columns fills [x0, x1). An edge is an integer, or one of these objects:
//   { "base", "anchor", "rise", "run", "offset"? } is
//   base + (x - anchor) * rise / run + offset, with truncating division.
//   { "base", "every", "extra" } is base + (x % every == 0 ? extra : 0).
// Materials are the lowercase palette names. An optional "note" is ignored.

struct ColumnEdge
{
    enum class Form : std::uint8_t
    {
        constant,
        linear,
        periodic
    };

    Form form{Form::constant};
    int base{};
    int anchor{};
    int rise{};
    int run{1};
    int every{1};
    int extra{};
    int offset{};

    // expected carries a y coordinate, or a reason the slope left the int range.
    // The arithmetic is int64 so a huge JSON value fails instead of overflowing.
    [[nodiscard]] std::expected<int, std::string> at(int x) const
    {
        // Evaluate the edge in 64-bit arithmetic, then reject a result that does not fit in int.
        std::int64_t y = base;

        // Apply the constant, linear, or periodic edge formula for column x.
        switch (form)
        {
        case Form::constant:
            break;
        case Form::linear:
            y = static_cast<std::int64_t>(base) +
                (static_cast<std::int64_t>(x) - anchor) * rise / run + offset;
            break;
        case Form::periodic:
            y = static_cast<std::int64_t>(base) + (x % every == 0 ? extra : 0);
            break;
        default:
            std::unreachable();
        }

        // Truncate back to int only when the slope stayed inside the grid range.
        if (y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max())
        {
            return std::unexpected("LAYOUT EDGE IS OUT OF RANGE");
        }
        return static_cast<int>(y);
    }
};

// variant is the JSON value. Object and Array recurse through Json, which vector
// allows while Json is still being defined. The value owns its text and children.
struct Json
{
    struct Object
    {
        std::vector<std::pair<std::string, Json>> fields;

        // string_view borrows the caller's key. find does not allocate.
        [[nodiscard]] const Json* find(std::string_view key) const
        {
            const auto found = std::ranges::find_if(
                fields, [key](const auto& field) { return field.first == key; });
            return found == fields.end() ? nullptr : &found->second;
        }
    };

    using Array = std::vector<Json>;
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, std::string, Array, Object>;
    Value value;
};

// Constrained so only a real JSON alternative can be stored. Assignment selects it.
template <class T>
    requires std::is_assignable_v<Json::Value&, T&&>
[[nodiscard]] Json jsonValue(T value)
{
    Json json;
    json.value = std::move(value);
    return json;
}

// Encode one Unicode scalar as UTF-8 and append those bytes to the owned string.
void appendUtf8(std::string& out, char32_t code)
{
    // One byte covers ASCII.
    if (code <= 0x7F)
    {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF)
    {
        // Two-byte sequence.
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF)
    {
        // Three-byte sequence.
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else
    {
        // Four-byte sequence.
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// The parser borrows the file text. Every string it returns is owned by a Json value.
class JsonParser
{
  public:
    explicit JsonParser(std::string_view text) : text_(text)
    {
        // Drop a leading UTF-8 byte order mark so the first value starts at a token.
        constexpr std::string_view bom{"\xEF\xBB\xBF"};
        if (text_.starts_with(bom))
        {
            text_.remove_prefix(bom.size());
        }
    }

    [[nodiscard]] std::expected<Json, std::string> parse()
    {
        // One value, then nothing but whitespace. Extra tokens are a layout error.
        auto value = parseValue();
        if (!value)
        {
            return value;
        }
        skip();
        if (index_ != text_.size())
        {
            return std::unexpected(fault("EXTRA DATA AFTER THE LAYOUT"));
        }
        return value;
    }

  private:
    std::string_view text_;
    std::size_t index_{};

    [[nodiscard]] std::string fault(std::string_view reason) const
    {
        return std::format("LAYOUT JSON AT {}: {}", index_, reason);
    }

    void skip()
    {
        // contains is the C++23 search on a view. The view does not own the whitespace.
        constexpr std::string_view whitespace{" \t\r\n"};
        while (index_ < text_.size() && whitespace.contains(text_[index_]))
        {
            ++index_;
        }
    }

    [[nodiscard]] static constexpr bool digit(char c)
    {
        return c >= '0' && c <= '9';
    }

    [[nodiscard]] std::expected<int, std::string> hex4()
    {
        // Four hex digits are required. A short tail is not a valid \u escape.
        if (index_ + 4 > text_.size())
        {
            return std::unexpected(fault("SHORT UNICODE ESCAPE"));
        }
        int code = 0;
        const char* first = text_.data() + index_;
        // from_chars parses the four hex digits without a locale or a prefix.
        const auto [ptr, error] = std::from_chars(first, first + 4, code, 16);
        if (error != std::errc{} || ptr != first + 4)
        {
            return std::unexpected(fault("BAD UNICODE ESCAPE"));
        }
        index_ += 4;
        return code;
    }

    [[nodiscard]] std::expected<void, std::string> appendUnicode(std::string& out)
    {
        // A high surrogate must be followed by a low surrogate. A lone half is rejected.
        auto code = hex4();
        if (!code)
        {
            return std::unexpected(code.error());
        }
        auto point = static_cast<char32_t>(*code);
        if (point >= 0xD800 && point <= 0xDBFF)
        {
            if (index_ + 1 >= text_.size() || text_[index_] != '\\' || text_[index_ + 1] != 'u')
            {
                return std::unexpected(fault("LONE SURROGATE"));
            }
            index_ += 2;
            auto low = hex4();
            if (!low)
            {
                return std::unexpected(low.error());
            }
            if (*low < 0xDC00 || *low > 0xDFFF)
            {
                return std::unexpected(fault("BAD SURROGATE PAIR"));
            }
            point = 0x10000 + ((point - 0xD800) << 10) + (static_cast<char32_t>(*low) - 0xDC00);
        } else if (point >= 0xDC00 && point <= 0xDFFF)
        {
            return std::unexpected(fault("LONE SURROGATE"));
        }
        appendUtf8(out, point);
        return {};
    }

    [[nodiscard]] std::expected<std::string, std::string> parseString()
    {
        // The opening quote is already identified. Copy bytes until the closing quote.
        ++index_;
        std::string out;
        while (index_ < text_.size())
        {
            const char c = text_[index_++];
            if (c == '"')
            {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20)
            {
                return std::unexpected(fault("UNESCAPED CONTROL CHARACTER"));
            }
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }

            // Expand backslash escapes and \u UTF-16 pairs.
            if (index_ >= text_.size())
            {
                return std::unexpected(fault("UNTERMINATED ESCAPE"));
            }
            const char escaped = text_[index_++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                out.push_back(escaped);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
                if (auto encoded = appendUnicode(out); !encoded)
                {
                    return std::unexpected(encoded.error());
                }
                break;
            default:
                return std::unexpected(fault("UNKNOWN ESCAPE"));
            }
        }
        return std::unexpected(fault("UNTERMINATED STRING"));
    }

    [[nodiscard]] std::expected<Json, std::string> parseNumber()
    {
        // Remember the first character, including a leading minus, for from_chars.
        const auto start = index_;
        if (text_[index_] == '-')
        {
            ++index_;
        }
        if (index_ >= text_.size() || !digit(text_[index_]))
        {
            return std::unexpected(fault("EXPECTED A DIGIT"));
        }
        while (index_ < text_.size() && digit(text_[index_]))
        {
            ++index_;
        }
        if (index_ < text_.size() &&
            (text_[index_] == '.' || text_[index_] == 'e' || text_[index_] == 'E'))
        {
            // Slopes use truncating integer division. A float would change the worlds.
            return std::unexpected(fault("LAYOUT NUMBERS MUST BE INTEGERS"));
        }

        // Parse the digit run as a signed 64-bit integer.
        std::int64_t number = 0;
        const char* first = text_.data() + start;
        const char* last = text_.data() + index_;
        const auto [ptr, error] = std::from_chars(first, last, number);
        if (error != std::errc{} || ptr != last)
        {
            return std::unexpected(fault("INTEGER IS OUT OF RANGE"));
        }
        return jsonValue(number);
    }

    [[nodiscard]] std::expected<Json, std::string> parseObject()
    {
        // Fields are "name" : value, separated by commas. A trailing comma is rejected.
        ++index_;
        Json::Object object;
        skip();
        if (index_ < text_.size() && text_[index_] == '}')
        {
            ++index_;
            return jsonValue(std::move(object));
        }
        for (;;)
        {
            skip();
            if (index_ >= text_.size() || text_[index_] != '"')
            {
                return std::unexpected(fault("EXPECTED A FIELD NAME"));
            }
            auto key = parseString();
            if (!key)
            {
                return std::unexpected(key.error());
            }
            if (object.find(*key) != nullptr)
            {
                return std::unexpected(fault("DUPLICATE FIELD"));
            }
            skip();
            if (index_ >= text_.size() || text_[index_] != ':')
            {
                return std::unexpected(fault("EXPECTED ':'"));
            }
            ++index_;
            auto value = parseValue();
            if (!value)
            {
                return value;
            }
            object.fields.emplace_back(std::move(*key), std::move(*value));
            skip();
            if (index_ >= text_.size())
            {
                return std::unexpected(fault("UNTERMINATED OBJECT"));
            }
            if (text_[index_] == '}')
            {
                ++index_;
                return jsonValue(std::move(object));
            }
            if (text_[index_] != ',')
            {
                return std::unexpected(fault("EXPECTED ',' OR '}'"));
            }
            ++index_;
            skip();
            if (index_ < text_.size() && text_[index_] == '}')
            {
                return std::unexpected(fault("TRAILING COMMA"));
            }
        }
    }

    [[nodiscard]] std::expected<Json, std::string> parseArray()
    {
        // Values are separated by commas. A trailing comma is rejected.
        ++index_;
        Json::Array array;
        skip();
        if (index_ < text_.size() && text_[index_] == ']')
        {
            ++index_;
            return jsonValue(std::move(array));
        }
        for (;;)
        {
            auto value = parseValue();
            if (!value)
            {
                return value;
            }
            array.push_back(std::move(*value));
            skip();
            if (index_ >= text_.size())
            {
                return std::unexpected(fault("UNTERMINATED ARRAY"));
            }
            if (text_[index_] == ']')
            {
                ++index_;
                return jsonValue(std::move(array));
            }
            if (text_[index_] != ',')
            {
                return std::unexpected(fault("EXPECTED ',' OR ']'"));
            }
            ++index_;
            skip();
            if (index_ < text_.size() && text_[index_] == ']')
            {
                return std::unexpected(fault("TRAILING COMMA"));
            }
        }
    }

    [[nodiscard]] std::expected<Json, std::string> parseValue()
    {
        // The first character selects object, array, string, boolean, null, or number.
        skip();
        if (index_ >= text_.size())
        {
            return std::unexpected(fault("UNEXPECTED END"));
        }
        const char c = text_[index_];
        if (c == '{')
        {
            return parseObject();
        }
        if (c == '[')
        {
            return parseArray();
        }
        if (c == '"')
        {
            auto text = parseString();
            if (!text)
            {
                return std::unexpected(text.error());
            }
            return jsonValue(std::move(*text));
        }
        if (c == 't' || c == 'f')
        {
            const std::string_view word = c == 't' ? "true" : "false";
            if (!text_.substr(index_).starts_with(word))
            {
                return std::unexpected(fault("EXPECTED A BOOLEAN"));
            }
            index_ += word.size();
            return jsonValue(c == 't');
        }
        if (c == 'n')
        {
            if (!text_.substr(index_).starts_with("null"))
            {
                return std::unexpected(fault("EXPECTED NULL"));
            }
            index_ += 4;
            return jsonValue(nullptr);
        }
        if (c == '-' || digit(c))
        {
            return parseNumber();
        }
        return std::unexpected(fault("EXPECTED A VALUE"));
    }
};

// File order matches Scene, including the empty canvas.
constexpr std::array layoutFiles{
    std::string_view{"waterworks.json"},
    std::string_view{"thermal.json"},
    std::string_view{"refinery.json"},
    std::string_view{"empty.json"},
};
static_assert(layoutFiles.size() == sceneCount);

constexpr std::array materialNames{
    std::pair{std::string_view{"sand"}, Material::sand},
    std::pair{std::string_view{"water"}, Material::water},
    std::pair{std::string_view{"oil"}, Material::oil},
    std::pair{std::string_view{"stone"}, Material::stone},
    std::pair{std::string_view{"wood"}, Material::wood},
    std::pair{std::string_view{"fire"}, Material::fire},
    std::pair{std::string_view{"smoke"}, Material::smoke},
    std::pair{std::string_view{"steam"}, Material::steam},
    std::pair{std::string_view{"lava"}, Material::lava},
    std::pair{std::string_view{"ice"}, Material::ice},
};

// Palette order used only to verify materialNames at compile time.
constexpr std::array paletteMaterialIds{
    Material::sand, Material::water, Material::oil,   Material::stone, Material::wood,
    Material::fire, Material::smoke, Material::steam, Material::lava,  Material::ice};

// A new palette entry has to gain a JSON name or this constant check fails.
[[nodiscard]] constexpr bool paletteNamesCoverMaterials()
{
    // Every palette material needs exactly one JSON name, and no extra names.
    if (materialNames.size() != materialCount)
    {
        return false;
    }
    for (Material id : paletteMaterialIds)
    {
        bool found = false;

        // Each palette id must appear exactly once in the JSON name table.
        for (const auto& name : materialNames)
        {
            if (name.second == id)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}
static_assert(paletteNamesCoverMaterials());

[[nodiscard]] std::expected<void, std::string> allowOnly(
    const Json::Object& object, std::initializer_list<std::string_view> keys)
{
    // Reject a field whose name is not in the allowlist for this shape.
    for (const auto& field : object.fields)
    {
        // contains walks the key list in place. It does not build a set.
        if (!std::ranges::contains(keys, std::string_view{field.first}))
        {
            return std::unexpected(std::format("UNKNOWN LAYOUT FIELD {}", field.first));
        }
    }
    return {};
}

[[nodiscard]] std::expected<int, std::string> expectInt(const Json::Object& object,
                                                        std::string_view key)
{
    // Missing, non-integer, and out-of-range fields each become a layout error.
    const Json* field = object.find(key);
    if (field == nullptr)
    {
        return std::unexpected(std::format("LAYOUT IS MISSING {}", key));
    }
    const auto* number = std::get_if<std::int64_t>(&field->value);
    if (number == nullptr)
    {
        return std::unexpected(std::format("LAYOUT FIELD {} MUST BE AN INTEGER", key));
    }
    if (*number < std::numeric_limits<int>::min() || *number > std::numeric_limits<int>::max())
    {
        return std::unexpected(std::format("LAYOUT FIELD {} IS OUT OF RANGE", key));
    }
    return static_cast<int>(*number);
}

[[nodiscard]] std::expected<Material, std::string> expectMaterial(const Json::Object& object)
{
    // The material field must be a known lowercase palette name.
    const Json* field = object.find("material");
    if (field == nullptr)
    {
        return std::unexpected("LAYOUT IS MISSING MATERIAL");
    }
    const auto* name = std::get_if<std::string>(&field->value);
    if (name == nullptr)
    {
        return std::unexpected("LAYOUT MATERIAL MUST BE A STRING");
    }
    const auto found =
        std::ranges::find_if(materialNames, [&](const auto& item) { return item.first == *name; });
    if (found == materialNames.end())
    {
        return std::unexpected(std::format("UNKNOWN MATERIAL {}", *name));
    }
    return found->second;
}

[[nodiscard]] std::expected<void, std::string> optionalNote(const Json::Object& object)
{
    // note is optional. When present it must be a string, and it is not painted.
    const Json* note = object.find("note");
    if (note != nullptr && std::get_if<std::string>(&note->value) == nullptr)
    {
        return std::unexpected("LAYOUT NOTE MUST BE A STRING");
    }
    return {};
}

[[nodiscard]] std::expected<ColumnEdge, std::string> expectEdge(const Json::Object& object,
                                                                std::string_view key)
{
    // An edge is a bare integer, a linear slope, or a periodic bump. Exactly one form.
    const Json* field = object.find(key);
    if (field == nullptr)
    {
        return std::unexpected(std::format("LAYOUT IS MISSING {}", key));
    }
    if (const auto* number = std::get_if<std::int64_t>(&field->value))
    {
        // A bare integer is a constant edge height for every column.
        if (*number < std::numeric_limits<int>::min() || *number > std::numeric_limits<int>::max())
        {
            return std::unexpected(std::format("LAYOUT FIELD {} IS OUT OF RANGE", key));
        }
        ColumnEdge edge;
        edge.base = static_cast<int>(*number);
        return edge;
    }
    const auto* spec = std::get_if<Json::Object>(&field->value);
    if (spec == nullptr)
    {
        return std::unexpected(std::format("LAYOUT FIELD {} MUST BE AN INTEGER OR AN EDGE", key));
    }
    const bool linear = spec->find("rise") != nullptr;
    const bool periodic = spec->find("every") != nullptr;
    if (linear == periodic)
    {
        return std::unexpected(std::format("LAYOUT EDGE {} NEEDS RISE OR EVERY", key));
    }
    ColumnEdge edge;
    if (linear)
    {
        // Linear edge: base + (x - anchor) * rise / run, with an optional offset.
        if (auto keys = allowOnly(*spec, {"base", "anchor", "rise", "run", "offset"}); !keys)
        {
            return std::unexpected(keys.error());
        }
        edge.form = ColumnEdge::Form::linear;
        auto base = expectInt(*spec, "base");
        if (!base)
        {
            return std::unexpected(base.error());
        }
        auto anchor = expectInt(*spec, "anchor");
        if (!anchor)
        {
            return std::unexpected(anchor.error());
        }
        auto rise = expectInt(*spec, "rise");
        if (!rise)
        {
            return std::unexpected(rise.error());
        }
        auto run = expectInt(*spec, "run");
        if (!run)
        {
            return std::unexpected(run.error());
        }
        if (spec->find("offset") != nullptr)
        {
            auto offset = expectInt(*spec, "offset");
            if (!offset)
            {
                return std::unexpected(offset.error());
            }
            edge.offset = *offset;
        }
        if (*run == 0)
        {
            return std::unexpected("LAYOUT EDGE RUN MUST NOT BE ZERO");
        }
        edge.base = *base;
        edge.anchor = *anchor;
        edge.rise = *rise;
        edge.run = *run;
        return edge;
    }

    // Periodic edge: add extra on every nth column.
    if (auto keys = allowOnly(*spec, {"base", "every", "extra"}); !keys)
    {
        return std::unexpected(keys.error());
    }
    edge.form = ColumnEdge::Form::periodic;
    auto base = expectInt(*spec, "base");
    if (!base)
    {
        return std::unexpected(base.error());
    }
    auto every = expectInt(*spec, "every");
    if (!every)
    {
        return std::unexpected(every.error());
    }
    auto extra = expectInt(*spec, "extra");
    if (!extra)
    {
        return std::unexpected(extra.error());
    }
    if (*every <= 0)
    {
        return std::unexpected("LAYOUT EDGE EVERY MUST BE POSITIVE");
    }
    edge.base = *base;
    edge.every = *every;
    edge.extra = *extra;
    return edge;
}

[[nodiscard]] std::expected<void, std::string> paintBlock(Grid cells, int x0, int y0, int x1,
                                                          int y1, Material material)
{
    // Reject an inverted or out-of-grid rectangle before stamping cells.
    if (x0 > x1 || y0 > y1)
    {
        return std::unexpected(
            std::format("LAYOUT RECTANGLE {},{} {},{} IS INVERTED", x0, y0, x1, y1));
    }
    if (x0 < 0 || y0 < 0 || x1 > worldWidth || y1 > worldHeight)
    {
        return std::unexpected(
            std::format("LAYOUT RECTANGLE {},{} {},{} IS OUTSIDE THE GRID", x0, y0, x1, y1));
    }
    block(cells, x0, y0, x1, y1, material);
    return {};
}

[[nodiscard]] std::expected<void, std::string> paintEllipse(Grid cells, int cx, int cy, int rx,
                                                            int ry, Material material)
{
    // The bounding box must sit inside the grid, and both radii must be positive.
    if (rx <= 0 || ry <= 0)
    {
        return std::unexpected("LAYOUT ELLIPSE RADIUS MUST BE POSITIVE");
    }
    // Reject ellipses whose bounding box crosses the grid edge.
    const auto left = static_cast<std::int64_t>(cx) - rx;
    const auto top = static_cast<std::int64_t>(cy) - ry;
    const auto right = static_cast<std::int64_t>(cx) + rx;
    const auto bottom = static_cast<std::int64_t>(cy) + ry;
    if (left < 0 || top < 0 || right >= worldWidth || bottom >= worldHeight)
    {
        return std::unexpected("LAYOUT ELLIPSE IS OUTSIDE THE GRID");
    }
    ellipse(cells, cx, cy, rx, ry, material);
    return {};
}

[[nodiscard]] std::expected<void, std::string> paintBeam(Grid cells, int x0, int y0, int x1, int y1,
                                                         int thick, Material material)
{
    // Check the span and every interpolated slab before painting any of them.
    if (thick <= 0)
    {
        return std::unexpected("LAYOUT BEAM THICKNESS MUST BE POSITIVE");
    }
    if (x1 <= x0)
    {
        return std::unexpected("LAYOUT BEAM HAS NO SPAN");
    }
    if (x0 < 0 || x1 > worldWidth)
    {
        return std::unexpected("LAYOUT BEAM IS OUTSIDE THE GRID");
    }
    // The same interpolation beam uses. iota is empty unless the span was checked above.
    for (int x : std::views::iota(x0, x1))
    {
        const auto t = static_cast<float>(x - x0) / static_cast<float>(x1 - x0);
        const int y =
            static_cast<int>(std::lerp(static_cast<float>(y0), static_cast<float>(y1), t));
        if (y < 0 || static_cast<std::int64_t>(y) + thick > worldHeight)
        {
            return std::unexpected(std::format("LAYOUT BEAM AT {} IS OUTSIDE THE GRID", x));
        }
    }
    beam(cells, x0, y0, x1, y1, thick, material);
    return {};
}

// One horizontal band in a columns shape: material between evaluated y0 and y1.
struct Layer
{
    Material material{};
    ColumnEdge top;
    ColumnEdge bottom;
};

[[nodiscard]] std::expected<std::vector<Layer>, std::string> expectLayers(
    const Json::Object& object)
{
    // Each layer names a material and the top and bottom edges of that band.
    const Json* field = object.find("layers");
    if (field == nullptr)
    {
        return std::unexpected("LAYOUT IS MISSING LAYERS");
    }
    const auto* list = std::get_if<Json::Array>(&field->value);
    if (list == nullptr)
    {
        return std::unexpected("LAYOUT LAYERS MUST BE AN ARRAY");
    }
    if (list->empty())
    {
        return std::unexpected("LAYOUT COLUMNS NEED A LAYER");
    }
    std::vector<Layer> layers;
    for (const Json& item : *list)
    {
        const auto* layer = std::get_if<Json::Object>(&item.value);
        if (layer == nullptr)
        {
            return std::unexpected("LAYOUT LAYER MUST BE AN OBJECT");
        }
        if (auto keys = allowOnly(*layer, {"material", "y0", "y1"}); !keys)
        {
            return std::unexpected(keys.error());
        }
        auto material = expectMaterial(*layer);
        if (!material)
        {
            return std::unexpected(material.error());
        }
        auto top = expectEdge(*layer, "y0");
        if (!top)
        {
            return std::unexpected(top.error());
        }
        auto bottom = expectEdge(*layer, "y1");
        if (!bottom)
        {
            return std::unexpected(bottom.error());
        }

        // Store the validated band for paintColumns.
        layers.push_back(Layer{*material, *top, *bottom});
    }
    return layers;
}

[[nodiscard]] std::expected<void, std::string> paintBlockShape(Grid cells,
                                                               const Json::Object& object)
{
    // Read the rectangle fields, then hand the numbers to the bounds check.
    if (auto keys = allowOnly(object, {"op", "note", "x0", "y0", "x1", "y1", "material"}); !keys)
    {
        return keys;
    }
    auto x0 = expectInt(object, "x0");
    if (!x0)
    {
        return std::unexpected(x0.error());
    }
    auto y0 = expectInt(object, "y0");
    if (!y0)
    {
        return std::unexpected(y0.error());
    }
    auto x1 = expectInt(object, "x1");
    if (!x1)
    {
        return std::unexpected(x1.error());
    }
    auto y1 = expectInt(object, "y1");
    if (!y1)
    {
        return std::unexpected(y1.error());
    }
    auto material = expectMaterial(object);
    if (!material)
    {
        return std::unexpected(material.error());
    }
    return paintBlock(cells, *x0, *y0, *x1, *y1, *material);
}

[[nodiscard]] std::expected<void, std::string> paintEllipseShape(Grid cells,
                                                                 const Json::Object& object)
{
    // Read the center, radii, and material, then hand them to the bounds check.
    if (auto keys = allowOnly(object, {"op", "note", "cx", "cy", "rx", "ry", "material"}); !keys)
    {
        return keys;
    }
    auto cx = expectInt(object, "cx");
    if (!cx)
    {
        return std::unexpected(cx.error());
    }
    auto cy = expectInt(object, "cy");
    if (!cy)
    {
        return std::unexpected(cy.error());
    }
    auto rx = expectInt(object, "rx");
    if (!rx)
    {
        return std::unexpected(rx.error());
    }
    auto ry = expectInt(object, "ry");
    if (!ry)
    {
        return std::unexpected(ry.error());
    }
    auto material = expectMaterial(object);
    if (!material)
    {
        return std::unexpected(material.error());
    }
    return paintEllipse(cells, *cx, *cy, *rx, *ry, *material);
}

[[nodiscard]] std::expected<void, std::string> paintBeamShape(Grid cells,
                                                              const Json::Object& object)
{
    // Read the beam endpoints, thickness, and material, then validate the span.
    if (auto keys = allowOnly(object, {"op", "note", "x0", "y0", "x1", "y1", "thick", "material"});
        !keys)
    {
        return keys;
    }
    auto x0 = expectInt(object, "x0");
    if (!x0)
    {
        return std::unexpected(x0.error());
    }
    auto y0 = expectInt(object, "y0");
    if (!y0)
    {
        return std::unexpected(y0.error());
    }
    auto x1 = expectInt(object, "x1");
    if (!x1)
    {
        return std::unexpected(x1.error());
    }
    auto y1 = expectInt(object, "y1");
    if (!y1)
    {
        return std::unexpected(y1.error());
    }
    auto thick = expectInt(object, "thick");
    if (!thick)
    {
        return std::unexpected(thick.error());
    }
    auto material = expectMaterial(object);
    if (!material)
    {
        return std::unexpected(material.error());
    }
    return paintBeam(cells, *x0, *y0, *x1, *y1, *thick, *material);
}

[[nodiscard]] std::expected<void, std::string> paintColumns(Grid cells, int x0, int x1,
                                                            std::span<const Layer> layers)
{
    // Each column evaluates every layer's edges, then stamps that vertical span.
    if (x0 >= x1)
    {
        return std::unexpected("LAYOUT COLUMNS HAVE NO SPAN");
    }
    if (x0 < 0 || x1 > worldWidth)
    {
        return std::unexpected("LAYOUT COLUMNS ARE OUTSIDE THE GRID");
    }
    for (int x : std::views::iota(x0, x1))
    {
        // Stack every layer band at this column x.
        for (const Layer& layer : layers)
        {
            auto top = layer.top.at(x);
            if (!top)
            {
                return std::unexpected(top.error());
            }
            auto bottom = layer.bottom.at(x);
            if (!bottom)
            {
                return std::unexpected(bottom.error());
            }
            if (auto painted = paintBlock(cells, x, *top, x + 1, *bottom, layer.material); !painted)
            {
                return painted;
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, std::string> paintColumnsShape(Grid cells,
                                                                 const Json::Object& object)
{
    // Read the column span and its layers, then paint each evaluated band.
    if (auto keys = allowOnly(object, {"op", "note", "x0", "x1", "layers"}); !keys)
    {
        return keys;
    }
    auto x0 = expectInt(object, "x0");
    if (!x0)
    {
        return std::unexpected(x0.error());
    }
    auto x1 = expectInt(object, "x1");
    if (!x1)
    {
        return std::unexpected(x1.error());
    }
    auto layers = expectLayers(object);
    if (!layers)
    {
        return std::unexpected(layers.error());
    }
    return paintColumns(cells, *x0, *x1, *layers);
}

[[nodiscard]] std::expected<void, std::string> paintShape(Grid cells, const Json& shape)
{
    // op selects block, ellipse, beam, or columns. Anything else is rejected.
    const auto* object = std::get_if<Json::Object>(&shape.value);
    if (object == nullptr)
    {
        return std::unexpected("LAYOUT SHAPE MUST BE AN OBJECT");
    }
    if (auto note = optionalNote(*object); !note)
    {
        return note;
    }
    const Json* opField = object->find("op");
    if (opField == nullptr)
    {
        return std::unexpected("LAYOUT SHAPE IS MISSING OP");
    }
    const auto* op = std::get_if<std::string>(&opField->value);
    if (op == nullptr)
    {
        return std::unexpected("LAYOUT OP MUST BE A STRING");
    }

    // Dispatch to the painter that matches the layout op string.
    if (*op == "block")
    {
        return paintBlockShape(cells, *object);
    }
    if (*op == "ellipse")
    {
        return paintEllipseShape(cells, *object);
    }
    if (*op == "beam")
    {
        return paintBeamShape(cells, *object);
    }
    if (*op == "columns")
    {
        return paintColumnsShape(cells, *object);
    }
    return std::unexpected(std::format("UNKNOWN LAYOUT OP {}", *op));
}

[[nodiscard]] std::expected<void, std::string> paintDocument(Grid cells, const Json& document)
{
    // The root may only contain shapes. Each shape is painted in file order.
    const auto* root = std::get_if<Json::Object>(&document.value);
    if (root == nullptr)
    {
        return std::unexpected("LAYOUT ROOT MUST BE AN OBJECT");
    }
    if (auto keys = allowOnly(*root, {"shapes"}); !keys)
    {
        return keys;
    }
    const Json* shapes = root->find("shapes");
    if (shapes == nullptr)
    {
        return std::unexpected("LAYOUT IS MISSING SHAPES");
    }
    const auto* list = std::get_if<Json::Array>(&shapes->value);
    if (list == nullptr)
    {
        return std::unexpected("LAYOUT SHAPES MUST BE AN ARRAY");
    }
    for (const Json& shape : *list)
    {
        if (auto painted = paintShape(cells, shape); !painted)
        {
            return painted;
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::filesystem::path, std::string> layoutDirectory()
{
    // The wide string owns the module path. It grows until GetModuleFileNameW fits,
    // the same lookup the shader loader uses, so layouts sit beside the executable.
    std::wstring name(512, L'\0');

    // Grow the buffer until GetModuleFileNameW returns the full module path.
    for (;;)
    {
        const DWORD length =
            GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
        if (length == 0)
        {
            return std::unexpected("COULD NOT RESOLVE THE LAYOUT DIRECTORY");
        }
        if (length < name.size())
        {
            name.resize(length);
            return std::filesystem::path(std::move(name)).parent_path() / "layouts";
        }
        name.resize(name.size() * 2);
    }
}

[[nodiscard]] std::expected<std::string, std::string> readLayout(const std::filesystem::path& path)
{
    // binary mode keeps the bytes stable so the parser, not the stream, handles newlines.
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
    {
        return std::unexpected(std::format("COULD NOT OPEN {}", path.filename().string()));
    }

    // Size the string from the file length, then read the whole layout in one pass.
    const auto end = input.tellg();
    if (end < 0)
    {
        return std::unexpected(std::format("COULD NOT READ {}", path.filename().string()));
    }
    std::string text(static_cast<std::size_t>(end), '\0');
    input.seekg(0);
    if (end > 0 && !input.read(text.data(), end))
    {
        return std::unexpected(std::format("COULD NOT READ {}", path.filename().string()));
    }
    return text;
}

[[nodiscard]] std::expected<std::vector<Cell>, std::string> loadLayout(Scene scene)
{
    // Resolve the scene's file, parse it, then paint into a fresh world buffer.
    const auto index = std::to_underlying(scene);

    // Map the scene id to a JSON file beside the executable.
    if (index >= layoutFiles.size())
    {
        return std::unexpected("UNKNOWN SCENE");
    }
    auto directory = layoutDirectory();
    if (!directory)
    {
        return std::unexpected(directory.error());
    }
    const auto path = *directory / layoutFiles[index];
    auto text = readLayout(path);
    if (!text)
    {
        return std::unexpected(text.error());
    }
    // The parser's string_view borrows text. Parsing finishes before text is destroyed.
    auto document = JsonParser(*text).parse();
    if (!document)
    {
        return std::unexpected(std::format("{}: {}", path.filename().string(), document.error()));
    }
    // The vector owns the cells. Grid is a view of that storage and must not outlive it.
    std::vector<Cell> cells(worldCells);
    Grid grid(cells.data());
    if (auto painted = paintDocument(grid, *document); !painted)
    {
        return std::unexpected(std::format("{}: {}", path.filename().string(), painted.error()));
    }
    return cells;
}

} // namespace

std::expected<std::vector<Cell>, std::string> makeScene(Scene scene)
{
    // The layout file is read at runtime. The returned vector owns the painted cells.
    return loadLayout(scene);
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