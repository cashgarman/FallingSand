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

/// <summary>UI command emitted when the user activates a control.</summary>
enum class Action : std::uint8_t
{
    choose,       // Pick a material from the palette.
    scene,        // Switch the active scene preset.
    play,         // Toggle pause.
    step,         // Advance one simulation tick while paused.
    reset,        // Reload the current scene.
    save,         // Save the world to disk.
    load,         // Load a saved world.
    brush,        // Drag on the brush size track.
    sizeDown,     // Decrease brush radius.
    sizeUp,       // Increase brush radius.
    cycleTool,    // Cycle paint, erase, and stir.
    openSettings, // Open the settings menu from pause or the cog.
    resume,       // Close the pause menu.
    back,         // Return from settings to pause.
    exit,           // Quit the application.
    resolution,     // Apply a window resolution choice.
    worldCell,      // Drag on the world cell-size track.
    worldResApply,  // Apply pending cell size and reset the world.
    worldResCancel  // Discard pending cell size changes.
};

/// <summary>Active modal overlay on top of the sandbox HUD.</summary>
enum class Menu : std::uint8_t
{
    none,     // No overlay; sandbox receives input.
    pause,    // Pause menu with resume, settings, and exit.
    settings  // Resolution and navigation controls.
};

/// <summary>One entry in the settings resolution list.</summary>
struct ResolutionChoice
{
    int width;              // Client width in pixels.
    int height;             // Client height in pixels.
    std::string_view label; // Button label shown in the settings menu.
};

// Number of selectable window resolutions.
constexpr std::size_t resolutionCount = 4;

// Resolution presets for the settings menu.
extern const std::array<ResolutionChoice, resolutionCount> resolutions;

/// <summary>Active painting interaction mode.</summary>
enum class Tool : std::uint8_t
{
    paint, // Deposit the selected material.
    erase, // Clear cells to empty.
    stir   // Mix existing cell contents.
};

/// <summary>Hit-testable control with an associated Action.</summary>
struct Button
{
    Rect bounds;      // Screen-space click target.
    Action action;    // Command to dispatch when activated.
    int argument{};   // Index or option passed with choose, scene, and resolution actions.
};

/// <summary>Screen rectangles and controls for one frame of HUD layout.</summary>
struct Layout
{
    Rect canvas;                                               // Simulation viewport in client pixels.
    Rect panel;                                                // Material palette column.
    Rect footer;                                               // Transport and tool controls.
    Rect brushTrack;                                           // Brush size slider track.
    float gap, scale;                                          // Spacing and UI scale factor for the current window size.
    std::array<Rect, materialCount> swatches;                  // Material row hit targets.
    std::array<Rect, sceneCount> scenes;                       // Scene tab hit targets.
    std::array<Button, 9> controls;                            // Footer buttons in display order.
    Rect cog;                                                  // Settings cog hit target.
    Rect pausePanel;                                           // Pause menu card bounds.
    Rect settingsPanel;                                        // Settings menu card bounds.
    std::array<Button, 3> pauseButtons;                        // Pause menu buttons.
    std::array<Button, resolutionCount + 2> settingsButtons;   // Resolution rows plus back and exit.
    Rect worldResBar;                                          // High-visibility world cell-size strip.
    Rect worldResTrack;                                        // Slider track for pixels per cell.
    Button worldResApply;                                      // Confirm apply and world reset.
    Button worldResCancel;                                     // Reject pending cell size.
    int width{};                                               // Client width used to build this layout.
    int height{};                                              // Client height used to build this layout.

    /// <summary>Finds the topmost control under the cursor.</summary>
    /// <param name="mouse">Cursor position in client pixels.</param>
    /// <returns>The activated button, or empty when nothing matched.</returns>
    [[nodiscard]] std::optional<Button> hit(Point mouse) const;

    /// <summary>Maps a canvas pixel position to world grid coordinates.</summary>
    /// <param name="pixel">Point in client pixels relative to the window origin.</param>
    /// <returns>Continuous grid coordinates before flooring for cell indices.</returns>
    [[nodiscard]] Point worldPoint(Point pixel) const;
};

/// <summary>Mutable HUD and simulation UI state for one frame.</summary>
struct UiState
{
    Scene scene{};                      // Active scene preset.
    Material material{Material::water}; // Material selected for painting.
    Tool tool{};                        // Paint, erase, or stir.
    bool paused{};                      // Whether the simulation is paused.
    int radius{8};                      // Brush radius in grid cells.
    int fps{};                          // Smoothed frames per second for the badge.
    float gpuMs{};                      // Last measured GPU frame time in milliseconds.
    std::string_view status;            // Short status line; caller owns the text for this frame.
    Menu menu{};                        // Active modal menu, if any.
    int worldCellPx{3};                 // Applied screen pixels per simulation cell.
    int pendingWorldCellPx{3};          // Slider value awaiting confirmation.
    bool worldResConfirm{};             // Whether apply/cancel are active for a pending change.
};

/// <summary>One axis-aligned colored rectangle for the instanced UI draw pass.</summary>
struct alignas(16) Quad
{
    std::array<float, 4> bounds; // x, y, width, height in client pixels.
    std::array<float, 4> color;  // Linear RGBA color for the fragment shader.
};

// Size and alignment are separate representation checks. The shader reads 32-byte quads.
static_assert(sizeof(Quad) == 32);

// Maximum UI quads uploaded per frame.
inline constexpr std::size_t maxQuads = 16384;

// Minimum brush radius in grid cells.
inline constexpr int minRadius = 2;

// Maximum brush radius in grid cells.
inline constexpr int maxRadius = 32;

// Minimum screen pixels per simulation cell for the world-resolution slider.
inline constexpr int minWorldCellPx = 1;

// Maximum screen pixels per simulation cell for the world-resolution slider.
inline constexpr int maxWorldCellPx = 8;

/// <summary>Computes layout rectangles for the given client size.</summary>
/// <param name="width">Client width in pixels.</param>
/// <param name="height">Client height in pixels.</param>
/// <param name="cellPixelSize">Screen pixels per grid cell; zero preserves aspect-fit sizing.</param>
/// <returns>A layout with canvas, panel, and control bounds.</returns>
[[nodiscard]] Layout makeLayout(int width, int height, int cellPixelSize = 0);

/// <summary>Builds UI quads for one frame and appends them to the output buffer.</summary>
/// <param name="quads">Output buffer cleared then filled with draw records.</param>
/// <param name="layout">Precomputed control geometry.</param>
/// <param name="state">Current HUD state for labels and highlights.</param>
/// <param name="mouse">Cursor position for hover styling and tooltips.</param>
/// <param name="width">Client width in pixels.</param>
/// <param name="height">Client height in pixels.</param>
/// <param name="canvasMaterial">Optional palette index for canvas hover tooltip.</param>
void composeUi(std::vector<Quad>& quads, const Layout& layout, const UiState& state, Point mouse,
               int width, int height, std::optional<std::size_t> canvasMaterial = std::nullopt);

} // namespace sand