#pragma once

#include "game.hpp"

#include <array>
#include <windows.h>

namespace sand {

struct Input
{
    Point mouse{-1, -1};
    std::array<bool, 256> pressed{};
    int wheel{};
    bool paint{}, erase{}, stir{};
    Point paintDown{};
    bool paintPressed{}, erasePressed{}, stirPressed{};
};

class Window
{
  public:
    explicit Window(bool show);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] HWND handle() const;
    [[nodiscard]] std::array<int, 2> size() const;
    [[nodiscard]] bool poll();
    [[nodiscard]] Input consumeInput();
    void wait() const;

  private:
    static LRESULT CALLBACK route(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT process(UINT message, WPARAM wparam, LPARAM lparam);

    HWND handle_{};
    Input input_{};
    int wheelRemainder_{};
    bool running_{true};
};

} // namespace sand
