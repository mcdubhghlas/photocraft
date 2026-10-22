# PhotoCraft architecture

> **Target:** Adobe Photoshop 2026. PhotoCraft is being rewritten from Rust to **ISO C17 + POSIX**;
> [`REWRITE_PLAN.md`](../REWRITE_PLAN.md) is the authoritative plan (§2 decisions, §3 module map, §4
> conventions, §7 intentional differences). This document describes the C architecture as it stands.
> The Rust tree under `crates/` and `apps/` remains as the read-only behavioural reference until it is
> deleted.

## Overview

PhotoCraft is a native, portable (Linux, macOS, FreeBSD/OpenBSD) layered image editor. The desktop
app is **SDL3** (window, input, pen tablet, clipboard, file dialogs, drag-and-drop) + **OpenGL 3.3
core** + a vendored **Nuklear** immediate-mode UI. There is no Windows build and no web/WebAssembly
build (REWRITE_PLAN §2 D2). There is no webview, Tauri, Electron or JavaScript anywhere.

The design rests on a few principles that keep it from repeating GIMP's historical mistake of baking
assumptions (8-bit, RGB-only, "a layer is a pixel buffer") into the core:

1. **Engine-first, headless-first.** Every feature is reachable without a GUI: from tests, the CLI,
   the JSON control channel and MCP. The GUI is one client of the engine.
2. **Everything is a command.** Each user-visible action is a command in the engine with a stable id
   (`filter.blur.gaussianBlur`) and JSON params. The menu, command palette, shortcuts, recorded
   actions, CLI, control channel, MCP and plug-ins all dispatch the same commands by id.
3. **Data describes the UI; the toolkit draws it.** Tools produce overlay primitives as data; filter
   and adjustment dialogs are generated from parameter schemas.
4. **No format or colour assumptions.** Bit depth (8/16/32f) and colour model (RGB/Gray/CMYK/Lab/…)
   are runtime data on every surface. No `u8`-only pixel path exists in a public API, and no code
   assumes sRGB — conversions go through `src/cms`.
5. **Copy-on-write tiles.** Pixel data is 256² sparse tiles with atomic reference counts. Snapshots
   and undo cost only the tiles that changed.
6. **Determinism.** Same input, same output across thread counts and between preview and export. The
   `src/base` thread pool (`pc_pool`) combines parallel reductions in a fixed order so results are
   identical with 1 or N threads.
7. **Never crash.** Library code returns `pc_status`/`pc_error`, never aborts (see
   [contributing.md](contributing.md) and `../craftrules/standards/never-crash.md`).
8. **Clean-room.** Implemented from public specs and observed behaviour only.

## Module map

Each former Rust crate maps to one directory under `src/<module>/`, with its public API in
`src/<module>/<module>.h` (or a few headers for large modules) and `static` internal helpers.

```text
src/base/     L0  NEW foundation: status/errors (pc_status, pc_error), checked math + saturating
                  casts (num.h), alloc (mem.h), growable buffers (buf.h), bounded endian reader/writer
                  (bytes.h), file I/O + atomic write (file.h), thread pool (pool.h, pc_pool), logging,
                  JSON wrapper over yyjson (json.h), base64, half floats, hashing/zlib/zstd wrappers
src/geom/     L0  points, rects, affine/perspective, mesh warp
src/cms/      L0  ICC colour management: profiles, curves, CLUTs, pipelines, transforms, intents, BPC
src/color/    L0  colour models, conversions, blend-mode math
src/raster/   L0  pixel formats, COW tiles, surfaces, masks, interrupt flag
src/psd/      L0  PSD/PSB read + write (standalone: base only; publishable)
src/codecs/   L0  flat raster decode/encode (standalone: base only)
src/raw/      L0  camera raw decode + develop (standalone)
src/affinity/ L0  bounded native Affinity reader (standalone, read only)
src/doc/      L1  document model: layer tree, masks, adjustments, effects, smart objects (pure data)
src/ops/      L2  reversible operations + history/undo (holds pc_doc_arc)
src/paint/    L2  brush engine: dabs, dynamics, stroke smoothing, presets
src/algo/     L2  adjustments, filters, selection, inpaint/heal, warp, resampling
src/text/     L2  font DB, shaping + layout (FreeType/HarfBuzz/FriBidi), text-layer raster
src/vector/   L2  shapes, paths, strokes → coverage
src/compose/  L3  CPU compositor (the reference oracle) + the DrawOp plan
src/format/   L3  native .pcraft format (manifest + content-addressed tiles, atomic/incremental saves)
src/gpu/      L3  OpenGL 3.3 compositor (GLSL 330); only src/ui and src/app may use it
src/io/       L4  import/export orchestration; doc ⇄ PSD mapping; SVG/other imports
src/plugins/  L4  sandboxed WebAssembly plug-ins (WAMR; optional, WITH_PLUGINS)
src/engine/   L5  Session + command registry (every action is a command); jobs; prefs
src/automation/ L6 MCP server + JSON-RPC control-protocol handlers (own JSON-RPC, no async runtime)
src/ui/       L6  the Nuklear shell (thin: all actions go through the engine); theme, panels, tools
src/app/      app the desktop program (SDL3 frame loop, control server, startup, crash guard)
src/cli/      app photocraft-cli (convert/info/run/batch/droplet/commands/mcp/serve)
```

Supporting trees:

```text
tests/harness.h        the test framework (one header, no dependencies)
tests/<module>/        test_*.c per module; tests/fixtures/ committed inputs; tests/golden/ oracles
tools/*.sh             corpus fetch, layer check, parity/scorecard/stats/perf generators, goldens
tools/rustref/         TEMPORARY Rust reference dumper (deleted when the Rust tree goes)
third_party/<name>/    vendored libraries: yyjson, nuklear, pocketfft, wamr
```

### Layering (enforced)

```text
 app  src/app · src/cli                       (executables: wire everything together)
 L6   src/ui · src/automation
 L5   src/engine
 L4   src/io · src/plugins
 L3   src/compose · src/format · src/gpu
 L2   src/ops · src/paint · src/algo · src/text · src/vector
 L1   src/doc
 L0   src/base · src/geom · src/cms · src/color · src/raster
      src/psd · src/codecs · src/raw · src/affinity   (standalone: include only src/base)
```

`tools/check-layers.sh` (run first in `make check`) enforces two rules: a file in `src/<module>/`
includes headers only from its own or a lower layer (plus `src/base` for everyone), following the
reference crates' direct dependencies; and GUI headers (SDL, OpenGL, Nuklear) appear only in `src/ui`
and `src/app`. Adding a module or a dependency means updating that table and `REWRITE_PLAN.md` §3
together. `src/gpu` needs OpenGL, so only `src/ui` and `src/app` include it; the headless CLI and
tests use the CPU compositor.

### Dependencies

Own domain code (`cms`, `psd`, `raw`, `affinity`, `compose`, `algo`, `paint`, `vector`, `doc`, `ops`,
`engine`) is ported 1:1 in behaviour, not swapped for third-party libraries, because it must match the
reference and the Photoshop oracles bit for bit. Mature libraries are used at the I/O and platform
edges: libpng, libjpeg-turbo, libtiff(-based codecs), libwebp, OpenEXRCore, libheif, libjxl for
codecs; FreeType/HarfBuzz/FriBidi/libunibreak for text; SDL3 + OpenGL for the GUI; zlib, libzstd,
libblake3 for compression and hashing; WAMR for plug-ins; and the vendored yyjson, Nuklear, pocketfft
and WAMR in `third_party/`. LGPL libraries (FriBidi, libheif) are linked dynamically; FreeType is used
under the FTL. See REWRITE_PLAN §2 (D8–D18, D24–D30) for the exact choices and their reasons.

## The engine / UI seam

The engine (`src/engine`) owns a `Session`: the open documents, the command registry, background jobs
and preferences. A frontend needs only a small, data-oriented surface:

- **Commands in.** The registry (built in `src/engine/command.c` by `register_modules()`, which
  aggregates each module's `pc_engine_<area>_specs()`) exposes, per command: `id`, `label`, `menu`
  path, default `shortcut`, a params doc string, an `enabled` predicate, and `run` (returns a JSON
  result and records exactly one history step). The UI, CLI, control channel and MCP all dispatch the
  same commands by id. `src/ui/menu_catalog.c` holds Photoshop's menu tree; an item whose id matches a
  live command goes live automatically. Only pure view/window state (zoom, panels, screen mode) lives
  in the shell.
- **Snapshots out.** Pixels live in COW tiles, so the UI and background jobs read a document snapshot
  cheaply. Undo swaps tile references; no pixels are copied.
- **Tools.** Pointer events in document coordinates run through the same tool state machines whether
  they come from SDL input or from `ui.pointer` on the control channel, so agents get identical
  results. Tools emit overlay primitives as data for the canvas to draw.
- **View-models and schemas.** Panels read plain structs from a snapshot; parameterised dialogs are
  generated from command params schemas.

The litmus test: `photocraft-cli` and the automation server do everything the GUI does except pointer
painting, which they drive by replaying pointer-event streams.

## Document model (`src/doc`)

`src/doc` is pure data (serialised via `src/doc/json.*`). A document carries its size, resolution,
colour mode (`src/doc/mode.h`: RGB, Gray, CMYK, Lab, Indexed, Bitmap, multichannel/spot), depth
(8/16/32f), an optional embedded ICC profile, a layer tree (`tree.h`), alpha channels, paths, guides,
slices, metadata and the current selection (a raster mask plus optional vector outline).

A layer has name, visibility, locks, blend mode, opacity and fill opacity, clipping, a raster mask and
vector mask, effects (`effects.h`), blend-if (`blend_if.h`) and content. Content is one of raster,
group, adjustment (`adjust.h`), fill (`fill.h`), text (`text.h`, `text_styles.h`), shape (`vector.h`)
or smart object. CMYK and Lab are modelled from the start so PSD round-trips never lose them, even
where rendering converts to RGB. Deep or cyclic trees are walked iteratively with a depth cap (never a
crash).

History lives in `src/ops`: `pc_doc_arc` is a reference-counted document that clones only when shared
(`make_mut`), so a history state costs only the tiles that changed. Each state remembers the layers it
targeted; undo/redo restore that target.

## Rendering

- **CPU compositor (`src/compose`)** is the reference oracle, used for export, tests and the CLI. It
  flattens the layer tree into a linear DrawOp plan (push/pop group, clip base, draw tiles, adjust,
  effect, fill) — the single place that encodes Photoshop's layer semantics (pass-through and clipping
  groups, fill vs opacity, blend-if, knockout). It works per tile on the pool.
- **GPU compositor (`src/gpu`)** runs the same plan on OpenGL 3.3 with GLSL 330 shaders on the app's
  GL context, specialising one fragment program per kernel (one pass per plan step) rather than one
  mega-shader. It must match the CPU oracle within ≤ 1/255 + a small float epsilon. What the planner
  can't express (e.g. advanced blending) is rejected and composited on the CPU, matching the
  reference's own fallback.
- **Colour.** Blending happens in document space by default. The display transform (document profile →
  monitor profile) plus Proof Colors / Gamut Warning / 32-bit preview is baked into a 3D LUT the
  canvas shader applies; document pixels never change.
- **Oracle.** A PSD embeds Photoshop's own merged composite, so running the compositor on a PSD's
  layers and diffing against its embedded composite is a large free conformance suite. The corpus
  tests automate this.

## Native format (`src/format`)

A `.pcraft` file is a ZIP of STORE entries, or a directory bundle with the same layout:
`manifest.json` (the versioned document tree, with migrations), `tiles/<blake3>.zst` and
`blobs/<blake3>.zst` (zstd-compressed, content-addressed objects), and PNG previews. Saving is
incremental: only new objects are written, and unreferenced ones are garbage-collected; writes are
atomic (temp file + `fsync` + `rename`). Autosave and crash recovery reuse the same writer on a
background thread. `src/format/convert_save.c` carries a `_Static_assert` on the sizes of the doc
structs so that adding a `pc_doc` field fails to compile until it is mapped (nothing is silently
dropped from a save); add new fields with `#[serde(default)]`-equivalent defaults so older files load.

## Automation

`src/automation` provides the MCP server and the JSON-RPC control protocol as own code over
stdio/TCP driven by `poll()` — no async runtime (REWRITE_PLAN §2 D14). The MCP framing and the
control envelope (`{id,method,params}` → `{id,ok,result|error}`) match the reference byte for byte.
`src/app/control_server.c` is the desktop TCP transport; `src/ui/shell/control.c` handles the `ui.*`
methods. See [control-protocol.md](control-protocol.md).

Plug-ins (`src/plugins`, optional via `WITH_PLUGINS`) are sandboxed WebAssembly filters run on WAMR
(interpreter mode, instruction + wall-time budgets, bounded memory, no WASI). A section walk enforces
limits before WAMR sees a module. Native `.8BF`/CEP/UXP hosting is out of scope.

## Intentional differences from the reference

Where the C behaviour deliberately differs from the Rust reference — dropped platforms (Windows, web),
library-dependent encoded bytes, error-message wording, never-crash guards on hostile input, and the
deferred macOS menu bar / Wayland portal colour picker — it is recorded line by line in
`REWRITE_PLAN.md` §6 (not ported) and §7 (intentional differences). Consult those sections before
assuming a divergence is a bug.
