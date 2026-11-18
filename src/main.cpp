#include "game.hpp"
#include "ui.hpp"
#include "vulkan.hpp"
#include "window.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <windowsx.h>

namespace sand {
namespace {

using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;
constexpr double tickSeconds = 1.0 / simulationHz;

struct Options
{
    bool test{};
    bool windowTest{};
    bool validate{};
    unsigned benchmarkFrames{};
    unsigned captureAfter{};
    Scene scene{Scene::thermal};
    std::filesystem::path capture;
};

Options parse(std::span<char*> arguments)
{
    Options options;
    const auto number = [](std::string_view word) {
        unsigned value{};
        const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
        if (error != std::errc{} || end != word.data() + word.size() || value > 100'000)
        {
            throw std::runtime_error("Expected an integer between 0 and 100000.");
        }
        return value;
    };
    for (std::size_t i = 1; i < arguments.size(); ++i)
    {
        const std::string_view key = arguments[i];
        const auto next = [&] {
            if (++i == arguments.size())
            {
                throw std::runtime_error(std::format("Missing value for {}.", key));
            }
            return std::string_view(arguments[i]);
        };
        if (key == "--validate")
        {
            options.validate = true;
        } else if (key == "--test")
        {
            options.test = true;
        } else if (key == "--window-test")
        {
            options.windowTest = true;
        } else if (key == "--benchmark")
        {
            options.benchmarkFrames = std::max(1u, number(next()));
        } else if (key == "--capture")
        {
            options.capture = std::filesystem::path(next());
        } else if (key == "--after")
        {
            options.captureAfter = number(next());
        } else if (key == "--scene")
        {
            const auto scene = next();
            constexpr std::array<std::string_view, sceneNames.size()> keys{"waterworks", "thermal",
                                                                           "refinery", "empty"};
            const auto found = std::ranges::find(keys, scene);
            if (found == keys.end())
            {
                throw std::runtime_error("Scenes are waterworks, thermal, refinery, and empty.");
            }
            options.scene = static_cast<Scene>(found - keys.begin());
        } else
        {
            throw std::runtime_error(std::format("Unknown option {}.", key));
        }
    }
    return options;
}

void require(bool result, std::string_view test)
{
    if (!result)
    {
        throw std::runtime_error(std::format("GPU test failed: {}.", test));
    }
}

std::array<double, 3> phaseTotals(std::span<const Cell> cells)
{
    std::array<double, 3> count{};
    for (const auto& cell : cells)
    {
        float sum = 0.0F;
        for (std::size_t i = 0; i < count.size(); ++i)
        {
            const auto v = cell.liquid[i];
            require(std::isfinite(v) && v >= -0.000001F, "finite, nonnegative liquid");
            count[i] += v;
            sum += v;
        }
        require(sum <= 1.00001F, "bounded cell fill");
    }
    return count;
}

void simulationTests(bool validate)
{
    using enum Material;
    VulkanGame game(nullptr, false, validate);
    auto cells = makeScene(Scene::empty);
    const auto index = [](int x, int y) { return y * worldWidth + x; };
    const auto fill = [&](int x0, int y0, int x1, int y1, Material m) {
        for (int y = y0; y < y1; ++y)
        {
            for (int x = x0; x < x1; ++x)
            {
                cells[index(x, y)] = makeCell(m);
            }
        }
    };
    fill(70, 144, 161, 148, stone);
    fill(70, 108, 74, 144, stone);
    fill(157, 108, 161, 144, stone);
    fill(74, 120, 157, 144, water);
    fill(92, 120, 126, 124, oil);
    fill(105, 95, 119, 99, sand);
    fill(260, 60, 276, 68, lava);
    const auto totals = phaseTotals(cells);
    game.replaceWorld(cells);
    game.paint({{102.0F, 132.0F}, {106.0F, 134.0F}, 7.0F, water, true});
    game.simulate(1);

    const auto flow = game.flowSnapshot();
    const auto rmsDivergence = [&](std::span<const Point> field) {
        double sum{};
        int count{};
        for (int y = 123; y < 143; ++y)
        {
            for (int x = 76; x < 155; ++x)
            {
                const auto i = index(x, y);
                const double d = field[i].x - field[i - 1].x + field[i].y - field[i - worldWidth].y;
                sum += d * d;
                ++count;
            }
        }
        return std::sqrt(sum / count);
    };
    const double before = rmsDivergence(flow.predictor);
    const double after = rmsDivergence(flow.projected);
    require(before > 0.001 && after < before * 0.50, "pressure projection reduces RMS divergence");
    require(flow.projected[index(110, 143)].y == 0.0F, "solid face has zero normal velocity");
    game.simulate(65);
    const auto result = game.snapshot();
    const auto afterTotals = phaseTotals(result);
    for (std::size_t i = 0; i < afterTotals.size(); ++i)
    {
        require(std::abs(afterTotals[i] - totals[i]) < 0.02, "transport conserves each liquid");
    }
    require(result[index(110, 144)].matter == std::to_underlying(stone), "tank wall was retained");
    require(std::ranges::count_if(result, [](const Cell& c) { return kind(c) == sand; }) == 56,
            "granular motion conserves grains");
    const auto phaseCenter = [&](std::span<const Cell> state, std::size_t phase, double volume) {
        double position{};
        for (auto [i, c] : std::views::enumerate(state))
        {
            position += static_cast<double>(i / worldWidth) * c.liquid[phase];
        }
        return position / volume;
    };
    require(phaseCenter(result, 2, totals[2]) > phaseCenter(cells, 2, totals[2]) + 12.0,
            "lava falls and advects");
    require(phaseCenter(result, 1, totals[1]) < phaseCenter(result, 0, totals[0]),
            "oil remains above denser water");

    cells = makeScene(Scene::empty);
    fill(51, 40, 60, 44, ice);
    fill(51, 44, 60, 48, lava);
    fill(62, 44, 72, 49, water);
    fill(87, 56, 97, 60, wood);
    fill(87, 60, 97, 63, fire);
    fill(50, 49, 73, 53, stone);
    fill(50, 40, 51, 49, stone);
    fill(72, 40, 73, 49, stone);
    game.replaceWorld(cells);
    game.simulate(28);
    const auto reacted = game.snapshot();
    require(std::ranges::count_if(reacted, [](const Cell& c) { return kind(c) == ice; }) < 36,
            "heat melts ice");
    require(std::ranges::count_if(reacted, [](const Cell& c) { return kind(c) == steam; }) > 0,
            "hot water creates steam");
    require(std::ranges::count_if(reacted, [](const Cell& c) { return kind(c) == stone; }) > 110,
            "quenched lava produces stone");
    require(std::ranges::count_if(reacted, [](const Cell& c) { return kind(c) == wood; }) < 40,
            "fire ignites wood");
    game.paint({{165, 120}, {190, 120}, 4, water, false});
    auto painted = game.snapshot();
    require(painted[index(178, 120)].liquid[0] > 0.99F, "continuous brush stroke");
    game.paint({{177, 120}, {179, 120}, 2, empty, false});
    require(game.snapshot()[index(178, 120)].liquid[0] == 0.0F, "brush erases liquid");
    require(game.validationErrors() == 0, "no Vulkan validation errors");
    std::println("GPU tests passed on {}. RMS divergence {:.5f} -> {:.5f}.", game.deviceName(),
                 before, after);
}

std::filesystem::path saveLocation()
{
    std::wstring folder(32768, L'\0');
    const DWORD length =
        GetEnvironmentVariableW(L"LOCALAPPDATA", folder.data(), static_cast<DWORD>(folder.size()));
    if (length == 0 || length >= folder.size())
    {
        return std::filesystem::current_path() / "world.sandbox";
    }
    folder.resize(length);
    return std::filesystem::path(folder) / "FallingSand" / "world.sandbox";
}

class Sandbox
{
  public:
    Sandbox(VulkanGame& game, Scene scene) : gpu_(game)
    {
        state.scene = scene;
        gpu_.replaceWorld(makeScene(scene));
    }

    UiState state;
    bool step{};

    void input(const Input& inputs, const Layout& layout)
    {
        if (inputs.pressed[VK_SPACE])
            action(Action::play, 0);
        if (inputs.pressed['N'])
            action(Action::step, 0);
        if (inputs.pressed['R'])
            action(Action::reset, 0);
        if (inputs.pressed[VK_F5])
            action(Action::save, 0);
        if (inputs.pressed[VK_F9])
            action(Action::load, 0);
        if (inputs.pressed['E'])
            toggle(Tool::erase);
        if (inputs.pressed['G'])
            toggle(Tool::stir);
        if (inputs.pressed[VK_OEM_MINUS])
            action(Action::sizeDown, 0);
        if (inputs.pressed[VK_OEM_PLUS])
            action(Action::sizeUp, 0);
        for (int i = 0; i < static_cast<int>(materials.size()); ++i)
        {
            const auto key = i == 9 ? '0' : '1' + i;
            if (inputs.pressed[key])
            {
                action(Action::choose, i);
            }
        }
        if (inputs.wheel != 0)
        {
            adjustRadius(inputs.wheel);
        }
        if (inputs.paintPressed)
        {
            drawDrag_ = layout.canvas.contains(inputs.paintDown);
            sliderDrag_ = false;
            if (auto pressed = layout.hit(inputs.paintDown))
            {
                sliderDrag_ = pressed->action == Action::brush;
                if (!sliderDrag_)
                {
                    action(pressed->action, pressed->argument);
                }
            }
        }
        if (sliderDrag_ && (inputs.paint || inputs.paintPressed))
        {
            const auto& track = layout.brushTrack;
            const float x = inputs.paint ? inputs.mouse.x : inputs.paintDown.x;
            const float t = std::clamp((x - track.x) / track.w, 0.0F, 1.0F);
            state.radius = minRadius + static_cast<int>(std::round((maxRadius - minRadius) * t));
        }
        if (!inputs.paint && !inputs.paintPressed)
        {
            drawDrag_ = sliderDrag_ = false;
        }
        if (Clock::now() >= statusUntil_)
        {
            state.status = {};
        }
    }

    std::optional<Stroke> stroke(const Input& input, const Layout& layout)
    {
        const bool paint = (input.paint || input.paintPressed) && drawDrag_;
        const bool erase = input.erase || input.erasePressed;
        const bool stir = input.stir || input.stirPressed;
        const bool pulse = !input.paint && !input.erase && !input.stir;
        const auto mouse = pulse && paint ? input.paintDown : input.mouse;
        if ((!paint && !erase && !stir) || !layout.canvas.contains(mouse))
        {
            previous_.reset();
            return std::nullopt;
        }
        const auto point = layout.worldPoint(mouse);
        const Tool tool = erase ? Tool::erase : stir ? Tool::stir : state.tool;
        const auto material = tool == Tool::erase ? Material::empty : state.material;
        const Point start = previous_ && tool == previousTool_ && material == previousMaterial_
                                ? *previous_
                                : point;
        previous_ = pulse ? std::nullopt : std::optional(point);
        previousTool_ = tool;
        previousMaterial_ = material;
        return Stroke{start, point, static_cast<float>(state.radius), material, tool == Tool::stir};
    }

  private:
    VulkanGame& gpu_;
    std::filesystem::path save_{saveLocation()};
    std::optional<Point> previous_;
    Tool previousTool_{};
    Material previousMaterial_{};
    bool drawDrag_{};
    bool sliderDrag_{};
    std::string status_;
    Clock::time_point statusUntil_{};

    void toggle(Tool tool)
    {
        state.tool = state.tool == tool ? Tool::paint : tool;
        previous_.reset();
    }

    void showStatus(std::string message)
    {
        status_ = std::move(message);
        state.status = status_;
        statusUntil_ = Clock::now() + std::chrono::seconds(3);
    }

    void adjustRadius(int amount)
    {
        state.radius = std::clamp(state.radius + amount, minRadius, maxRadius);
    }

    void action(Action action, int argument)
    {
        switch (action)
        {
        case Action::choose:
            state.material = materials[argument].id;
            state.tool = Tool::paint;
            break;
        case Action::scene:
            state.scene = static_cast<Scene>(argument);
            gpu_.replaceWorld(makeScene(state.scene));
            previous_.reset();
            break;
        case Action::play:
            state.paused = !state.paused;
            break;
        case Action::step:
            state.paused = true;
            step = true;
            break;
        case Action::reset:
            gpu_.replaceWorld(makeScene(state.scene));
            previous_.reset();
            showStatus("SCENE RESET");
            break;
        case Action::save: {
            std::error_code error;
            if (save_.has_parent_path())
            {
                std::filesystem::create_directories(save_.parent_path(), error);
            }
            auto saved = error
                             ? std::expected<void, std::string>(std::unexpected("SAVE UNAVAILABLE"))
                             : saveWorld(save_, gpu_.snapshot());
            showStatus(saved ? "WORLD SAVED" : saved.error());
            break;
        }
        case Action::load: {
            auto loaded = loadWorld(save_);
            if (loaded)
            {
                gpu_.replaceWorld(*loaded);
                previous_.reset();
            }
            showStatus(loaded ? "WORLD LOADED" : loaded.error());
            break;
        }
        case Action::sizeDown:
            adjustRadius(-1);
            break;
        case Action::sizeUp:
            adjustRadius(1);
            break;
        case Action::cycleTool:
            state.tool = static_cast<Tool>((std::to_underlying(state.tool) + 1) % 3);
            break;
        case Action::brush:
            break;
        }
    }
};

void windowTests(Window& window, VulkanGame& gpu)
{
    Sandbox sandbox(gpu, Scene::waterworks);
    const auto [w, h] = window.size();
    const auto [width, height] = gpu.prepare(w, h);
    auto layout = makeLayout(width, height);
    std::vector<Quad> quads;
    const auto click = [&](Rect bounds) {
        const auto x = static_cast<int>(bounds.x + bounds.w / 2);
        const auto y = static_cast<int>(bounds.y + bounds.h / 2);
        const auto point = MAKELPARAM(x, y);
        SendMessageW(window.handle(), WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(window.handle(), WM_LBUTTONUP, 0, point);
    };
    click(layout.scenes.back());
    sandbox.input(window.consumeInput(), layout);
    require(sandbox.state.scene == Scene::empty, "scene button");
    click(layout.swatches[0]);
    sandbox.input(window.consumeInput(), layout);
    require(sandbox.state.material == Material::sand, "material button");
    click(layout.controls[1].bounds);
    sandbox.input(window.consumeInput(), layout);
    require(sandbox.state.paused && std::exchange(sandbox.step, false), "step button");
    click(layout.canvas);
    const auto paint = window.consumeInput();
    sandbox.input(paint, layout);
    composeUi(quads, layout, sandbox.state, paint.mouse, width, height);
    require(gpu.draw({layout.canvas, {}, 8, false, 0, sandbox.stroke(paint, layout), 0, quads, {}}),
            "present after painting");
    require(std::ranges::count_if(gpu.snapshot(),
                                  [](const Cell& c) { return kind(c) == Material::sand; }) > 100,
            "single-click paint");
    SetWindowPos(window.handle(), nullptr, 0, 0, 1070, 680,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const auto [newW, newH] = window.size();
    const auto [drawW, drawH] = gpu.prepare(newW, newH);
    require(drawW == newW && drawH == newH && drawW < width, "resize swapchain");
    layout = makeLayout(drawW, drawH);
    composeUi(quads, layout, sandbox.state, {}, drawW, drawH);
    for (int i = 0; i < 7; ++i)
    {
        require(gpu.draw({layout.canvas, {}, 8, false, 0, {}, 1, quads, {}}),
                "present resized window");
    }
    const auto saved = std::filesystem::temp_directory_path() /
                       std::format("falling-sand-{}.sandbox", GetCurrentProcessId());
    require(saveWorld(saved, gpu.snapshot()).has_value(), "save world");
    auto loaded = loadWorld(saved);
    std::filesystem::remove(saved);
    require(
        loaded.has_value() &&
            std::ranges::any_of(*loaded, [](const Cell& c) { return kind(c) == Material::sand; }),
        "load saved world");
    gpu.wait();
    require(gpu.validationErrors() == 0, "no Vulkan validation errors in windowed mode");
    std::println(
        "Window tests passed for scenes, materials, painting, stepping, resize, and save/load.");
}

void reportBenchmark(std::vector<double>& gpu, const std::vector<double>& sim, unsigned frames,
                     double elapsed, std::string_view device)
{
    std::ranges::sort(gpu);
    const auto average = [](std::span<const double> values) {
        return values.empty() ? 0.0
                              : std::accumulate(values.begin(), values.end(), 0.0) /
                                    static_cast<double>(values.size());
    };
    std::println("{} | {}x{} grid, one simulation tick per frame | {} frames | {:.1f} FPS", device,
                 worldWidth, worldHeight, frames, frames / elapsed);
    if (!gpu.empty())
    {
        const double p95 = gpu[static_cast<std::size_t>(0.95 * (gpu.size() - 1))];
        std::println("GPU mean {:.3f} ms | p95 {:.3f} ms | simulation mean {:.3f} ms", average(gpu),
                     p95, average(sim));
    }
}

void play(const Options& options)
{
    Window window(true);
    const bool fast = options.benchmarkFrames != 0 || options.windowTest;
    VulkanGame gpu(window.handle(), fast, options.validate);
    if (options.windowTest)
    {
        windowTests(window, gpu);
        return;
    }
    Sandbox sandbox(gpu, options.scene);
    std::vector<Quad> quads;
    quads.reserve(maxQuads);
    std::vector<double> gpuMs, simMs;
    const auto start = Clock::now();
    auto previousTime = start;
    auto statsTime = start;
    auto benchmarkTime = start;
    double accumulator{};
    float visualTime{};
    unsigned rendered = 0, statsFrames = 0;
    constexpr unsigned warmup = 60;

    while (window.poll())
    {
        const auto [clientW, clientH] = window.size();
        const auto [w, h] = gpu.prepare(clientW, clientH);
        if (w <= 0 || h <= 0)
        {
            window.wait();
            previousTime = Clock::now();
            continue;
        }
        const auto input = window.consumeInput();
        if (input.pressed[VK_ESCAPE])
        {
            break;
        }
        const auto now = Clock::now();
        const double elapsed = Seconds(now - previousTime).count();
        previousTime = now;
        const auto layout = makeLayout(w, h);
        sandbox.input(input, layout);

        unsigned ticks = 0;
        if (fast || !options.capture.empty())
        {
            ticks = 1;
        } else if (sandbox.state.paused)
        {
            accumulator = 0.0;
            ticks = std::exchange(sandbox.step, false) ? 1u : 0u;
        } else
        {
            accumulator = std::min(accumulator + elapsed, tickSeconds * 4.0);
            ticks = static_cast<unsigned>(accumulator / tickSeconds);
            accumulator -= ticks * tickSeconds;
        }
        visualTime += static_cast<float>(ticks * tickSeconds);
        const bool cursor = layout.canvas.contains(input.mouse);
        const auto worldMouse = layout.worldPoint(input.mouse);
        const auto interval = Seconds(now - statsTime).count();
        if (interval >= 0.4)
        {
            sandbox.state.fps = static_cast<int>(std::round(statsFrames / interval));
            sandbox.state.gpuMs = static_cast<float>(gpu.timing().gpuMs);
            statsFrames = 0;
            statsTime = now;
        }
        composeUi(quads, layout, sandbox.state, input.mouse, w, h);
        const auto capture =
            rendered == options.captureAfter ? options.capture : std::filesystem::path{};
        if (!gpu.draw({layout.canvas, worldMouse, static_cast<float>(sandbox.state.radius), cursor,
                       visualTime, sandbox.stroke(input, layout), ticks, quads, capture}))
        {
            continue;
        }
        ++rendered;
        ++statsFrames;
        if (!capture.empty())
        {
            std::println("Captured {}.", capture.string());
            break;
        }
        if (options.benchmarkFrames)
        {
            if (rendered == warmup)
            {
                gpu.wait();
                benchmarkTime = Clock::now();
            } else if (rendered > warmup)
            {
                auto [simulation, total] = gpu.timing();
                if (total > 0.0)
                {
                    gpuMs.push_back(total);
                    simMs.push_back(simulation);
                }
                if (rendered - warmup == options.benchmarkFrames)
                {
                    gpu.wait();
                    reportBenchmark(gpuMs, simMs, options.benchmarkFrames,
                                    Seconds(Clock::now() - benchmarkTime).count(),
                                    gpu.deviceName());
                    break;
                }
            }
        }
    }
    gpu.wait();
    if (gpu.validationErrors() != 0)
    {
        throw std::runtime_error("Vulkan validation reported errors.");
    }
}

} // namespace
} // namespace sand

int main(int argc, char* argv[])
{
    try
    {
        const auto options = sand::parse(std::span(argv, static_cast<std::size_t>(argc)));
        if (options.test)
        {
            sand::simulationTests(options.validate);
        } else
        {
            if (argc == 1)
            {
                std::array<DWORD, 2> consoleProcesses{};
                if (GetConsoleProcessList(consoleProcesses.data(),
                                          static_cast<DWORD>(consoleProcesses.size())) == 1)
                {
                    FreeConsole();
                }
            }
            sand::play(options);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        if (argc == 1)
        {
            MessageBoxA(nullptr, error.what(), "Falling Sand", MB_OK | MB_ICONERROR);
        }
        return EXIT_FAILURE;
    }
}
