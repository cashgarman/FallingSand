# Falling Sand

**Falling Sand** is a real-time **2D falling-sand / cellular automata** sandbox for Windows. You paint ten materials onto a **384×224** cell grid; **granular solids** fall, **gases** rise, **liquids** are solved with a GPU pressure–flux pipeline, and **thermal chemistry** drives fire, smoke, steam, melting, and quenching. The simulation and renderer run almost entirely on the **Vulkan 1.3** device via **GLSL 460** compute and graphics shaders; the host is **C++23** with a native **Win32** window and an immediate-mode pixel UI composited in a second draw pass.

Default startup scene is **Thermal** (`Scene::thermal`). The intro blurb in older docs referred to “Thermal Vent” — that preset matches `layouts/thermal.json`.

---

## Table of contents {#table-of-contents}

<ol>
<li><a href="#features">Features</a></li>
<li><a href="#requirements">Requirements</a></li>
<li><a href="#repository-layout">Repository layout</a></li>
<li><a href="#build-and-run">Build and run</a></li>
<li><a href="#command-line-options">Command-line options</a></li>
<li><a href="#controls-and-ui">Controls and UI</a></li>
<li><a href="#scenes-and-layouts">Scenes and layouts</a></li>
<li><a href="#save-format">Save format</a></li>
<li><a href="#data-model">Data model</a></li>
<li><a href="#gpu-buffers-and-ping-pong">GPU buffers and ping-pong</a></li>
<li><a href="#frame-pipeline-host">Frame pipeline (host)</a></li>
<li><a href="#simulation-tick-compute">Simulation tick (compute)</a></li>
<li><a href="#rendering-path-graphics">Rendering path (graphics)</a></li>
<li><a href="#shared-shader-library-worldglsl">Shared shader library (<code>world.glsl</code>)</a></li>
<li><a href="#testing-and-benchmarks">Testing and benchmarks</a></li>
<li><a href="#further-reading">Further reading</a></li>
</ol>

---

## Features {#features}

- **Ten paintable materials** (sand, water, oil, stone, wood, fire, smoke, steam, lava, ice) plus empty cells for erase.
- **Four built-in scenes** loaded from JSON (`waterworks`, `thermal`, `refinery`, `empty`).
- **GPU simulation** at **90 Hz** during play (fixed `simulationHz`), with pause and single-step.
- **Brush painting**, **erase** (right mouse), and **stir** (middle mouse) implemented in `brush.comp`.
- **Instanced HUD**: material palette, scene tabs, transport controls, FPS/GPU badge, brush size track, pause/settings overlays (`ui.cpp` → `ui.vert` / `ui.frag`).
- **Save/load** of cell state to a binary sandbox file under `%LOCALAPPDATA%\FallingSand\world.sandbox` (or current-directory fallback).

---

## Requirements {#requirements}

| Component | Version / notes |
| --- | --- |
| OS | **Windows x64** only (`CMakeLists.txt` rejects non-Win32). |
| CMake | **3.28+** |
| Compiler | **Visual Studio 2022** (x64), C++23 (`/W4`, static MSVC runtime in Release) |
| Vulkan | **1.3** instance/device, `glslc` from the Vulkan SDK (`Vulkan::Vulkan` + `glslc` target) |
| GPU | Compute workgroups up to **16×16**, **≥10** storage buffers per stage, timestamp queries optional |

Runtime: place **`FallingSand.exe`** next to copied **`shaders/`** (SPIR-V) and **`layouts/`** (JSON). CMake target `sand_shaders` and `sand_layouts` copy these into `build/<Config>/` automatically.

---

## Repository layout {#repository-layout}

```
FallingSand/
├── CMakeLists.txt          # Executable, shader compile (glslc), layout copy, CTest
├── build-and-run.bat       # Configure (VS 2022 x64), build, launch exe
├── README.md               # This file
├── STYLE.md                # Shader comment/style guide (not installed by CMake)
├── src/
│   ├── main.cpp            # Entry, CLI, game loop, Sandbox input/actions
│   ├── window.cpp/.hpp     # Win32 window + Input (mouse buttons, keys, wheel)
│   ├── game.cpp/.hpp       # Cell/Material/Scene, layouts, save/load, makeScene
│   ├── ui.cpp/.hpp         # HUD layout, hit tests, composeUi → Quad list
│   └── vulkan.cpp/.hpp     # Device, swapchain, compute + graphics pipelines, draw()
├── shaders/
│   ├── world.glsl          # Shared buffers, Cell, push constants, helpers
│   ├── *.comp              # Simulation + brush compute kernels
│   ├── fullscreen.vert     # Full-screen triangle for world pass
│   ├── world.frag          # World visualization (read-only world.glsl)
│   ├── ui.vert / ui.frag   # Instanced UI quads
│   └── …                     # Sources compiled to .spv beside the exe
└── layouts/
    ├── waterworks.json
    ├── thermal.json
    ├── refinery.json
    └── empty.json
```

Other local folders (`build/`, `build-ninja/`, `dist/`) are build or install outputs and are not part of the source tree.

---

## Build and run {#build-and-run}

### Quick script

```bat
build-and-run.bat
build-and-run.bat Debug
build-and-run.bat Release --scene empty --validate
```

### Manual (PowerShell)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\FallingSand.exe
```

Shaders are compiled with:

`glslc --target-env=vulkan1.3 -O <shader> -o <shader>.spv`

and depend on `shaders/world.glsl` where included.

Install layout (optional):

```powershell
cmake --install build --config Release --prefix dist/FallingSand
```

`cmake --install` copies the executable, SPIR-V under `shaders/`, layouts, and `README.md`.

---

## Command-line options {#command-line-options}

Parsed in `main.cpp` (`parseOptions`):

| Flag | Meaning |
| --- | --- |
| `--validate` | Enable Khronos validation (and sync validation where supported). |
| `--test` | Headless GPU simulation tests (CTest `gpu_simulation`). |
| `--window-test` | Automated UI/window tests (paint, step, resize, save/load). |
| `--benchmark N` | Run **N** frames at **1 tick/frame** after warm-up; print FPS and GPU timings. |
| `--capture PATH` | After `--after F` frames, write a BMP of the swapchain image to `PATH`. |
| `--after F` | Frame index for `--capture` (required with capture). |
| `--scene NAME` | Initial scene: `waterworks`, `thermal`, `refinery`, `empty`. Default: **thermal**. |

Unknown options throw `std::runtime_error`.

---

## Controls and UI {#controls-and-ui}

### Mouse

| Input | Effect |
| --- | --- |
| **Left button** | Paint on canvas (selected material) or click HUD controls. |
| **Right button** | Erase to empty on canvas. |
| **Middle button** | **Stir** — `brush.comp` mixes liquids along the stroke (`Stroke::stir`). |
| **Wheel** | Adjust brush radius (`minRadius` 2 … `maxRadius` 32). |

### Keyboard

| Key | Effect |
| --- | --- |
| `1`–`9`, `0` | Select materials (palette order in `materials` table). |
| `E` | Toggle **erase** tool (same as holding right mouse for painting mode). |
| `G` | Toggle **stir** tool. |
| `-` / `+` (`VK_OEM_MINUS` / `VK_OEM_PLUS`) | Decrease / increase brush size. |
| **Space** | Pause / resume simulation. |
| `N` | Advance **one** simulation tick while paused. |
| `R` | Reset world to current scene preset. |
| **F5** | Save to sandbox file. |
| **F9** | Load sandbox file (clears velocity, pressure, and other flow buffers on GPU). |
| **Esc** | Open **pause** menu; from pause open **settings** (resolution). Esc backs out of menus. |

Quit is via the pause/settings **Exit** button (`Action::exit`), not Esc alone.

### HUD structure (`makeLayout` / `composeUi`)

- **Canvas** — letterboxed region mapping client pixels to world coordinates (`Layout::worldPoint`).
- **Panel** — material swatches and scene tabs.
- **Footer** — play/pause, step, reset, save/load, tool cycle, brush size track.
- **Cog** — settings while running; in pause menu, settings for resolution presets (1280×800 … 2560×1440).
- **Status line** — short messages after save/load/errors.
- **Tooltip** — hovering the canvas probes GPU cell and shows dominant material.

UI geometry is rebuilt each frame from window size; draw records are up to **`maxQuads` (16384)** `Quad` structs (32 bytes, `alignas(16)`) uploaded to a per-frame storage buffer.

---

## Scenes and layouts {#scenes-and-layouts}

`Scene` enum (`game.hpp`):

| ID | Tab name | Layout file |
| --- | --- | --- |
| `waterworks` | Waterworks preset | `layouts/waterworks.json` |
| `thermal` | Thermal vent | `layouts/thermal.json` |
| `refinery` | Refinery | `layouts/refinery.json` |
| `empty` | Blank canvas | `layouts/empty.json` |

`makeScene` reads JSON beside the executable, validates dimensions, and fills a `worldCells` vector. Switching scenes from the UI **replaces** the grid (save first if you care about the current world).

---

## Save format {#save-format}

`saveWorld` / `loadWorld` (`game.cpp`):

- Magic: **`SAND0001`** (8 bytes).
- `uint32_t` width, height — must match `worldWidth` (384) and `worldHeight` (224).
- `worldCells` × **16-byte `Cell`** records (trivially copyable, GPU-matched layout).

Writes use a `.tmp` sibling file when possible. Load does not restore velocity or pressure; `VulkanGame::replaceWorld` uploads cells and **zero-fills** flow/auxiliary buffers (indices 2, 3, 5, 6 in storage).

Default path: `%LOCALAPPDATA%\FallingSand\world.sandbox`, else `./world.sandbox` (`saveLocation()` in `main.cpp`).

---

## Data model {#data-model}

### `Cell` (CPU and GPU, 16 bytes)

```cpp
struct Cell {
    std::array<float, 3> liquid;  // x=water, y=oil, z=lava — fill in [0, 1]
    std::uint32_t matter;        // packed id, age, heat
};
```

Shader-side mirror in `world.glsl` (`vec3 liquid`, `uint matter`).

### Packed `matter` word

| Bits | Field | Access |
| --- | --- | --- |
| 0–7 | Material id | `material(c)` → `c.matter & 255u` |
| 8–15 | Age (e.g. smoke/fire lifetime) | `age(c)` |
| 16–25 | Heat (0…1023 → 0…1) | `heat(c)` |
| (upper) | Remaining bits unused in helpers | `packMatter(id, life, temperature)` |

Material constants (shader and logic): `EMPTY`, `SAND`, `WATER`, `OIL`, `STONE`, `WOOD`, `FIRE`, `SMOKE`, `STEAM`, `LAVA`, `ICE` (0–10).

### Liquids

- Three **immiscible fill channels** share one velocity field; **separate.comp** slowly exchanges oil/lava vs water between staggered row pairs.
- `wet(c)` when total liquid `volume(c) > 0.028`.
- **Inverse density** for pressure face weights uses channel weights `(1.0, 0.78, 2.3)` in `inverseDensity`.

### Solids and gases

- `solid(c)`: stone, wood, ice, sand — block face connections via `faceWeight`.
- `gas(id)`: fire, smoke, steam — handled in **grains** (rise) and **reaction**.

### Host helpers

- `makeCell(Material, quantity)` — initial paint/scene values.
- `displayMaterial` / `kind` — UI and tooltips from liquid + matter.

---

## GPU buffers and ping-pong {#gpu-buffers-and-ping-pong}

From `world.glsl` (compute; read-only subset in `world.frag` via `WORLD_READ_ONLY`):

| Binding | Buffer | Role |
| --- | --- | --- |
| 0–1 | `CellsA`, `CellsB` | Cell ping-pong (`pc.state` selects read; writes use `writeCell` / `writeOther`) |
| 2 | `Velocity` | Staggered-style face flow used by flux/advect/project |
| 3 | `Prediction` | Predicted velocity (predict pass) |
| 4 | `Equations` | Pressure network row per cell (`Equation`: `vec4 weights`, `float rhs`) |
| 5–6 | `PressureA`, `PressureB` | Pressure ping-pong for Jacobi |
| 7 | `Flux` | Outward liquid flux per cell |
| 8–9 | `LimitA`, `LimitB` | Inflow capacity ping-pong for limit/advect |

**Push constant `Frame`** (`world.glsl`): grid `size`, ping-pong `state`, `tick`, `operation` (`mode`, `phase`, `material`, spare), `brush` segment, `canvas` rect, `cursor` (world xy, radius, visibility), `screen` (width, height, visual time).

`vulkan.cpp` constants:

- **`pressureBatches = 12`** — 12 pressure dispatches per tick (red-black `operation[1] = pass & 1`).
- **`fluxLimitPasses = 5`** — odd count so limit field settles before advection.

---

## Frame pipeline (host) {#frame-pipeline-host}

Each frame (`VulkanGame::draw`):

1. Wait on frame-in-flight fence; optional timestamp readback.
2. Acquire swapchain image.
3. Upload **UI quads** to the slot’s storage buffer.
4. **Compute** (if stroke and/or `ticks > 0`):
   - Optional **`brush.comp`** for the current stroke.
   - **`recordSim`**: `ticks` iterations of the simulation chain (below).
5. **Graphics** (`recordDraw`):
   - Barrier: compute writes → fragment shader read.
   - Dynamic rendering: clear → **world** pass (scissored to canvas) → **UI** pass (full window).
   - Present (or capture BMP via transfer).

Play loop (`main.cpp`): when not paused, accumulates real time and runs `floor(accumulator / tickSeconds)` ticks per frame (`tickSeconds = 1/90`). Paused runs 0 ticks unless **N** or UI **step** sets a one-shot tick. Benchmark/capture modes force **1 tick/frame**.

---

## Simulation tick (compute) {#simulation-tick-compute}

Order inside **one tick** (`VulkanGame::Impl::recordSim` in `vulkan.cpp`). Brush is **outside** this loop and runs once per frame when the user strokes.

| Step | Kernel | Workgroup layout | What it does |
| ---: | --- | --- | --- |
| 1 | **reaction.comp** | 16×8 | Chemistry: heat, fire, quenching, melting, smoke/steam lifetimes; updates `matter` and liquid mix. |
| — | `state ^= 1` | — | Toggle cell buffer ping-pong. |
| 2 | **grains.comp** | 16×8 over 2×2 blocks | Sand **falls**, gas **rises**; local 2×2 updates to reduce races. |
| 3 | **predict.comp** | 16×8 | Semi-Lagrangian **velocity prediction** from neighbors, viscosity, gravity. |
| 4 | **equation.comp** | 16×8 | Build **pressure network row** per wet, non-solid cell (`faceWeight` on neighbors). |
| 5 | **pressure.comp** ×12 | 16×16, shared mem | **Jacobi** relaxation on pressure (two iterations per dispatch, halo tile); red-black phases via `operation[1]`. |
| 6 | **project.comp** | 16×8 | **Project** predicted velocity with pressure gradients (divergence reduction). |
| 7 | **flux.comp** | 16×8 | Face **outward liquid flux** from velocity; negative flow pulls from neighbors. |
| 8 | **limit.comp** ×5 | 16×8 | **Inflow limits** ping-pong (`limitA`/`limitB`); how much each cell can accept. |
| 9 | **advect.comp** | 16×8 | Move **water/oil/lava** along flux subject to limits; writes other cell buffer. |
| — | `state ^= 1` | — | Toggle cell ping-pong after advection. |
| 10 | **separate.comp** ×2 | 16×8, half height dispatch | **Phase separation** oil/lava vs water on staggered rows; `operation[1]` checkerboard with `tick`. |

**brush.comp** (when painting): dispatches over a bounding box of the stroke; modes — deposit material/liquid, erase, or stir (`pc.operation` mode + `pc.brush` + radius).

This design is a **bounded, real-time 2D fluid approximation**: shared velocity, finite pressure iterations, finite-volume transport with donor/receiver limits — suited for gameplay rather than offline CFD accuracy.

---

## Rendering path (graphics) {#rendering-path-graphics}

| Stage | Shader | Notes |
| --- | --- | --- |
| World | **fullscreen.vert** + **world.frag** | Single large triangle; fragment shader maps canvas pixels → grid coords; includes `world.glsl` read-only. Shading: material/liquid reconstruction, absorption, highlights, Fresnel, refraction, lava glow, procedural noise. |
| UI | **ui.vert** + **ui.frag** | Instanced quads (`6` vertices × `quadCount`); per-quad bounds + RGBA from CPU `Quad` list. Bitmap font and panels drawn entirely on CPU in `ui.cpp`. |

Push constants for both passes share `canvas`, `cursor`, `screen` (and simulation uses the same layout block).

Pipeline: **Vulkan dynamic rendering** (no classic render pass object), swapchain format color attachment, scissor for world letterboxing.

---

## Shared shader library (`world.glsl`) {#shared-shader-library-worldglsl}

Included by every `.comp` and by `world.frag` (with `WORLD_READ_ONLY`).

- **Coordinate helpers**: `inside`, `indexOf`, `readCell`, `writeCell`, `writeOther`.
- **Material/thermo helpers**: `packMatter`, `solid`, `wet`, `gas`, `faceWeight`, `inverseDensity`.
- **RNG**: `hash32` / `random01` for stochastic grain motion.

Compute shaders use `#extension GL_GOOGLE_include_directive` and are compiled as Vulkan 1.3 SPIR-V.

Host code does not parse GLSL; keeping `Cell` layout and material IDs in sync is enforced by shared constants and static asserts in `game.hpp`.

---

## Testing and benchmarks {#testing-and-benchmarks}

```powershell
.\build\Release\FallingSand.exe --test --validate
.\build\Release\FallingSand.exe --window-test --validate
.\build\Release\FallingSand.exe --benchmark 1200 --scene thermal
```

- **`--test`**: GPU checks (walls, divergence reduction, phase conservation, sand, thermal reactions, brush) without a visible window loop.
- **`--window-test`**: Scene buttons, paint, step, resize, present, save/load.
- **`--benchmark`**: 60-frame warmup, then mean/p95 GPU ms and simulation ms; device name in header.

`--validate` enables validation layers for debugging synchronization and API usage.

---

## Further reading {#further-reading}

- Liquid solver inspiration: semi-Lagrangian advection, pressure projection, bounded Jacobi — see [Bridson & Müller-Fischer, SIGGRAPH fluids course notes](https://www.cs.ubc.ca/~rbridson/fluidsimulation/).
- Swapchain synchronization: [Khronos Vulkan Guide — swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
- Shader authorship conventions in this repo: `STYLE.md` (optional reference for contributors).

---

## License

No license file is present in the repository root; treat usage as unspecified unless your organization adds one.