#pragma once

#include "game.hpp"
#include "ui.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <windows.h>

// Public GPU facade. Vulkan types stay in vulkan.cpp.
//
// Pimpl keeps the public class in this header and the device, swapchain, and
// pipelines behind unique_ptr<Impl>. unique_ptr is the single owner of that
// implementation. Copy is deleted so two facades cannot destroy one device.
// Declaring the destructor also suppresses implicit moves, so the facade stays
// where it was created.
// The destructor is declared here and defined in the source file, where Impl is
// a complete type, because unique_ptr deletes its pointer from the destructor.
// span and optional on DrawFrame borrow UI quads and a brush stroke for one frame.
// deviceName() returns a string_view into the device properties owned by Impl.

namespace sand {

/// <summary>Parameters for one presented frame of simulation and compositing.</summary>
struct DrawFrame
{
    Rect canvas;                   // UI viewport used to letterbox the world.
    Point cursor;                  // Brush position in world grid coordinates.
    float radius;                  // Brush radius in grid cells.
    bool cursorVisible;            // Whether the brush ring is drawn.
    float visualTime;              // Accumulated time for animated rendering.
    std::optional<Stroke> stroke;  // Optional paint or stir stroke for this frame.
    unsigned ticks;                  // Simulation steps to run before draw.
    std::span<const Quad> ui;      // UI quads composited over the world.
    std::filesystem::path capture; // When non-empty, write this image path after present.
};

/// <summary>GPU timing from the most recently completed measured frame.</summary>
struct Timing
{
    double simulationMs{}; // Compute time for simulation kernels.
    double gpuMs{};        // Total GPU time for the frame.
};

/// <summary>Advection predictor and projected flow fields copied from the GPU.</summary>
struct FlowSnapshot
{
    std::vector<Point> predictor; // Per-cell predictor flow vectors.
    std::vector<Point> projected; // Per-cell projected flow vectors.
};

/// <summary>Owns Vulkan device, swapchain, and compute pipelines for the sandbox.</summary>
class VulkanGame
{
  public:
    /// <summary>Creates a Vulkan device and swapchain for a window.</summary>
    /// <param name="window">HWND that receives presentation.</param>
    /// <param name="benchmark">When true, prefers paths tuned for benchmarking.</param>
    /// <param name="validate">When true, enables validation layers.</param>
    VulkanGame(HWND window, bool benchmark, bool validate);
    ~VulkanGame();

    // Copy is deleted. unique_ptr already forbids sharing the implementation.
    VulkanGame(const VulkanGame&) = delete;
    VulkanGame& operator=(const VulkanGame&) = delete;

    /// <summary>Resizes the swapchain when the client area changes.</summary>
    /// <param name="width">Requested client width in pixels.</param>
    /// <param name="height">Requested client height in pixels.</param>
    /// <returns>Actual drawable width and height after swapchain recreation.</returns>
    [[nodiscard]] std::array<int, 2> prepare(int width, int height);

    /// <summary>Runs simulation ticks and presents one frame.</summary>
    /// <param name="frame">Draw parameters and UI quads for the frame.</param>
    /// <returns>False when the swapchain is out of date and the frame was skipped.</returns>
    [[nodiscard]] bool draw(const DrawFrame& frame);

    /// <summary>Replaces the GPU world buffer with a full grid.</summary>
    /// <param name="cells">Full world grid of length worldCells.</param>
    void replaceWorld(std::span<const Cell> cells);

    /// <summary>Copies the full world grid from the GPU to host memory.</summary>
    /// <returns>Current cells from the active simulation buffer.</returns>
    [[nodiscard]] std::vector<Cell> snapshot();

    /// <summary>Reads one cell from the live world grid.</summary>
    /// <param name="x">Grid column in [0, worldWidth).</param>
    /// <param name="y">Grid row in [0, worldHeight).</param>
    /// <returns>Cell at (x, y) after GPU synchronization.</returns>
    [[nodiscard]] Cell probeCell(int x, int y);

    /// <summary>Copies predictor and projected flow buffers from the GPU.</summary>
    /// <returns>Per-cell flow samples for diagnostics and tests.</returns>
    [[nodiscard]] FlowSnapshot flowSnapshot();

    /// <summary>Advances the simulation without presenting a frame.</summary>
    /// <param name="ticks">Number of fixed-timestep steps to run.</param>
    void simulate(unsigned ticks);

    /// <summary>Applies a brush stroke on the GPU world buffer.</summary>
    /// <param name="stroke">Paint or stir parameters in grid coordinates.</param>
    void paint(const Stroke& stroke);

    /// <summary>Blocks until all submitted GPU work has finished.</summary>
    void wait();

    /// <summary>Returns timing from the last completed measured frame.</summary>
    /// <returns>Simulation and total GPU milliseconds.</returns>
    [[nodiscard]] Timing timing() const;

    /// <summary>Returns the name of the selected physical device.</summary>
    /// <returns>View into device properties owned by the implementation.</returns>
    [[nodiscard]] std::string_view deviceName() const;

    /// <summary>Counts validation layer messages recorded since creation.</summary>
    /// <returns>Number of validation errors observed.</returns>
    [[nodiscard]] unsigned validationErrors() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sand