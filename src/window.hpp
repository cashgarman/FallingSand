#pragma once

#include "game.hpp"

#include <array>
#include <windows.h>

// Win32 window owner.
//
// The class acquires an HWND and releases it in the destructor. Copying is deleted
// because two objects would destroy the same window. Declaring the destructor
// suppresses the implicit move operations, so a Window stays where it was created.
// handle() and the other observers are const because they do not change that ownership.
// nodiscard flags results the caller needs. The constructor is explicit so a bool
// cannot become a Window through an implicit conversion.

namespace sand {

/// <summary>Keyboard, mouse, and wheel state for one frame.</summary>
struct Input
{
    Point mouse{-1, -1};                                // Cursor in client pixels; (-1, -1) when outside.
    std::array<bool, 256> pressed{};                    // Edge-triggered key down events for this frame.
    int wheel{};                                        // Mouse wheel delta for this frame.
    bool paint{}, erase{}, stir{};                      // Buttons currently held.
    Point paintDown{};                                  // Client position where the primary button went down.
    bool paintPressed{}, erasePressed{}, stirPressed{}; // Edge-triggered button downs.
};

/// <summary>Owns the Win32 HWND and translates messages into Input.</summary>
class Window
{
  public:
    /// <summary>Creates the sandbox window.</summary>
    /// <param name="show">When true, the window is shown on creation.</param>
    explicit Window(bool show);
    ~Window();

    // Copy is deleted. A second owner would destroy the same HWND.
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// <summary>Returns the underlying window handle.</summary>
    /// <returns>HWND owned by this object.</returns>
    [[nodiscard]] HWND handle() const;

    /// <summary>Returns the current client area size.</summary>
    /// <returns>Width and height in pixels.</returns>
    [[nodiscard]] std::array<int, 2> size() const;

    /// <summary>Pumps pending Win32 messages and updates running state.</summary>
    /// <returns>False after WM_QUIT or destroy.</returns>
    [[nodiscard]] bool poll();

    /// <summary>Returns the latest input and clears one-shot fields.</summary>
    /// <returns>Input snapshot for the current frame.</returns>
    [[nodiscard]] Input consumeInput();

    /// <summary>Blocks until a message arrives when the event queue is empty.</summary>
    void wait() const;

    /// <summary>Requests a client-area resize.</summary>
    /// <param name="width">New client width in pixels.</param>
    /// <param name="height">New client height in pixels.</param>
    void clientSize(int width, int height);

  private:
    static LRESULT CALLBACK route(HWND handle, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT process(UINT message, WPARAM wparam, LPARAM lparam);

    HWND handle_{};
    Input input_{};
    int wheelRemainder_{};
    bool running_{true};
};

} // namespace sand