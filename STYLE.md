# FallingSand coding style

This guide records conventions used in `shaders/grains.comp`. Treat that file as the reference implementation when matching style in new or edited code.

## Naming (no single-letter variables)

**Do not use single-letter variable names.** Every variable must state its purpose in the name. Readability beats brevity; longer names are not only acceptable but preferred when they make the code easier to follow in a teaching codebase.

| Avoid | Prefer (examples) |
|--------|-------------------|
| `ivec2 p` | `gridPos`, `cellCoord`, `tileOrigin` — whatever matches the role (grid position, offset, anchor, etc.) |
| `int i` | `cellIndex`, `flatIndex`, `bufferIndex` |
| `Cell c` | `cell`, `centerCell`, `neighborCell` — qualify when several cells are in scope |

The same rule applies to other one-letter locals (`n`, `x`, `a` in a 2×2 block, and so on): rename to something a reader can understand without hunting through ten lines of context. Reserve very short names for loop indices only when the loop is trivial **and** the project style for that file already documents an exception; default is still descriptive names.

## Plain English (required)

FallingSand is a **teaching tool for making sand games**. Comments are for someone who may never have written a shader before. **Avoid jargon and code-speak** in comments (not in the code itself):

- Prefer everyday words: “grid square,” “brush stroke,” “what was already in the cell” — not “dispatch,” “payload,” “bail,” or “publish to the buffer” unless you immediately explain them in plain words.
- Do **not** write comments as API documentation (`dot(...)`, “read-only snapshot,” “velocity channel”). Say what the math **means** (“how far along the brush line,” “stop water from sloshing here”).
- Short game-design analogies are welcome (sand piles, blowing smoke, eraser clears the spot).
- When a technical term is unavoidable, gloss it in the same sentence (“shared memory — fast scratch space the whole thread group can see”).

Readability for learners beats sounding like a graphics textbook.

## Audience and tone

Comments assume a reader learning how falling-sand and GPU simulation work. Use plain language, short sentences, and occasional game-design analogies (neighbor swaps, Noita-style gas). Prefer **teaching** over exhaustive line-by-line narration. Follow **Plain English (required)** above on every new or revised comment.

## Comments

### File and function headers

- Start a translation unit with a **two-line** `//` block that states what the pass does and why the structure matters (for example, 2×2 blocks and thread locality).
- Leave a **blank line** after `#include` lines before the next comment or code, when the include block ends.
- Before each function, add a **short block** (one or two `//` lines) describing purpose and important parameters. Use backticks for names that appear in code (`upper`, `lower`, `matter`, `pc.tick`).

### Section headers inside functions

- Mark phases with a label and one line of intent, for example `// Sand: vertical fall, then diagonal trickle when the cell straight below is solid.`
- Put section headers **above** the code they introduce, not between unrelated lines.

### In-scope (block-level) comments

- Explain **non-obvious logic** inside `{ ... }`: swaps, snapshots, write-back, diagonal paths. Skip lines that are already obvious from the code.
- Place a **blank line immediately above** each full-line `//` comment that follows executable code or another logical chunk within the same scope.
- **Exception:** at the **start of a scope**, a comment may sit directly under the opening `{` with **no** blank line above it (the `{` is enough separation). This applies to the first line inside a function, `if`, `for`, or any other braced block.
- Everywhere else inside the scope, a full-line `//` comment still needs a blank line above it when it follows code.

### Local logical lumps (required)

Comment **every local logical lump** of code that does something meaningful but is not self-evident to a beginner—even **two or three lines** that work together (look up a grid index and read the cell, build a stroke test, fill one liquid channel, and similar steps).

- Place **one short plain-English** full-line `//` comment **immediately above** the lump (same blank-line rules as other in-scope comments).
- Say **what the lump accomplishes** in game/simulation terms, not how the syntax works.
- Skip only lumps that are already obvious from the names and structure (for example, a single `return` right after a clearly labeled guard).
- Prefer a lump comment over scattering half-explanations across several lines; do not narrate every statement.

This project is a teaching tool: when in doubt, add the lump comment.

### Inline end-of-line comments

- Put **magic numbers**, thresholds, and terse constant notes on the **same line** as the code they qualify, using `literal: short note` (`0.32: upper mostly empty`, `97987u: salt constant for random01`).
- Use inline `//` for **short** clarifications on calls and conditions (`+1u: separate roll per fall attempt`, `+2u: separate roll for this diagonal spill`).
- For **dense formulas** (bit masks, `dot`, `any`, coordinate packing), add a **separate full-line** comment immediately above the statement when a single end-of-line note would be too long.
- In **shader** files (`.comp`, `.vert`, `.frag`, and `world.glsl`), that technical line must use the **`// Implementation:`** prefix and pair with an unlabeled plain-English line (see **dual-layer** below).

### Shader files: dual-layer comments (plain English + `Implementation:`)

Shaders teach falling-sand logic **and** GPU math. When a lump has non-obvious arithmetic, use **two layers**:

1. **Plain English** (first, **no prefix**): simple, unlabeled `//` text — what this step means for the game/simulation — per **Plain English (required)** and **Local logical lumps (required)**.
2. **`Implementation:`** (optional line immediately after the plain line, or alone when only the formula needs explaining): dense or complicated technical notes — bit masks, `dot`, indexing, interpolation, coordinate transforms.

Prefix **only** the technical line with `// Implementation:` so readers can tell teaching text from formula text at a glance. Do not label plain-English comments with `detail:`, `computation:`, `math:`, or any other tag.

**`Implementation:` is full-line only** — never on end-of-line comments. Same-line notes stay short (`0.32: …`, `+1u: …`, `slip cap; …`); put formula-heavy explanation on a full line above (plain English first, then `// Implementation:`).

Do not replace plain-English comments with jargon-only notes; **both** layers are expected where the math is hard.

**End-of-line:** magic numbers and terse roll offsets may stay on the code line (`// 0.32: upper mostly empty`, `// +1u: separate roll per fall attempt`); put formula-heavy full-line notes on the line above (or below the plain line) with the `// Implementation:` prefix.

### Multi-line comments

- Prefer **one full-line `//` per line**; wrap only when a sentence does not fit reasonably on one line.
- **Do not** break a comment so the second line is only one or two words (for example `// neighbors.` or `// to zero`). Join that fragment onto the previous line instead.
- Parenthetical clarifications may stay on a second line when they carry a full phrase, for example `(checkerboard stagger) to reduce same-cell races ...`.

### Comments on braces

- A short note may sit on the same line as an opening brace when it explains the block body: `{ // full swap may move liquid`.

## Indentation

- Use **hard tab** characters for block indent, **one tab per indent level** (equivalent to four spaces in width).
- Do **not** indent with leading spaces for block levels.
- **Wrapped continuation lines** may use tabs for the nesting depth, then **spaces** to align with an earlier token on the continued expression (see multi-line `ivec2` initializers and `if` conditions in `grains.comp`).

### Indentation consistency

Every line’s **leading indent must match its surrounding block**. Sibling statements share the same tab depth; bodies of `if`, `for`, and functions gain **one extra tab** per nesting level.

- If a line looks **out of step** with its neighbors (for example an `if` at the same depth as the statement above it that should be its parent, or a body line at the wrong depth), **fix the leading whitespace** — do not change code or comments.
- The rule applies to **continuation lines** too: tabs reflect nesting depth; trailing spaces after tabs only align wrapped tokens, not extra logical levels.
- When editing a file, scan edited regions for stray tabs or accidental space-based block indent and align before committing.

## Braces (Allman)

- Place the **opening `{` on its own line** after a function signature, `if`, `for`, or similar control statement.
- Place the **closing `}` on its own line** at the same tab level as the matching opening construct.
- A single-statement body may omit braces only when the style already does so for clarity—e.g. a guarded `return` on the line after `if`, or a one-line `fall(...)` under each of several sibling `if` conditions—matching existing patterns in `grains.comp`.
- Several **sibling** `if` statements with braceless bodies each sit at the **same tab depth** as their parent block (not nested inside the previous `if` unless that nesting is intentional logic). See **Indentation consistency** and the sibling-`if` example below.

## Spacing and readability

- Put a **blank line** between major steps in `main`: anchor setup, cell reads, sand pass, gas pass, write-back loop.
- Use spaces around `=` in **layout** and similar declarations where it aids reading: `layout(local_size_x = 16, local_size_y = 8)`.
- Keep related reads and flags grouped; separate groups with a single blank line.
- Code content (identifiers, literals, operators) is unchanged by style rules—only layout, braces, tabs, and comments are prescribed here.

## What not to do

- Do not add line-by-line commentary on every statement.
- Do not change behavior or magic values when reformatting; document them inline instead.
- Do not reintroduce **space-based** block indentation in files that follow this guide.

## Examples

Minimal patterns taken from `shaders/grains.comp`. (`→` marks a tab character in prose below; snippets use real tabs.)

### Section header (blank line above)

```glsl
Cell bottomRight = readCell(anchor + RIGHT + DOWN);

// Sand: vertical fall, then diagonal trickle when the cell straight below is solid.
fall(topLeft, bottomLeft, random01(seed));
```

### First comment inside a scope (no blank line above)

```glsl
if (material(upper) == SAND && !solid(lower) && chance < mobility)
{
	// Falling-sand games usually swap neighbors: sand moves down, whatever was below moves up.
	Cell swapCell = upper;
```

### Later in-scope comment (blank line required above)

```glsl
		upper = lower;

		// Next step in the same block still gets a blank line above the comment.
		lower = swapCell;
```

### Inline one-liner on a magic number

```glsl
if (gas(material(lower)) && material(upper) == EMPTY && volume(upper) < 0.32) // 0.32: upper mostly empty
```

### Shader dual-layer: plain English, then `Implementation:`

```glsl
	// Skip this thread when its 2×2 patch would start past the edge of the world.
	// Implementation: tick&1u is tick mod 2 on x; (tick>>1u)&1u is the next bit on y; any(greaterThanEqual(anchor, pc.size)) is out of bounds
	if (any(greaterThanEqual(anchor, pc.size)))
		return;
```

```glsl
	// Sand slips more on dry ground; liquid below makes grains less willing to move.
	// Implementation: dot(lower.liquid, vec3(...)) weights water, oil, lava; 0.90 minus that sum is mobility
	float mobility = 0.90 - dot(lower.liquid, vec3(0.19, 0.39, 0.72)); // slip cap; water / oil / lava weights
```

### Collapsed comment (avoid a second line with only one or two words)

Prefer:

```glsl
// Each GPU thread block is 16×8 threads — enough parallelism without tiny 2×2 blocks fighting neighbors.
```

Not:

```glsl
// Each GPU thread block is 16×8 threads — enough parallelism without tiny 2×2 blocks fighting
// neighbors.
```

### Allman braces with tab indentation

```glsl
void rise(inout Cell lower, inout Cell upper)
{
	if (gas(material(lower)) && material(upper) == EMPTY && volume(upper) < 0.32) // 0.32: upper mostly empty
	{
		uint displaced = upper.matter;
	}
}
```

### Continuation alignment (tabs, then spaces to column)

```glsl
	ivec2 anchor = ivec2(gl_GlobalInvocationID.xy) * 2 - // one invocation covers 2×2 cells
				   ivec2(int(pc.tick & 1u), int((pc.tick >> 1u) & 1u)); // tick phases stagger x,y
```

### Shader dual-layer: random seed lump

```glsl
	// Tie random rolls to this patch and frame so sand jitters naturally instead of marching in lockstep.
	// Implementation: indexOf(anchor) is row-major flat index; ^ (pc.tick * 97987u) stirs the hash each tick
	uint seed = uint(indexOf(anchor)) ^ (pc.tick * 97987u); // 97987u: salt constant for random01
```

### Related calls: separate random rolls (end-of-line, not `Implementation:`)

```glsl
	fall(topLeft, bottomLeft, random01(seed));
	fall(topRight, bottomRight, random01(seed + 1u)); // +1u: separate roll per fall attempt
```

### Sibling braceless `if` statements (indentation consistency)

Each condition is independent; both `if` headers share the same tab depth inside `main`, with a one-tab-indented body under each:

```glsl
	// Blocked straight down? Try diagonal spill like grains sliding off a pile.
	if (sandTopLeft && solid(bottomLeft))
		fall(topLeft, bottomRight, random01(seed + 2u)); // +2u: separate roll for this diagonal spill
	if (sandTopRight && solid(bottomRight))
		fall(topRight, bottomLeft, random01(seed + 3u)); // +3u: separate roll for the right-column diagonal
```

### Comment on an opening brace

```glsl
		if (inside(gridPos) && (updated[cornerSlot].matter != original[cornerSlot].matter ||
		                        any(notEqual(updated[cornerSlot].liquid, original[cornerSlot].liquid))))
		{ // full swap may move liquid
```

## Reference

When in doubt, match `shaders/grains.comp` for comment density, tab indents, Allman braces, and section structure.