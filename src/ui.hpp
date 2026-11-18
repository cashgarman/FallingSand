#pragma once

#include "game.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Immediate-mode layout for the sandbox HUD.
//
// Action and Tool are enum classes, so the closed set of commands stays scoped and
// the compositor can switch on every value. optional is the result of a hit test:
// either a button or an empty miss, with no error to explain. string_view status
// borrows text owned by the caller for this frame. array sizes follow the material
// and scene tables, so a new entry forces the layout arrays to match.
// Quad is alignas(16) and 32 bytes so the GPU record matches the shader stride.
// inline constexpr limits are header values with one identity across translation units.

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

// Size and alignment are separate representation checks. The shader reads 32-byte quads.
static_assert(sizeof(Quad) == 32);

inline constexpr std::size_t maxQuads = 16384;
inline constexpr int minRadius = 2;
inline constexpr int maxRadius = 32;

[[nodiscard]] Layout makeLayout(int width, int height);
void composeUi(std::vector<Quad>& quads, const Layout& layout, const UiState& state, Point mouse,
               int width, int height);

} // namespace sand
