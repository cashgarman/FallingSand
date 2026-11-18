#pragma once

#include "game.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sand {

enum class Action : std::uint8_t
{
    choose,
    scene,
    play,
    step,
    reset,
    save,
    load,
    brush,
    sizeDown,
    sizeUp,
    cycleTool
};

enum class Tool : std::uint8_t
{
    paint,
    erase,
    stir
};

struct Button
{
    Rect bounds;
    Action action;
    int argument{};
};

struct Layout
{
    Rect canvas;
    Rect panel;
    Rect footer;
    Rect brushTrack;
    float gap, scale;
    std::array<Rect, materials.size()> swatches;
    std::array<Rect, sceneNames.size()> scenes;
    std::array<Button, 9> controls;
    [[nodiscard]] std::optional<Button> hit(Point mouse) const;
    [[nodiscard]] Point worldPoint(Point pixel) const;
};

struct UiState
{
    Scene scene{};
    Material material{Material::water};
    Tool tool{};
    bool paused{};
    int radius{8};
    int fps{};
    float gpuMs{};
    std::string_view status;
};

struct alignas(16) Quad
{
    std::array<float, 4> bounds;
    std::array<float, 4> color;
};
static_assert(sizeof(Quad) == 32);

inline constexpr std::size_t maxQuads = 16384;
inline constexpr int minRadius = 2;
inline constexpr int maxRadius = 32;

[[nodiscard]] Layout makeLayout(int width, int height);
void composeUi(std::vector<Quad>& quads, const Layout& layout, const UiState& state, Point mouse,
               int width, int height);

} // namespace sand
