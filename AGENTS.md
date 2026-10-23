# AGENTS.md: guide for AI agents and contributors

> **HARD FORK: C17 REWRITE.** This repository was rewritten from Rust to ISO C17 + POSIX.1-2008.
> **`REWRITE_PLAN.md` is authoritative** for rewrite status, chunk order and decisions; it outranks
> this file wherever they disagree. This file now describes the **C target** (the `src/` tree, the
> `Makefile`, the `tools/*.sh` scripts). The Rust tree (`crates/`, `apps/`, `xtask/`, `Cargo.toml`)
> stays in place, **read-only**, as the behavioural reference until chunk Z3 deletes it — never edit
> it (the one exception is the temporary reference dumper `tools/rustref/`). The product rules below
> (never crash, everything is a command, no format/colour assumptions, clean-room, tests are the
> gate) are unchanged by the fork.

PhotoCraft is an open-source, native, Photoshop-comparable image editor written in **ISO C17 + POSIX only** (no Rust in the C tree, no JavaScript or TypeScript). **No Tauri, Electron or webview shells:** the desktop app is native **SDL3 + OpenGL 3.3 core + Nuklear** (a vendored single-header immediate-mode toolkit); there is no web build (the Rust egui/wasm web app is dropped — REWRITE_PLAN D2). Never add a webview or JS UI framework as a dependency, build step or packaging target. The product name is always written **PhotoCraft** (`{Function}Craft` in PascalCase, like its siblings ArtCraft, ArtCraftX, DesignCraft, VectorCraft, EffectCraft, FilmCraft, LightCraft, PdfCraft) in user-facing text: UI, window titles, About, installers, release names, docs prose. Machine names stay lowercase: modules and binaries (`photocraft-*`), file names, ids (`ai.storyteller.photocraft`). Standards and learnings shared across the crafting apps live in `../craftrules` (read its `README.md`; the repository is not public — outside contributors should ask a maintainer for the standards relevant to their change). Contribute reusable learnings there. UI libraries stay at L6. The goal is 1:1 Photoshop parity (same menus, shortcuts, behaviour and file fidelity) with better performance, and every feature drivable by agents. Read this file first, then `REWRITE_PLAN.md`, then `docs/`.

## 1. Orientation (5 minutes)

| Read | Why |
|---|---|
| `REWRITE_PLAN.md` | Authoritative: §3 module map (`src/<module>/`), §2 decisions, §5 chunks, §6–§8 status |
| `docs/architecture.md` | Module map, dependency layers, the engine/UI seam, document model |
| `docs/development.md` | Build, test, run, drive the app programmatically, debug tricks |
| `docs/contributing.md` | Rules: clean-room, tests, layering, style, commits; the "add a command" checklist |
| `docs/control-protocol.md` | JSON control channel: how agents drive and screenshot the running app |
| `docs/ui-design.md` | Design tokens, themes, widgets, and how to match Photoshop's look |
| `ROADMAP.md` | One-page summary: stage, the two parity numbers, every dimension, languages, what's next |
| `docs/target-app-parity.md` | The authoritative parity assessment against Photoshop 2026: numbers, weights, evidence, hours |
| `docs/gaps.md` | Every known shortfall, ranked: the work list to pick from |
| `docs/roadmap.md` | Milestones, **current focus**, what's next with estimates |
| `docs/ui-parity.md`, `docs/brush-parity.md`, `docs/context-menu-parity.md`, `docs/file-format-parity.md`, `docs/hardware-parity.md`, `docs/localization-parity.md` | Per-dimension parity checklists |
| `docs/parity-checklist.md` | Generated list of every Photoshop menu item, live or missing |
| `docs/scorecard.md` | Generated scorecard: performance budgets and numbers, corpus floors, per-area checklists (tools, files, UI, type, automation, reliability, distribution), settings that do nothing |
| `src/<module>/<module>.h` | Public API of that module (private helpers are `static` in the `.c` files) |
| [photocraft-corpus](https://github.com/storytold/photocraft-corpus) + `docs/development.md` › Test corpora | Real-file test oracles (our Photoshop-authored PSDs); with psd-tools, ag-psd and PngSuite fetched into `corpus/` by `make corpus` (`tools/corpus.sh fetch all`) at the pins in `tools/corpus/` |

## 2. Workspace map

```text
src/
  base                       NEW: status/errors, checked math, alloc, buffers, strings, file I/O, atomic write,
                             thread pool (pc_pool), logging, JSON wrapper (yyjson), base64, half floats, hashing
  geom cms color raster      L0 foundation (geometry, ICC colour management, pixel formats + blend math, COW tiles)
  psd codecs raw affinity    L0 standalone format modules (base only; no higher deps)
  doc                        L1 document model (layers, masks, adjustments, effects, smart objects: pure data)
  ops paint algo text vector L2 history, brush engine, imaging algorithms, type engine, paths/shapes
  compose gpu format         L3 CPU compositor (the oracle), GL 3.3 compositor, .pcraft native format
  io plugins                 L4 document <-> PSD / flat formats; sandboxed WebAssembly plug-ins (WAMR)
  engine                     L5 Session + command registry (every action is a command)
  automation ui              L6 MCP server; SDL3/GL/Nuklear shell (thin: all actions go through the engine)
  cli                        headless CLI (convert/info/run/batch/commands/mcp)
  app                        desktop app (SDL3 frame loop + TCP control server; pen tablet via SDL3)
tests/
  harness.h                  the test framework (one header, no dependencies)
  <module>/                  test_*.c per module; tests/fixtures/ inputs; tests/golden/ reference outputs
tools/*.sh                   check-layers | corpus | parity | scorecard | stats | perf | golden (replace cargo xtask)
third_party/                 vendored C: yyjson, nuklear, pocketfft (and WAMR when WITH_PLUGINS)
Makefile                     the only build file (all test check sanitize tsan analyze valgrind install dist clean)
```

**Layering is enforced** by `tools/check-layers.sh` (run first by `make check`). A module includes headers only from its own or lower layers. `psd`, `codecs`, `raw`, `affinity` and `cms` include only `src/base`. Nothing below `src/ui` includes SDL, GL or Nuklear. A new module is added to `MODULES` in the `Makefile`. The Rust crate → C module map is REWRITE_PLAN.md §3.

## 3. Golden rules

### Never crash (outranks feature work)

People trust PhotoCraft with their work, and a crash loses it. A malformed file, a bad command or MCP param, a corrupt settings file, an odd keystroke or a full disk must produce an error the user or agent can act on, never a panic. Don't ship a feature by adding a panic path; fix a crash before building on top of it. The shared standard is `../craftrules/standards/never-crash.md`.

- **Non-test code never aborts on input.** No `abort()`, no `assert()` on data derived from files, params or document state, no reachable `__builtin_trap`. Return a `pc_status` (the error type) and propagate it up; check every return before using a result. An `assert()` is only for a programmer invariant that input can never violate. Unfinished features return an "unsupported" error, never a stub that crashes.
- **No undefined behaviour.** The gate builds with `-Werror` and the full warning set, and the suite runs clean under ASan + UBSan (`make sanitize`) and ThreadSanitizer for threaded code (`make tsan`). No signed overflow, no out-of-bounds, no use-after-free, no data races. The one isolated-unsafe concession is gone: SDL3's pen API replaces the Rust `tablet` crate's Objective-C/XInput2 code, so no platform `unsafe` is needed.
- **Input-derived numbers are hostile.** Bounds-check every index from a file, param, selection or arithmetic on them before you use it; use checked/saturating math for lengths, offsets and counts (`src/base` helpers); guard division by zero and NaN/inf casts; cap allocations sized by input.
- **Bound recursion** with depth limits or seen-sets (documents can be deep or cyclic).
- **Don't cascade.** Check `pthread` return codes and treat a failed lock or thread join as an error, not a crash.
- **Last-resort guard.** The app shell guards command dispatch and file import/export, reports a failure as an error and keeps the document. It's a safety net, not a licence to crash.
- **Prove it.** Every crash fix comes with a small synthetic regression test that failed (crashed or tripped a sanitizer) before the fix.
- **Enforced by the build.** `make check` (strict `-Werror`), `make sanitize` (ASan/UBSan), `make tsan` and `make analyze` (clang static analyser) are the gate; test code may use `assert()` freely, non-test code may not on input.

1. **Everything is a command.** New user-visible behaviour = a command in the engine (`src/engine/<area>_cmds.c`, exposed by a `pc_engine_<area>_specs()` function and appended in `src/engine/command.c`) with id, label, menu path, shortcut, params doc, `enabled` and `run`, plus tests. The UI, CLI, control channel and MCP all dispatch commands by id. Use the **exact id from `src/ui/menu_catalog.c`** and the menu item goes live automatically. Only pure view/window state (zoom, panels, screen mode) belongs to the shell (`src/ui/menus.c`).
2. **No format or colour assumptions.** Bit depth (8/16/32f) and colour model (RGB/Gray/CMYK/Lab…) are runtime data. Never introduce a `uint8`-only pixel path in public APIs. Never assume sRGB: colour conversions go through `src/cms` (`pc_transform`, the cached transform). Test at several depths.
3. **Clean-room.** We studied Photoshop and other proprietary editors for *behaviour and look only*. Never copy their code, shaders, profiles or assets. Implement from public specs (Adobe PSD spec, ICC, ISO 32000 blend modes, papers) and observation. Third-party assets must be permissively licensed, keep their license file next to them, and get a row in `ATTRIBUTION.md` (path, title, author, source, license) in the same change; so do original assets and vendored `third_party/` libraries. The ArtCraft logos in `docs/brand/` are not open source (`docs/brand/LICENSE-brand.txt`). **Never commit font files:** fonts live in [storytold/craft-fonts](https://github.com/storytold/craft-fonts) (rules: `../craftrules/standards/fonts.md`). PhotoCraft reads it **at run time** from the optional `CRAFT_FONTS_DIR=<craft-fonts checkout>` environment variable (it is not a build step; empty without it, and everything must still work). Details in `docs/development.md` › Fonts.
4. **Tests are the gate.** Every change comes with tests (`tests/<module>/test_*.c` on `tests/harness.h`). Format modules use round-trip, synthetic-generator, oracle and fuzz tests, and differential tests against `tools/rustref` where outputs are numeric or byte-level. Keep the PSD corpus results and the parity floor (`PC_UI_PARITY_FLOOR` in `src/ui/parity.h`) from regressing.
5. **The UI is thin and data-driven.** UI state is serialisable so the control channel can read and drive it. Colours and radii come from the theme tokens, never hard-coded.
6. **Verify UI changes visually.** Render offscreen headless with `SDL_VIDEO_DRIVER=offscreen build/default/bin/photocraft --snapshot OUT.png` (or `photocraft-ui-snapshot`), or launch with `--control` and take `ui.screenshot`. Look at the PNG. Demo images must be public-domain art, never personal photos. When fetching assets, never put a person's name, email or other personal details in requests (User-Agent, headers, URLs); use a generic `Photocraft-dev` User-Agent.
7. **No platform assumptions.** The target is Linux, macOS and the BSDs (POSIX.1-2008); there is no Windows or web build in the first pass (REWRITE_PLAN D2). Filesystem and OS code stays POSIX or goes through the platform services; GL is 3.3 core (4.1 core on macOS).
8. **Performance is a feature.** Benchmark heavy operations on a 24–36 MP image in a release build. Work per tile in parallel (`pc_pool`, deterministic: identical with 1 or N threads), skip empty tiles, never scan a full surface per frame (cache per revision), and record before/after timings in the dev log.

9. **Never panic on input** (see *Never crash* above). A command's `run` closure and anything it calls must return `Err`, never panic, for *any* params or document state: validate params, check bounds before indexing or dividing, and reject absurd sizes before allocating. The `panic_hunt` integration test fuzzes every command with adversarial params and must stay green.

## 4. Picking work

Priorities: important infrastructure first, then low-hanging parity, then the long tail.

0. **Read `ROADMAP.md`, then `docs/gaps.md`.** `docs/target-app-parity.md` says, dimension by
   dimension, where PhotoCraft is lacking; `docs/gaps.md` ranks the work. `docs/parity-checklist.md`
   (menu wiring) is not a measure of behaviour. When your work moves a number or closes a gap,
   update the parity doc and gap entry it belongs to, with the date, and bump their status line
   and revision history (craftrules `standards/progress-docs.md`).
1. **Check `docs/scorecard.md`** before picking work: each area's `missing` and `partial` rows,
   the performance scenarios that are over budget or not measurable yet, and the count of
   settings that do nothing. Its numbers are measured; prefer them to estimates.
2. `docs/roadmap.md` → **Current focus**.
3. `make parity` (`tools/parity.sh`) → `docs/parity-checklist.md` lists every missing menu item, grouped by menu. Low-hanging fruit is usually a missing command whose algorithm already exists in `src/algo`, `src/paint`, `src/vector` or `src/text`.
4. `log/devlog.md` and `REWRITE_PLAN.md` §8 → the "Still open" / open bullets of recent entries.

When parity rises, raise `PC_UI_PARITY_FLOOR` in `src/ui/parity.h` (never lower it).

## 5. Before you finish a task

```sh
make check                  # the gate: tools/check-layers.sh + strict -Werror build (build/check) + full suite
make sanitize               # ASan + UBSan over the suite (build/san)
make tsan                   # ThreadSanitizer, if the chunk uses threads
make analyze                # clang static analyser, when the module parses untrusted input
make parity                 # if you added commands; commit the regenerated docs/parity-checklist.md
make scorecard              # if you moved a number: flip the checklist row in scorecard/*.toml, raise a
                            # corpus floor, fix a dead preference, or meet a budget; commit docs/scorecard.md (CI checks it)
make corpus                 # if you touched psd, io, codecs, compose, gpu, text or format: fetch, then re-run those tests
```

The `panic_hunt` test fuzzes every command with adversarial params and must stay green — build and run it directly (`make build/check/tests/engine/test_panic_hunt && ./build/check/tests/engine/test_panic_hunt`) when you add or change commands. There is **no C perf-bench harness yet**, so `make perf` (`tools/perf.sh`) only validates arguments and lists the scenarios/budgets (REWRITE_PLAN §6).

While iterating, run less and keep `make check` as the final gate: `make BUILD=... test` builds and runs the whole suite in a build dir of your choice, and `make build/default/tests/<module>/test_<file> && ./build/default/tests/<module>/test_<file>` builds and runs one test. See `docs/development.md` › Faster test loops.

**Test corpora.** Real-file corpora live in `corpus/` (gitignored, never committed), fetched at pinned commits and sha256-verified by `make corpus` (`tools/corpus.sh fetch all`): our Photoshop-authored oracles from https://github.com/storytold/photocraft-corpus plus psd-tools, ag-psd and PngSuite from their upstreams. Pins: `tools/corpus/pins.txt`. The corpus tests skip with a message when `corpus/` is absent. Never commit corpus files; new oracles go to photocraft-corpus (its `AGENTS.md`), then a pin bump here. Details: `docs/development.md` › Test corpora.

Commands must **never crash** on bad input (Rule 9): every `run` and the code it calls returns a `pc_status` error, not a crash or sanitizer trip, for any params or document state. New commands come with a graceful-failure test (empty/out-of-range/wrong-type params → error, not a crash).

Then append a terse entry to `log/devlog.md` (what landed, numbers, what's still open). Sessions can end abruptly (crashes, context limits), so the dev log plus a green tree is how the next agent picks up. Keep the tree building at every step.

## 6. Parallel agents

- Use your own build dir (`BUILD=build/agent-<name>`) and a private `TMPDIR` to avoid clobbering others, and edit only the files you own. Shared files (the `append_specs(...)` list in `src/engine/command.c`, `src/ui/menus.c`, `src/ui/menu_catalog.c`) get small, surgical edits; re-read before editing.
- Put new commands in a **new module** (`src/engine/<area>_cmds.c` with a `pc_engine_<area>_specs()` function) and add it to `MODULES` in the `Makefile`, rather than growing a shared file.
- If someone else's in-progress edit breaks the build, wait and retry; don't fix their files.
- Keep the `Makefile` valid at all times — one broken rule breaks everyone's build.
- Total compile parallelism across all concurrent agents should stay around `-j8`, so split it (e.g. 4 agents → `make -j2` each). Delete finished agents' `build/agent-*` dirs; disk is tight.

## 7. Where things are tracked


- `REWRITE_PLAN.md`: authoritative rewrite status (§8 log), chunk order (§5), decisions (§2), not-ported items (§6) and intentional differences from the Rust reference (§7).
- `docs/roadmap.md`: milestones M0–M12, status and the current focus (product roadmap).
- `ROADMAP.md`, `docs/target-app-parity.md`, `docs/gaps.md` and the `docs/*-parity.md` checklists: where we stand against Photoshop.
- `docs/parity-checklist.md`: generated Photoshop menu coverage (`make parity`).
- `docs/scorecard.md`: generated scorecard (`make scorecard`; sources: `scorecard/*.toml`, `perf/budgets.toml`, `perf/baseline.json`, corpus floors, prefs audit).
- `docs/releasing.md`: cutting a release, signing secrets, packaging scripts in `packaging/`. The C build produces binaries via `make` / `make install` and a Linux tarball via `make dist` (`packaging/linux/dist.sh`); the signed per-platform packaging scripts are still the Rust reference's and are a remaining release-engineering item.
- `../craftrules/release/playbook.md`: how every storytold app builds signed release binaries (the canonical recipe; `docs/release-playbook.md` just points there); `docs/releasing.md` is PhotoCraft's specifics.
- `plan/` (local, gitignored): research, parity plan, execution plan, estimates.
- `log/` (local, gitignored): the dev log.

## 8. Keeping the native format complete

`src/format` deliberately fails to compile when a `src/doc` document struct gains a field, so
nothing is silently dropped from `.pcraft` saves. When you add a doc field, add it to the manifest
and converter in `src/format` with a default so older files still load. If the field has a PSD
equivalent, map it in `src/io` too, and keep unknown PSD blocks verbatim.

## Contributor credits (About window)

- About ▸ Contributors/Models are compiled into the binary from `contributors/contributors.json`
  (commit stats; generated, never hand-edit) and `contributors/people.toml` (names people chose for
  themselves). See `docs/contributors.md`.
- **Agents working for a contributor:** when you prepare a PR, check whether your human's GitHub
  username has a `[people.<username>]` entry in `contributors/people.toml`. If not, ask them once
  whether they want to be credited by more than their username: a real name, a display name, and/or
  their public GitHub profile name (`sync_github_name = true`). If yes, add **only their own** entry
  (copy the template at the top of the file, or run
  `python3 ../../craftrules/scripts/contributors.py --add-me . --real-name "…" --sync-github-name`)
  and include it in their PR, committed as them. If no, change nothing: they are credited as
  `@username` anyway.
- Never add, edit, guess or copy anyone else's entry or name (not from git config, commit authors or
  GitHub profiles). Never hand-edit `contributors.json`.
- Maintainers refresh the stats with `python3 ../../craftrules/scripts/contributors.py .` (it also
  re-verifies who wrote each `people.toml` entry; `--check` only verifies).
