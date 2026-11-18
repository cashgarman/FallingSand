# Falling Sand

A Windows sandbox with ten paintable materials, three built-in experiments, and an empty canvas. The game starts in **Thermal Vent**, where gravity pours lava and water toward the same catch basin.

## Play

Run **FallingSand.exe** with the accompanying **shaders** and **layouts** directories beside it. The game needs a Windows x64 PC with a Vulkan 1.3 driver. The installed build includes the compiled shaders and uses the statically linked MSVC runtime.

| Control | Action |
| --- | --- |
| Left mouse | Paint with the selected material or tool |
| Right mouse | Erase |
| Middle mouse | Stir liquids by dragging |
| Wheel or +/- | Change brush radius |
| 1–9 and 0 | Select one of the ten materials |
| E / G | Toggle the eraser or stir tool |
| Space / N | Pause or resume / advance a single tick while paused |
| R | Restore the current scene |
| F5 / F9 | Save or load the local sandbox slot |
| Esc | Quit |

The scene tabs replace the current canvas. Save before changing scenes to keep your work. The save slot holds the material grid and temperatures at `%LOCALAPPDATA%\FallingSand\world.sandbox`. Loading starts with a still velocity field.

| Material | Behavior |
| --- | --- |
| Sand | Falls into piles, sinks through water and oil, and displaces liquid |
| Water | Flows and pools, suppresses fire, and boils against lava |
| Oil | Flows more slowly than water, rises through it, and burns |
| Stone | Forms watertight barriers and can be produced by quenching lava |
| Wood | Blocks flow and ignites after exposure to sustained heat |
| Fire | Heats its surroundings, ignites fuel, and leaves smoke |
| Smoke | Rises and dissipates |
| Steam | Rises, cools, and can condense into water |
| Lava | Flows slowly, heats nearby materials, and forms a solid crust against water |
| Ice | Blocks flow and melts into water near heat |

## Build and verify

Use CMake 3.28 or later, Visual Studio 2022 Build Tools 17.14 with the C++ workload and Windows SDK, and a Vulkan SDK with `glslc`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\FallingSand.exe
```

```powershell
.\build\Release\FallingSand.exe --test --validate
.\build\Release\FallingSand.exe --window-test --validate
.\build\Release\FallingSand.exe --benchmark 1200 --scene thermal
.\build\Release\FallingSand.exe --capture build\thermal.bmp --after 108 --scene thermal
cmake --install build --config Release --prefix dist/FallingSand
```

`--validate` uses the SDK's Khronos validation and synchronization validation layers. The headless GPU test checks wall velocity, reduction in divergence, phase conservation, sand, thermal reactions, and the brush. The window test covers UI clicks, paint, step, swapchain resize, repeated presentation, and save/load. The benchmark runs one simulation tick per measured frame after a 60-frame warm-up. It selects an uncapped presentation mode when available. Ordinary play runs the simulation at 90 ticks per second and presents with an uncapped mode when the device supports it.

## Engine and simulation

The engine uses Win32 for its single window and the Vulkan C API for device selection, synchronization, compute, dynamic rendering, and presentation. It keeps the world and solver state on the GPU at 384 × 224 cells. Regular frames upload the instanced pixel UI and brush parameters. World transfers occur at scene changes, save/load, and diagnostic readback. Frame resources are double-buffered and presentation semaphores are attached to swapchain images. [Khronos Vulkan Guide on swapchain semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)

The liquid solver advances staggered face velocities with semi-Lagrangian advection, gravity, and phase-dependent viscosity. A variable-density pressure projection uses solid-wall no-through-flow and atmospheric pressure at dry cells. Each pressure workgroup loads a halo and computes two Jacobi iterations in shared memory. Finite-volume transport then moves the three liquid fractions using one shared flux per face. The donor limits outward volume and an iterative receiver limit credits already-accepted outflow, maintaining capacity while preserving phase mass during transport. [Bridson and Müller-Fischer's SIGGRAPH fluid simulation course](https://www.cs.ubc.ca/~rbridson/fluidsimulation/)

This is a real-time 2D approximation. Pressure iterations and transport are bounded, the phases share a velocity field, and immiscible buoyancy, grains, gases, and thermal chemistry use game-scale rules. The fluid shader reconstructs surfaces from the simulated fractions and face velocities and shades them with absorption, highlights, Fresnel reflection, background refraction, and lava emission.

The C++23 host code follows the supplied C++ study guides for RAII, value-oriented types, spans and range views, recoverable errors with `std::expected`, and `std::mdspan` scene layout. It also uses constrained templates, designated initialization, compile-time layout checks, and source-located Vulkan errors. The C++26 proposals covered by the guides are outside the build's language baseline.

