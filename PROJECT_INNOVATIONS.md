# FallingSand — Fluid Simulation Innovations & Research Log

> **Purpose:** Durable reference for transport conservation work, failed experiments, open research, and branch lineage.  
> **Primary session:** Wednesday **Nov 18, 2026** (voice + agent work, mostly **~3:17 PM – 5:33 PM UTC-7**).  
> **Repo:** `i:\FallingSand`  
> **Agent transcript:** [`3442c5f3-35e4-44b3-943c-5ebc8c1553f3`](C:\Users\cashg\.cursor\projects\i-FallingSand\agent-transcripts\3442c5f3-35e4-44b3-943c-5ebc8c1553f3\3442c5f3-35e4-44b3-943c-5ebc8c1553f3.jsonl)

---

## Table of contents

1. [Pipeline snapshot](#1-pipeline-snapshot)
2. [Original symptom: bounded cell fill](#2-original-symptom-bounded-cell-fill)
3. [Paired-transfer: vanishing, leaching, towers (first pass)](#3-paired-transfer-vanishing-leaching-towers-first-pass)
4. [Displacement experiments (failed conservation)](#4-displacement-experiments-failed-conservation)
5. [Research roundup & Lundell](#5-research-roundup--lundell)
6. [Branch `mass-transfer-root`](#6-branch-mass-transfer-root)
7. [Standing water towers — diagnosis](#7-standing-water-towers--diagnosis)
8. [Tower spill fix (in progress)](#8-tower-spill-fix-in-progress)
9. [GPU test contract](#9-gpu-test-contract)
10. [Branch lineage](#10-branch-lineage)
11. [References & open implementations](#11-references--open-implementations)
12. [Lessons learned](#12-lessons-learned)

---

## 1. Pipeline snapshot

Per tick (`src/vulkan.cpp`, `fluxLimitPasses = 5`):

`reaction` → `grains` → **`predict`** → `equation` → **`pressure`** (red–black) → **`project`** → **`flux`** → **`limit` × 5** → **`advect`** → `separate` × 2

| Stage | Role for liquids |
|--------|------------------|
| `predict.comp` | Semi-Lagrangian trace + viscosity blend + **+0.085 downward bias on `predicted.y`** |
| `project.comp` | Pressure gradient corrects velocity on **+x / +y faces only** |
| `flux.comp` | Face flux ∝ velocity × volume; `outgoingScale` throttles multi-face export |
| `limit.comp` | Per-cell **acceptance fraction** `limitA` / `limitB` (ping-pong); **pass 0 conservative** |
| `advect.comp` | Debit donor, credit receiver; **max ~1.0 total liquid per cell** (`dot(liquid, vec3(1))`) |
| `separate.comp` | **Phase** swap oil/lava vs water between stacked pairs — **not** bulk gravity fill |

**Cell volume:** `volume(c) = dot(c.liquid, vec3(1))` in `world.glsl`.  
**Air:** `EMPTY` is **not** `solid()` — stone/wood/ice/sand block faces.

---

## 2. Original symptom: bounded cell fill

**When:** Mid-session **Nov 18, 2026** (~5:06 PM+), during limit/transport experiments.

**Test:** `FallingSand.exe --test` → `phaseTotals()` in `src/main.cpp`:

```cpp
require(sum <= 1.00001F, "bounded cell fill");
```

Per-cell sum of water + oil + lava must stay ≤ **1.00001**.

**Typical failure mode:** Inflating how much liquid a cell **accepts** (via `limitA`) without matching **face-equivalent export** causes **overfill** after several transport ticks — not a shader compile error.

**Related failure:** `transport conserves each liquid` — global phase totals must stay within **0.02** of initial totals after ~65 simulation steps.

---

## 3. Paired-transfer: vanishing, leaching, towers (first pass)

**~5:06 – 5:13 PM UTC-7**

### Reported bugs (pre-pairing)

1. **Instant vanish** on sloped stone pours  
2. **Vertical towers** when piling water  
3. **Map leaching** — sideways spread without matching loss at source  

### Root cause (asymmetric transport)

Donor **export** used:

- `outflow = dot(max(-toward, 0), remote)` with **inflated `remote`** (room / displacement boosts on `limitA`)

Receiver **import** used:

- `into = max(toward, 0) * limitA[self]` — **no** matching face budget

Donor debited more than receivers credited → **volume loss**; void neighbors got **teleporting** fluid → **leaching**. Full neighbors kept **low `remote`** → **towers**.

### Fix: `enhanced-fluid-dynamics` branch (~5:09 PM)

**Innovation — paired per-face budgets**

- `faceIn = max(toward,0) * limitA[cell]`
- `faceOut = max(-toward,0) * nbrLimit` (neighbor limits, not boosted remotes)
- **Total export capped:** `totalOut = min(amount, dot(faceOut))`, scale `faceOut` if needed
- **Hydrostatic reweight** (optional): redistribute **which faces** carry export, **not** total volume
- **Solids:** excluded from “void room” displacement tricks
- **`limit.comp`:** `else if (!solid(cell)) result = min(1.0, capacity)` so empty non-solids are not stuck at `limitA = 0`
- **Pass 0:** neighbor `previous()` credit **still disabled** (`pc.operation.x == 0u`)

**Outcome:** GPU tests **passed**; **vanish + leach** fixed; **towers remained** (conservative by design).

**Molasses feel (research ~5:13 PM):** Lower `predict` viscosity helps slightly; **dominant** sluggishness is **limit × 5 + flux-limited spread**, not μ alone.

---

## 4. Displacement experiments (failed conservation)

**~5:06 – 5:07 PM UTC-7** (before / alongside pairing)

Ideas tried:

- Boost horizontal export when under a water column / into empty neighbors  
- Inflate `remote` from neighbor **headroom**  

**Result:** Reintroduced **debit ≠ credit**; slope vanish and leaching returned. Replaced by pairing, not layered on top.

**Lesson:** Any “push sideways” mechanism must appear as **the same scalar on donor `faceOut` and receiver `faceIn`**, not export-weight inflation.

---

## 5. Research roundup & Lundell

**~5:07 – 5:09 PM UTC-7** (research-only + GitHub survey)

### Academic / classic grid fluids

| Source | Relevance |
|--------|-----------|
| [Stable Fluids (Stam 1999)](https://graphics.stanford.edu/courses/cs448-01-spring/papers/stam.pdf) | Divergence-free **velocity**; does not alone conserve **cell scalar volume** if advection is asymmetric |
| [GPU Gems Ch. 38](https://developer.nvidia.com/gpugems/gpugems/part-vi-beyond-triangles/chapter-38-fast-fluid-dynamics-simulation-gpu) | Same family as this project’s predict → pressure → project chain |
| [Lundell thesis (2014, DiVA)](http://www.diva-portal.org/smash/record.jsf?pid=diva2%3A732935) | **Per-cell mass** + **hydrostatic pressure between neighbors** for sandbox games — conceptual match; **no public GPU reference implementation** found in repo surveys |

### Open / community implementations (GitHub & demos)

| Project | Technique | Limit bump? |
|---------|-----------|-------------|
| **[Sandspiel](https://github.com/MaxBittker/sandspiel)** | Explicit pixel moves, down-then-side rules, double buffer | **No** — moves **amounts** |
| **Water-CA / w-shadow / Terraria-style** | `NewMass`, stable stack split, **Δm/4** horizontal | **No** — explicit Δm |
| **[Zylann `fluid2d_demo`](https://github.com/Zylann/godot_mega_tests/tree/master/c++%20gdextension/fluid2d_demo)** (cited in session) | 2D fluid demo patterns | Mass-style updates, not acceptance inflation |
| **Noita / falling-sand CA tutorials** | Priority: down → diagonals → sideways into empties | Explicit moves |

**Key research finding:** None of the closest **open** references fix stacking by **inflating `limitA` / acceptance**. They **transfer explicit mass** with **clamped debit/credit** and **double buffering**.

### Lundell-inspired fill-pressure bump (failed)

On branch `water-only-predict`, **`limit.comp` passes ≥ 2** added a **fill-pressure** block (~lines 53–64 in that revision) that **increased effective capacity** into `limitA`.

**GPU result:** **`bounded cell fill` FAIL** — cells accepted more than one unit over time.

**Decision:** Remove bump; move spreading to **explicit mass transfer** (`mass-transfer-root`).

---

## 6. Branch `mass-transfer-root`

**Created:** **Nov 18, 2026 ~5:22 PM UTC-7** from `water-only-predict`.

### `shaders/limit.comp`

- **Removed** Lundell fill-pressure `capacity += …` inflation.
- **Kept** pass-0 conservatism (no neighbor `previous()` credit when `pc.operation.x == 0u`).
- **Added** `equalizationHeadroom()` — w-shadow / Sandspiel-style **(nbrFill − fill) / 4** per face, **×0.5** Jacobi step, capped so total capacity does not exceed **one cell unit**.
- **Kept** `else if (!solid(cell)) result = min(1.0, capacity)` for **air full headroom** when `incoming == 0`.

### `shaders/advect.comp` (stable tested baseline before tower spill refactor)

**Helpers:**

- `stableFill()` — stacked vertical equilibrium (Water-CA-like).
- `verticalTransfer(donor, recv)` — stable split + headroom clamp.
- `horizontalTransfer(donor, recv)` — **(Δm)/4**, half-step, `kMinFlow`.

**Flux path (pressure-driven):**

- Paired `faceIn` / `faceOut` with `totalOut = min(amount, dot(faceOut))`.

**Mass path (explicit, flux-quiet only in baseline):**

- `cellFluxQuiet()` — mass only when **all** `toward` components == 0.
- **Mutually exclusive** branches: flux **or** mass per tick (not both).
- **Red–black:** `(cellIndex + pc.operation.x) & 1` → **importers vs exporters** per advect (conservation fix; parallel same-pass import+export had failed `transport conserves each liquid`).

**`predict.comp` on lineage:** water μ **0.005**, damp **0.996** (unchanged on this branch).

**GPU (baseline red–black + quiet mass):** **PASS** — RTX 3080, RMS divergence ~0.094 → ~0.028 (reported ~5:22–5:31 PM session).

---

## 7. Standing water towers — diagnosis

**~5:31 – 5:33 PM UTC-7** (investigation; no behavior change until next task)

**User observation:** Full water columns do not spill/cascade into adjacent **empty air** (no friction/obstruction).

**Air is not the blocker:** `solid()` false for air; `limit.comp` gives **full headroom** (`capacity = 1`, `result = min(1, capacity)`).

### Four culprits (code-level)

| # | Mechanism | Location | Effect |
|---|-----------|----------|--------|
| **1** | Mass only if **global flux == 0** | `advect.comp` — `cellFluxQuiet()` + `if (cellFluxQuiet && amount > 0)` | Columns with **residual `flux[]`** after project **never** run explicit mass export |
| **2** | **Flux XOR mass** per tick | `if (any(toward)) { flux } else if (mass) { mass }` | Static columns: **near-zero but nonzero** velocity → flux path active, mass **off**; **~zero `toward` into side air** → **no horizontal spill** |
| **3** | **Red–black + half-step + `kMinFlow = 0.01`** | Export/import parity; `flow *= 0.5`; minimum transfer threshold | At most **half** of cells export mass per advect; small column drainage **stutters** |
| **4** | **No mass export “up”; gravity split** | Mass faces: **down / left / right** only (`massOut.w,x,z`). **Downward** bulk bias in **`predict.comp`** (`predicted.y += 0.085`), **not** a dedicated mass-gravity pass. **No** `massOut` into cell above | Side spill relies on **horizontal** mass or **flux**; vertical mass drain is **downward** only (correct for +y-down grid) |

**Not blocking:** Treating air as non-receiving solid (it isn’t). **Limit** gives air **full acceptance** for inbound flux.

**Coordinate note:** `DOWN = (0, 1)` — **+y is down** on the grid.

---

## 8. Tower spill fix (in progress)

**~5:33 PM UTC-7** — user-requested changes to `advect.comp` on `mass-transfer-root`

### Intended changes

1. **Per-face `faceFluxNegligible()`** — `abs(toward_face) ≤ kFluxEpsilon` (0.028) instead of global exact-zero quiet.  
2. **Combine flux + mass** in one transport block when either moves.  
3. Tune **`kMinFlow` 0.01 → 0.002**; vertical **full step × `kGravityFall` (1.2)** vs horizontal **×0.5** Jacobi.  
4. Keep red–black importers/exporters (conservation).

### Status at doc write

| Check | Result |
|-------|--------|
| Build | **OK** (`sand_shaders`, `falling_sand`) |
| `FallingSand.exe --test` | **FAIL — `bounded cell fill`** (combining flux + mass overfill) |
| Prior red–black + exclusive mass | **PASS** |

**Next engineering directions** (from session, not implemented here):

- Mass on **negligible faces only** with **stricter** cap on `faceIn` headroom, or  
- **Separate advect dispatch** for mass (CA half-step), or  
- **Limit-only** equalization (already partially in `limit.comp`) without parallel flux+mass overfill.

Current working tree `advect.comp` constants: `kMinFlow=0.002`, `kFluxEpsilon=0.028`, `kGravityFall=1.2`.

---

## 9. GPU test contract

From `src/main.cpp` (`simulationTests`):

| Assertion | Threshold |
|-----------|-----------|
| `bounded cell fill` | Per-cell `sum(liquid) ≤ 1.00001` |
| `transport conserves each liquid` | Per-phase total drift **< 0.02** after 65 ticks |
| `pressure projection reduces RMS divergence` | projected RMS **< 50%** of predictor |
| (others) | Solids retained, sand grain count, lava falls, oil above water |

Command: `.\build\Release\FallingSand.exe --test`

---

## 10. Branch lineage

```text
main
 └── enhanced-fluid-dynamics   (paired faceIn/faceOut; ~5:09 PM)
      └── water-only-predict   (μ 0.005, damp 0.996; Lundell bump tried → FAIL)
           └── mass-transfer-root   (explicit mass + limit equalization; ~5:22 PM)
```

Git note: branch `Enhanced Fluid Dynamics` was created as **`enhanced-fluid-dynamics`** (no spaces).

---

## 11. References & open implementations

- Stam — Stable Fluids  
- NVIDIA GPU Gems — fast grid fluids  
- Lundell — DiVA 2014 sandbox water thesis (theory, no public shader port)  
- Sandspiel — explicit cellular moves  
- Water-CA / w-shadow — stableFill, NewMass, quarter horizontal spread  
- Zylann fluid2d_demo — referenced as open 2D mass/flux pattern  
- jgallant / GlennCr CA tutorials — down-first, overflow rules (session notes)

---

## 12. Lessons learned

1. **`limitA` is an acceptance fraction, not mass.** Inflating it (Lundell bump) **breaks** `bounded cell fill` without paired export.  
2. **Conservation needs matched face scalars** — donor `faceOut` and receiver `faceIn` must reflect the **same** flow; `remote` boosts without `into` boosts cause vanish/leach.  
3. **Parallel advect + simultaneous import/export** on both cells failed `transport conserves`; **red–black import/export split** passed.  
4. **Towers ≠ viscosity-only** — pass-0 / full-neighbor **limit choke** + **flux-quiet mass gating** dominate visual “molasses” more than μ in `predict`.  
5. **Open sand/water CA wins on explicit Δm** — research-aligned path is **mass transfer + caps**, not limit inflation.  
6. **Combining flux + mass in one parallel pass** is **conservation-hard** — latest spill relaxations **regressed** bounded fill; treat as active R&D.

---

## Document history

| Date | Event |
|------|--------|
| **2026-11-18 ~5:33+ PM UTC-7** | Tower diagnosis + spill fix attempt (`bounded cell fill` fail) |
| **2026-11-18 ~5:22 PM** | `mass-transfer-root` shipped; GPU pass (red–black baseline) |
| **2026-11-18 ~5:09–5:13 PM** | Paired transfer + molasses research |
| **2026-11-18 ~5:06–5:07 PM** | Displacement bugs + research roundup |
| **2026-11-18 afternoon** | Bounded fill failures on limit inflation / pass-0 credit bugs |

*This file is a living log. Update when branches merge or GPU status changes.*