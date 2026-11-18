#include "window.hpp"

#include <stdexcept>
#include <windowsx.h>

namespace sand {
namespace {

// Fixed registration values. constexpr keeps them available as constant expressions.
constexpr wchar_t className[] = L"FallingSandVulkan";
constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW;

} // namespace

Window::Window(bool show)
{
    // Per-monitor DPI so client pixels match the swapchain extent.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Value-initialize the window class so unused Win32 fields stay zero.
    WNDCLASSW type{};
    type.lpfnWndProc = route;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = className;
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassW(&type))
    {
        // Failure here is not a value the caller can recover. An exception leaves
        // no Window object, so the destructor does not run for this attempt.
        throw std::runtime_error("Could not register the Falling Sand window.");
    }

    RECT bounds{0, 0, 1280, 800};
    AdjustWindowRectExForDpi(&bounds, windowStyle, FALSE, 0, GetDpiForSystem());
    handle_ = CreateWindowExW(0, className, L"Falling Sand", windowStyle, CW_USEDEFAULT,
                              CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                              nullptr, nullptr, instance, this);
    if (!handle_)
    {
        UnregisterClassW(className, instance);
        throw std::runtime_error("Could not create the Falling Sand window.");
    }

    // Headless benchmark runs keep the HWND hidden.
    if (show)
    {
        ShowWindow(handle_, SW_SHOW);
    }
}

Window::~Window()
{
    // Release the HWND this object acquired, then the window class.
    if (IsWindow(handle_))
    {
        DestroyWindow(handle_);
    }
    UnregisterClassW(className, GetModuleHandleW(nullptr));
}

HWND Window::handle() const
{
    return handle_;
}

std::array<int, 2> Window::size() const
{
    // The array is the width and height. A minimized window reports an empty size.
    RECT bounds{};
    GetClientRect(handle_, &bounds);
    if (IsIconic(handle_))
    {
        return {};
    }
    return {static_cast<int>(bounds.right), static_cast<int>(bounds.bottom)};
}

bool Window::poll()
{
    // Drain the thread queue. WM_QUIT ends the frame loop.
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return running_;
}

Input Window::consumeInput()
{
    // Copy the latched edges, then clear them. The caller owns the copy.
    Input result = input_;
    input_.pressed.fill(false);
    input_.wheel = 0;
    input_.paintPressed = input_.erasePressed = input_.stirPressed = false;
    return result;
}

void Window::wait() const
{
    WaitMessage();
}

void Window::clientSize(int width, int height)
{
    // Expand to window rect so the client area matches the requested size.
    const UINT dpi = GetDpiForWindow(handle_);
    RECT bounds{0, 0, width, height};
    AdjustWindowRectExForDpi(&bounds, windowStyle, FALSE, 0, dpi);
    SetWindowPos(handle_, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK Window::route(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        // lparam is the CREATESTRUCT for this message. The pointer stored in
        // lpCreateParams is the Window* passed to CreateWindowExW.
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* window = static_cast<Window*>(create->lpCreateParams);
        window->handle_ = handle;

        // Store the Window* as an integer-sized slot, then read that same pointer back.
        SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));

    // Before WM_NCCREATE finishes, forward to the default window procedure.
    return window ? window->process(message, wparam, lparam)
                  : DefWindowProcW(handle, message, wparam, lparam);
}

LRESULT Window::process(UINT message, WPARAM wparam, LPARAM lparam)
{
    // The messages this window understands. Anything else goes to DefWindowProc.
    switch (message)
    {
    case WM_CLOSE:
        running_ = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {930, 620};
        return 0;
    }
    case WM_DPICHANGED: {
        const auto& r = *reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(handle_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_KEYDOWN:

        // Bit 30 of lparam is the previous key state. A set bit means this is a repeat.
        if (wparam < input_.pressed.size() && (lparam & (1L << 30)) == 0)
        {
            input_.pressed[wparam] = true;
        }
        return 0;
    case WM_MOUSEWHEEL:
        // Accumulate wheel delta so partial notches count toward one step.
        wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
        input_.wheel += wheelRemainder_ / WHEEL_DELTA;
        wheelRemainder_ %= WHEEL_DELTA;
        return 0;
    case WM_MOUSEMOVE: {
        // GET_X_LPARAM sign-extends the low word. A narrowing cast to float is explicit.
        input_.mouse = {static_cast<float>(GET_X_LPARAM(lparam)),
                        static_cast<float>(GET_Y_LPARAM(lparam))};
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, handle_, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (GetCapture() != handle_)
        {
            input_.mouse = {-1, -1};
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        // Latch paint, erase, or stir while capture keeps tracking outside the client.
        input_.mouse = {static_cast<float>(GET_X_LPARAM(lparam)),
                        static_cast<float>(GET_Y_LPARAM(lparam))};
        input_.paint |= message == WM_LBUTTONDOWN;
        input_.erase |= message == WM_RBUTTONDOWN;
        input_.stir |= message == WM_MBUTTONDOWN;
        if (message == WM_LBUTTONDOWN)
        {
            input_.paintDown = input_.mouse;
            input_.paintPressed = true;
        }
        input_.erasePressed |= message == WM_RBUTTONDOWN;
        input_.stirPressed |= message == WM_MBUTTONDOWN;
        SetCapture(handle_);
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        // Clear the matching button and release capture when none remain down.
        input_.paint &= message != WM_LBUTTONUP;
        input_.erase &= message != WM_RBUTTONUP;
        input_.stir &= message != WM_MBUTTONUP;
        if (!input_.paint && !input_.erase && !input_.stir)
        {
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        // Dropping focus or capture cancels in-progress drags.
        input_.paint = input_.erase = input_.stir = false;
        return 0;
    default:
        return DefWindowProcW(handle_, message, wparam, lparam);
    }
}

} // namespace sand
