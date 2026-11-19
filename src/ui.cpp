#include "ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <ranges>
#include <span>

namespace sand {

// Settings-menu resolution presets defined once for the UI table.
const std::array<ResolutionChoice, resolutionCount> resolutions{
    ResolutionChoice{1280, 800, "1280 X 800"},
    ResolutionChoice{1600, 900, "1600 X 900"},
    ResolutionChoice{1920, 1080, "1920 X 1080"},
    ResolutionChoice{2560, 1440, "2560 X 1440"},
};

namespace {

// Shared HUD colors. constexpr keeps each channel value a constant expression.
constexpr std::uint32_t screen = 0x0e131b;
constexpr std::uint32_t panel = 0x151e29;
constexpr std::uint32_t inset = 0x101823;
constexpr std::uint32_t border = 0x2b3c4b;
constexpr std::uint32_t muted = 0x889cad;
constexpr std::uint32_t white = 0xdce9eb;
constexpr std::uint32_t mint = 0x87e4ba;
constexpr std::uint32_t mintDark = 0x1b3836;

using Glyph = std::array<unsigned, 7>;

// Bitmap font. Each glyph is seven rows of bits, stored in a constexpr array so
// text layout can use it from any later constant context.
constexpr std::array<Glyph, 36> font{{
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
    {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {31, 4, 4, 4, 4, 4, 31},
    {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
    {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
    {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 27, 17}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
    {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},   {14, 16, 16, 30, 17, 17, 14},
    {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
}};

constexpr Glyph glyph(char c)
{
    // Compile-time lookup when the character is a constant. Runtime calls use the same body.
    if (c >= 'A' && c <= 'Z')
    {
        return font[static_cast<std::size_t>(c - 'A')];
    }
    if (c >= '0' && c <= '9')
    {
        return font[static_cast<std::size_t>(26 + c - '0')];
    }
    switch (c)
    {
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '+':
        return {0, 4, 4, 31, 4, 4, 0};
    case '.':
        return {0, 0, 0, 0, 0, 12, 12};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '>':
        return {16, 8, 4, 2, 4, 8, 16};
    case ':':
        return {0, 12, 12, 0, 12, 12, 0};
    default:
        return {};
    }
}

constexpr std::array<std::array<std::uint8_t, 8>, materialCount> icons{{
    {0, 0, 0x10, 0x28, 0x18, 0x54, 0x7e, 0xff},
    {0x10, 0x10, 0x28, 0x28, 0x44, 0x54, 0x7c, 0x38},
    {0x0c, 0x14, 0x18, 0x30, 0x68, 0x44, 0x7c, 0x38},
    {0, 0x3c, 0x46, 0x62, 0x8e, 0x82, 0xfe, 0},
    {0, 0x3c, 0x4e, 0x96, 0xaa, 0x96, 0x4e, 0x3c},
    {0x08, 0x18, 0x22, 0x36, 0x73, 0x5b, 0x7e, 0x3c},
    {0x3c, 0x62, 0x70, 0x3c, 0x0e, 0x42, 0x3c, 0},
    {0x10, 0x28, 0x20, 0x12, 0x0c, 0x29, 0x42, 0x3c},
    {0, 0x18, 0x3c, 0x24, 0x66, 0x5a, 0xdf, 0xff},
    {0x10, 0x54, 0x38, 0xfe, 0x38, 0x54, 0x10, 0},
}};

std::array<float, 4> linear(std::uint32_t rgb)
{
    // Capture-free lambda for one sRGB channel. It exists only for this conversion.
    const auto channel = [](std::uint32_t value) {
        const float s = static_cast<float>(value & 255) / 255.0F;
        return s <= 0.04045F ? s / 12.92F : std::pow((s + 0.055F) / 1.055F, 2.4F);
    };
    return {channel(rgb >> 16), channel(rgb >> 8), channel(rgb), 1.0F};
}

class Painter
{
  public:

    // explicit so a quad vector is not implicitly converted into a Painter.
    explicit Painter(std::vector<Quad>& quads) : quads_(quads)
    {
        quads_.clear();
    }

    void rect(Rect r, std::uint32_t color)
    {
        box(r, linear(color));
    }

    // Draw a one-pixel border around an inset fill.
    void frame(Rect r, std::uint32_t line, std::uint32_t fill)
    {
        rect(r, line);
        rect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, fill);
    }

    void label(float x, float y, std::string_view value, float px, std::uint32_t color)
    {
        // string_view reads the caller's characters. It does not allocate or own them.
        auto ink = linear(color);
        for (char c : value)
        {
            auto pattern = glyph(c);
            for (int row = 0; row < 7; ++row)
            {
                stripe(x, y + px * row, px, pattern[row], 5, ink);
            }
            x += px * 6;
        }
    }

    // Stamp the 8x8 material bitmap for palette rows and tooltips.
    void icon(float x, float y, std::size_t material, float px, std::uint32_t color)
    {
        auto ink = linear(color);
        for (int row = 0; row < 8; ++row)
        {
            stripe(x, y + px * row, px, icons[material][row], 8, ink);
        }
    }

  private:

    // Borrows the caller's quad buffer. The vector must outlive this Painter.
    std::vector<Quad>& quads_;

    void box(Rect r, const std::array<float, 4>& ink)
    {
        const float x0 = std::round(r.x), y0 = std::round(r.y);
        quads_.push_back({{x0, y0, std::round(r.x + r.w) - x0, std::round(r.y + r.h) - y0}, ink});
    }

    void stripe(float x, float y, float px, unsigned bits, int columns,
                const std::array<float, 4>& ink)
    {
        // Merge adjacent one bits into a single quad per glyph row.
        for (int col = 0; col < columns;)
        {
            if ((bits & (1u << (columns - 1 - col))) == 0)
            {
                ++col;
                continue;
            }
            int end = col + 1;
            while (end < columns && (bits & (1u << (columns - 1 - end))) != 0)
            {
                ++end;
            }
            box({x + px * col, y, px * (end - col), px}, ink);
            col = end;
        }
    }
};

void button(Painter& p, Rect r, Point mouse, std::string_view name, float px, bool active = false)
{
    // Frame and label reflect hover and active selection.
    bool hover = r.contains(mouse);
    p.frame(r, active ? mint : hover ? muted : border, active ? mintDark : hover ? border : inset);
    const float textWidth = px * (6.0F * static_cast<float>(name.size()) - 1.0F);
    p.label(r.x + (r.w - textWidth) / 2.0F, r.y + (r.h - 7.0F * px) / 2.0F, name, px,
            active  ? mint
            : hover ? white
                    : muted);
}

void materialTooltip(Painter& p, Point mouse, std::size_t material, float s, float px, int width,
                     int height)
{
    const auto& info = materials[material];
    const float iconScale = std::max(1.0F, std::min(2.0F, std::round(1.5F * s)));
    const float iconPx = 8.0F * iconScale;
    const float pad = 6.0F * s;
    const float textWidth = px * (6.0F * static_cast<float>(info.name.size()) - 1.0F);
    const float boxW = pad + iconPx + pad + textWidth + pad;
    const float boxH = std::max(iconPx, 7.0F * px) + 2.0F * pad;
    const float offset = 14.0F * s;
    float tx = mouse.x + offset;
    float ty = mouse.y + offset;

    // Flip the box to the left or above the cursor when it would leave the window.
    if (tx + boxW > static_cast<float>(width))
    {
        tx = mouse.x - offset - boxW;
    }
    if (ty + boxH > static_cast<float>(height))
    {
        ty = mouse.y - offset - boxH;
    }
    tx = std::max(0.0F, tx);
    ty = std::max(0.0F, ty);
    const Rect box{tx, ty, boxW, boxH};
    p.frame(box, mint, screen);
    p.icon(tx + pad, ty + (boxH - iconPx) / 2.0F, material, iconScale, info.color);
    p.label(tx + pad + iconPx + pad, ty + (boxH - 7.0F * px) / 2.0F, info.name, px, white);
}

// Procedural gear icon for the settings cog control.
void cog(Painter& p, Rect r, Point mouse)
{
    const bool hover = r.contains(mouse);
    p.frame(r, hover ? mint : border, hover ? mintDark : inset);
    const float hub = std::max(3.0F, std::round(r.w * 0.22F));
    const float tooth = std::max(2.0F, std::round(r.w * 0.14F));
    const float cx = r.x + r.w / 2.0F;
    const float cy = r.y + r.h / 2.0F;
    const std::uint32_t ink = hover ? white : mint;
    p.rect({cx - tooth / 2.0F, r.y + 4.0F, tooth, r.h - 8.0F}, ink);
    p.rect({r.x + 4.0F, cy - tooth / 2.0F, r.w - 8.0F, tooth}, ink);
    p.rect({cx - r.w * 0.22F, cy - tooth / 2.0F, r.w * 0.44F, tooth}, ink);
    p.rect({cx - tooth / 2.0F, cy - r.h * 0.22F, tooth, r.h * 0.44F}, ink);
    p.frame({cx - hub, cy - hub, hub * 2.0F, hub * 2.0F}, ink, hover ? mintDark : inset);
}

} // namespace

Layout makeLayout(int width, int height, int cellPixelSize)
{
    const auto w = static_cast<float>(width), h = static_cast<float>(height);
    Layout l{};
    l.scale = std::clamp(std::min(w / 1230.0F, h / 768.0F), 0.60F, 2.0F);
    const float s = l.scale;
    l.gap = 24 * s;
    const float rightWidth = 236 * s;
    const float top = 119 * s;
    const float worldResH = 52 * s;
    const float bottom = h - 83 * s - worldResH;
    const float availableWidth = w - rightWidth - 3 * l.gap;
    constexpr float aspect = static_cast<float>(worldWidth) / worldHeight;
    float stageWidth = std::min(availableWidth, (bottom - top) * aspect);
    float stageHeight = stageWidth / aspect;
    // When cellPixelSize is set, size the canvas from pixels-per-cell instead of only aspect fit.
    if (cellPixelSize > 0)
    {
        const float desiredW = static_cast<float>(worldWidth * cellPixelSize);
        const float desiredH = static_cast<float>(worldHeight * cellPixelSize);
        const float fit = std::min({availableWidth / desiredW, (bottom - top) / desiredH, 1.0F});
        stageWidth = desiredW * fit;
        stageHeight = desiredH * fit;
    }
    const float horizontalSpace = w - (stageWidth + l.gap + rightWidth);
    const float y = std::round(top + (bottom - top - stageHeight) / 2);
    const float x = std::round(horizontalSpace / 2);
    l.canvas = {x, y, std::round(stageWidth), std::round(stageHeight)};
    l.panel = {l.canvas.x + l.canvas.w + l.gap, y, rightWidth, l.canvas.h};
    l.footer = {x, l.canvas.y + l.canvas.h + 15 * s, l.canvas.w + l.gap + rightWidth, 41 * s};

    // Divide the palette column evenly for one swatch row per material.
    const float rowStep = (l.panel.h - 76 * s) / static_cast<float>(materialCount);
    for (std::size_t i = 0; i < l.swatches.size(); ++i)
    {
        l.swatches[i] = {l.panel.x + 11 * s, l.panel.y + 41 * s + rowStep * i, rightWidth - 22 * s,
                         rowStep - 3 * s};
    }
    // Place scene tabs along the top edge of the canvas.
    constexpr std::array tabWidths{144, 155, 121, 91};
    float tabX = x;
    for (std::size_t i = 0; i < l.scenes.size(); ++i)
    {
        l.scenes[i] = {tabX, y - 38 * s, tabWidths[i] * s, 27 * s};
        tabX += (tabWidths[i] + 7) * s;
    }

    const float f = l.footer.y + 3 * s;
    const float row = l.footer.h - 6 * s;
    const float end = l.footer.x + l.footer.w;
    l.controls = {
        Button{{x, f, 81 * s, row}, Action::play},
        Button{{x + 87 * s, f, 68 * s, row}, Action::step},
        Button{{x + 161 * s, f, 77 * s, row}, Action::reset},
        Button{{end - 149 * s, f, 70 * s, row}, Action::save},
        Button{{end - 73 * s, f, 73 * s, row}, Action::load},
        Button{{x + 289 * s, f, 125 * s, row}, Action::brush},
        Button{{x + 264 * s, f, 23 * s, row}, Action::sizeDown},
        Button{{x + 417 * s, f, 23 * s, row}, Action::sizeUp},
        Button{{x + 451 * s, f, 105 * s, row}, Action::cycleTool},
    };
    l.brushTrack = {x + 294 * s, f + row * 0.68F, 115 * s, 3 * s};

    const float barY = h - worldResH - 6 * s;
    l.worldResBar = {8 * s, barY, w - 16 * s, worldResH};
    const float barPad = 10 * s;
    const float trackW = l.worldResBar.w * 0.42F;
    l.worldResTrack = {l.worldResBar.x + 280 * s, l.worldResBar.y + barPad + 18 * s, trackW, 5 * s};
    const float btnY = l.worldResBar.y + barPad;
    const float btnW = 118 * s;
    const float btnH = l.worldResBar.h - 2 * barPad;
    l.worldResApply = Button{{l.worldResBar.x + l.worldResBar.w - 2 * btnW - 12 * s, btnY, btnW, btnH},
                             Action::worldResApply};
    l.worldResCancel = Button{{l.worldResBar.x + l.worldResBar.w - btnW - 4 * s, btnY, btnW, btnH},
                              Action::worldResCancel};

    const float cogSize = std::max(22.0F, std::round(26 * s));
    const float margin = std::max(10.0F, std::round(12 * s));
    l.cog = {w - margin - cogSize, margin, cogSize, cogSize};
    l.width = width;
    l.height = height;

    const float menuW = std::min(340.0F * s, w - 2 * margin);
    const float buttonH = 28 * s;
    const float menuGap = 6 * s;
    const float pad = 14 * s;
    const float title = 24 * s;
    // Center a modal card and stack its buttons with uniform spacing.
    const auto place = [&](Rect& card, std::span<Button> buttons) {
        const float rows = static_cast<float>(buttons.size());
        const float cardH = pad + title + rows * buttonH + (rows - 1) * menuGap + pad;
        const float left = std::round((w - menuW) / 2.0F);
        const float top = std::round((h - cardH) / 2.0F);
        card = {left, top, menuW, cardH};
        float by = top + pad + title;
        for (Button& b : buttons)
        {
            b.bounds = {left + pad, by, menuW - 2 * pad, buttonH};
            by += buttonH + menuGap;
        }
    };
    l.pauseButtons = {
        Button{{}, Action::resume},
        Button{{}, Action::openSettings},
        Button{{}, Action::exit},
    };
    place(l.pausePanel, l.pauseButtons);
    for (std::size_t i = 0; i < resolutionCount; ++i)
    {
        l.settingsButtons[i] = Button{{}, Action::resolution, static_cast<int>(i)};
    }
    l.settingsButtons[resolutionCount] = Button{{}, Action::back};
    l.settingsButtons[resolutionCount + 1] = Button{{}, Action::exit};
    place(l.settingsPanel, l.settingsButtons);
    return l;
}

std::optional<Button> Layout::hit(Point mouse) const
{
    // Material rows beat scene tabs and footer controls.
    for (std::size_t i = 0; i < swatches.size(); ++i)
    {
        if (swatches[i].contains(mouse))
        {
            return Button{swatches[i], Action::choose, static_cast<int>(i)};
        }
    }

    // Scene tabs are tested before the footer button row.
    for (std::size_t i = 0; i < scenes.size(); ++i)
    {
        if (scenes[i].contains(mouse))
        {
            return Button{scenes[i], Action::scene, static_cast<int>(i)};
        }
    }

    // find_if walks the controls with a lambda that borrows mouse by copy of the Point.

    // The lambda does not outlive this statement. optional is empty when nothing matched.
    auto found = std::ranges::find_if(
        controls, [mouse](const Button& b) { return b.bounds.contains(mouse); });
    if (found != controls.end())
    {
        return *found;
    }
    if (worldResApply.bounds.contains(mouse))
    {
        return worldResApply;
    }
    if (worldResCancel.bounds.contains(mouse))
    {
        return worldResCancel;
    }
    if (worldResTrack.contains(mouse))
    {
        return Button{worldResTrack, Action::worldCell};
    }
    return std::nullopt;
}

Point Layout::worldPoint(Point pixel) const
{
    // Map client pixels through the canvas rect into continuous grid coordinates.
    return {(pixel.x - canvas.x) * worldWidth / canvas.w,
            (pixel.y - canvas.y) * worldHeight / canvas.h};
}

void composeUi(std::vector<Quad>& quads, const Layout& l, const UiState& state, Point mouse,
               int width, int height, std::optional<std::size_t> canvasMaterial)
{
    Painter p(quads);
    const float s = l.scale;
    const float px = std::max(1.0F, std::floor(2.0F * s));
    const float tiny = s >= 1.65F ? 2.0F : 1.0F;
    const float x = l.canvas.x, y = l.canvas.y;
    const float end = l.panel.x + l.panel.w;
    const float headerY = std::max(13 * s, y - 102 * s);
    constexpr std::uint32_t tile = 0x223b3c;

    // Keep the canvas available to the fluid pass.
    p.rect({0, 0, static_cast<float>(width), y}, screen);
    p.rect({0, y, x, l.canvas.h}, screen);
    p.rect({x + l.canvas.w, y, width - x - l.canvas.w, l.canvas.h}, screen);
    p.rect({0, y + l.canvas.h, static_cast<float>(width), height - y - l.canvas.h}, screen);

    p.frame({x, headerY, 46 * s, 46 * s}, 0x416b62, tile);
    p.icon(x + 11 * s, headerY + 11 * s, 1, 3 * s, mint);
    p.label(x + 59 * s, headerY + 3 * s, "FALLING SAND", std::max(2.0F, std::round(3 * s)), white);
    p.label(x + 60 * s, headerY + 33 * s, "MATERIAL SANDBOX", tiny, muted);

    // format fills the badge from the frame rate and the GPU time, and returns a string.
    const auto metric = std::format("{:3} FPS   {:.2f} MS GPU", state.fps, state.gpuMs);
    const float badgeWidth = (static_cast<float>(metric.size()) * 6 * px + 31 * s);
    const Rect badge{end - badgeWidth, headerY + 8 * s, badgeWidth, 28 * s};
    p.frame(badge, border, inset);
    p.rect({badge.x + 9 * s, badge.y + 11 * s, 5 * s, 5 * s}, state.paused ? 0xe4b17d : mint);
    p.label(badge.x + 21 * s, badge.y + (28 * s - 7 * px) / 2, metric, px, white);

    // Highlight the active scene tab; others respond to hover only.
    for (std::size_t i = 0; i < l.scenes.size(); ++i)
    {
        if (static_cast<Scene>(i) == state.scene)
        {
            button(p, l.scenes[i], mouse, sceneNames[i], px, true);
        } else
        {
            button(p, l.scenes[i], mouse, sceneNames[i], px);
        }
    }

    // One-pixel frame around the simulation viewport.
    p.rect({x - 1, y - 1, l.canvas.w + 2, 1}, border);
    p.rect({x - 1, y + l.canvas.h, l.canvas.w + 2, 1}, border);
    p.rect({x - 1, y, 1, l.canvas.h}, border);
    p.rect({x + l.canvas.w, y, 1, l.canvas.h}, border);

    p.frame(l.panel, border, panel);
    p.label(l.panel.x + 13 * s, y + 13 * s, "MATERIALS", px, white);
    p.label(end - 31 * s, y + 15 * s, "10", tiny, mint);
    p.rect({l.panel.x + 11 * s, y + 33 * s, l.panel.w - 22 * s, 1}, border);
    // Draw each material row with icon, name, behavior hint, and hotkey cap.
    for (std::size_t i = 0; i < l.swatches.size(); ++i)
    {
        const auto& info = materials[i];
        const Rect r = l.swatches[i];
        const bool active = state.material == info.id;
        const bool hover = r.contains(mouse);
        p.frame(r,
                active  ? mint
                : hover ? muted
                        : border,
                active  ? mintDark
                : hover ? border
                        : inset);
        const float iconScale = std::max(1.0F, std::min(3.0F, std::round(2 * s)));
        p.icon(r.x + 9 * s, r.y + (r.h - 8 * iconScale) / 2, i, iconScale, info.color);
        p.label(r.x + 35 * s, r.y + 5 * s, info.name, px, active ? white : info.color);
        if (r.h >= px * 7 + tiny * 7 + 9 * s)
        {
            p.label(r.x + 35 * s, r.y + r.h - 7 * tiny - 4 * s, info.behavior, tiny, muted);
        }

        // The key character lives for this iteration. The string_view passed below
        // borrows that one character only for the label call.
        const char key = i == 9 ? '0' : static_cast<char>('1' + i);
        const Rect keycap{r.x + r.w - 24 * s, r.y + (r.h - 17 * s) / 2, 16 * s, 17 * s};
        p.frame(keycap, active ? 0x568577 : border, inset);
        p.label(keycap.x + (keycap.w - 5 * tiny) / 2, keycap.y + (keycap.h - 7 * tiny) / 2,
                std::string_view(&key, 1), tiny, active ? mint : muted);
    }
    p.label(l.panel.x + 13 * s, y + l.panel.h - 20 * s, "1-0  SELECT MATERIAL", tiny, muted);

    // CTAD picks the array extent from this initializer list.
    const std::array captions{state.paused ? "PLAY" : "PAUSE",
                              "STEP",
                              "RESET",
                              "SAVE",
                              "LOAD",
                              "",
                              "-",
                              "+",
                              state.tool == Tool::paint   ? "PAINT"
                              : state.tool == Tool::erase ? "ERASE"
                                                          : "STIR"};
    for (std::size_t i = 0; i < l.controls.size(); ++i)
    {
        button(p, l.controls[i].bounds, mouse, captions[i], px, i == 0 || i == 8);
    }
    const auto brush = l.controls[5].bounds;
    p.label(brush.x + 7 * s, brush.y + 6 * s, std::format("SIZE {:02}", state.radius), tiny, white);
    p.rect(l.brushTrack, border);

    // Fill the brush track through the current radius.
    auto fill = l.brushTrack;
    fill.w *= static_cast<float>(state.radius - minRadius) / (maxRadius - minRadius);
    p.rect(fill, mint);
    p.rect({fill.x + fill.w - 1, fill.y - 2 * s, 3 * s, fill.h + 4 * s}, mint);

    constexpr std::uint32_t worldResFill = 0x7a1b6e;
    constexpr std::uint32_t worldResEdge = 0xff2ef0;
    constexpr std::uint32_t worldResInk = 0xfff44a;
    const int cellPx =
        state.worldResConfirm ? state.pendingWorldCellPx : state.worldCellPx;
    const Rect& resBar = l.worldResBar;
    p.frame(resBar, worldResEdge, worldResFill);
    p.label(resBar.x + 12 * s, resBar.y + 8 * s, ">>> BUILD REFRESH — WORLD PX / SAND CELL <<<",
            std::max(2.0F, px), worldResInk);
    p.label(resBar.x + 12 * s, resBar.y + 28 * s,
            std::format("GRID {}x{}   VALUE {:d} PX/CELL", worldWidth, worldHeight, cellPx),
            tiny, white);
    p.rect(l.worldResTrack, 0x2e0a28);
    auto resFill = l.worldResTrack;
    resFill.w *= static_cast<float>(cellPx - minWorldCellPx) / (maxWorldCellPx - minWorldCellPx);
    p.rect(resFill, worldResEdge);
    p.rect({resFill.x + resFill.w - 1, resFill.y - 3 * s, 4 * s, resFill.h + 6 * s}, white);
    if (state.worldResConfirm)
    {
        button(p, l.worldResApply.bounds, mouse, "APPLY + RESET", tiny,
               l.worldResApply.bounds.contains(mouse));
        button(p, l.worldResCancel.bounds, mouse, "CANCEL", tiny,
               l.worldResCancel.bounds.contains(mouse));
    } else
    {
        button(p, l.worldResApply.bounds, mouse, "APPLY + RESET", tiny, false);
        button(p, l.worldResCancel.bounds, mouse, "CANCEL", tiny, false);
    }

    const float foot = l.footer.y + l.footer.h + 8 * s;
    p.label(x, foot, "SPACE PLAY/PAUSE   N STEP   RMB ERASE   MMB STIR   WHEEL SIZE   ESC MENU",
            tiny, muted);
    if (!state.status.empty())
    {
        const float textWidth = static_cast<float>(state.status.size()) * 6 * tiny - tiny;
        p.label(end - textWidth, foot, state.status, tiny, mint);
    }

    cog(p, l.cog, mouse);

    // Cursor-following material label: palette row wins over canvas probe.
    std::optional<std::size_t> tooltipMaterial;
    if (state.menu == Menu::none && mouse.x >= 0.0F)
    {
        for (std::size_t i = 0; i < l.swatches.size(); ++i)
        {
            if (l.swatches[i].contains(mouse))
            {
                tooltipMaterial = i;
                break;
            }
        }
        if (!tooltipMaterial)
        {
            tooltipMaterial = canvasMaterial;
        }
        if (tooltipMaterial)
        {
            materialTooltip(p, mouse, *tooltipMaterial, s, px, width, height);
        }
    }
    // Skip pause and settings cards while the sandbox owns the frame.
    if (state.menu == Menu::none)
    {
        return;
    }

    // Draw the centered pause or settings card on top of the HUD.
    const bool settings = state.menu == Menu::settings;
    const Rect card = settings ? l.settingsPanel : l.pausePanel;
    p.frame(card, border, screen);
    p.label(card.x + 14 * s, card.y + 12 * s, settings ? "SETTINGS" : "MENU", px, white);
    if (!settings)
    {
        constexpr std::array names{"RESUME", "SETTINGS", "EXIT"};
        for (std::size_t i = 0; i < l.pauseButtons.size(); ++i)
        {
            button(p, l.pauseButtons[i].bounds, mouse, names[i], px);
        }
        return;
    }
    for (std::size_t i = 0; i < resolutionCount; ++i)
    {
        const bool active = width == resolutions[i].width && height == resolutions[i].height;
        button(p, l.settingsButtons[i].bounds, mouse, resolutions[i].label, px, active);
    }
    button(p, l.settingsButtons[resolutionCount].bounds, mouse, "BACK", px);
    button(p, l.settingsButtons[resolutionCount + 1].bounds, mouse, "EXIT", px);
}

} // namespace sand
