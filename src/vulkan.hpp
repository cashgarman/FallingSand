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

namespace sand {

struct DrawFrame
{
    Rect canvas;
    Point cursor;
    float radius;
    bool cursorVisible;
    float visualTime;
    std::optional<Stroke> stroke;
    unsigned ticks;
    std::span<const Quad> ui;
    std::filesystem::path capture;
};

struct Timing
{
    double simulationMs{};
    double gpuMs{};
};

struct FlowSnapshot
{
    std::vector<Point> predictor;
    std::vector<Point> projected;
};

class VulkanGame
{
  public:
    VulkanGame(HWND window, bool benchmark, bool validate);
    ~VulkanGame();
    VulkanGame(const VulkanGame&) = delete;
    VulkanGame& operator=(const VulkanGame&) = delete;

    [[nodiscard]] std::array<int, 2> prepare(int width, int height);
    [[nodiscard]] bool draw(const DrawFrame& frame);
    void replaceWorld(std::span<const Cell> cells);
    [[nodiscard]] std::vector<Cell> snapshot();
    [[nodiscard]] FlowSnapshot flowSnapshot();
    void simulate(unsigned ticks);
    void paint(const Stroke& stroke);
    void wait();
    [[nodiscard]] Timing timing() const;
    [[nodiscard]] std::string_view deviceName() const;
    [[nodiscard]] unsigned validationErrors() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sand
