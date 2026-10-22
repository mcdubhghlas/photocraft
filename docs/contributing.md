# Contributing

PhotoCraft is in a hard fork from Rust to **ISO C17 + POSIX.1-2008**. For all new work,
[`REWRITE_PLAN.md`](../REWRITE_PLAN.md) is authoritative (its §2 decisions, §3 module map and §4 C
conventions in particular). The Rust tree under `crates/` and `apps/` stays as the read-only
behavioural reference until it is deleted; never edit it except the temporary reference dumper
(`tools/rustref/`).

- **Language:** ISO C17 (`-std=c17`, `_POSIX_C_SOURCE=200809L`), no GNU extensions. No JavaScript or
  TypeScript, no Tauri/Electron/webview. The GUI is SDL3 + OpenGL 3.3 + a vendored Nuklear; there is
  no web/WebAssembly build.
- **Licence:** contributions are MIT OR Apache-2.0 (`LICENSE-MIT`, `LICENSE-APACHE`, `NOTICE`). The
  ArtCraft logos in `docs/brand/` are not open source (`docs/brand/LICENSE-brand.txt`).
- **Clean-room:** do not copy code, shaders, icons, ICC profiles or other assets from proprietary
  software (such as Photoshop). Match behaviour and look by observation and public specs (the Adobe
  PSD spec, ICC, ISO 32000 blend modes, papers). Only use assets with permissive licences, keep the
  licence next to the asset (e.g. `assets/fonts/OFL-*.txt`, `assets/icons/LICENSE-lucide.txt`), and
  list every asset in `ATTRIBUTION.md`. Vendored libraries live in `third_party/<name>/` with their
  LICENSE and an `ATTRIBUTION.md` row. **Fonts** live in
  [storytold/craft-fonts](https://github.com/storytold/craft-fonts), never in this repo; build with
  them through the optional `CRAFT_FONTS_DIR` input (`docs/development.md` › Fonts; rules in
  `../craftrules/standards/fonts.md`, a sibling checkout — `storytold/craftrules` is not public, so
  ask a maintainer for the rules relevant to your change).

## Never crash (outranks feature work)

Library code must never terminate the process: no `abort`, `exit`, `assert`, `panic` path, division
by zero, out-of-bounds access, or unchecked allocation on input-derived sizes. The shared standard is
`../craftrules/standards/never-crash.md`.

- **Fallible functions return `pc_status`** (`PC_OK == 0`, else `PC_ERR_INVALID`, `PC_ERR_NOMEM`,
  `PC_ERR_IO`, `PC_ERR_FORMAT`, `PC_ERR_UNSUPPORTED`, `PC_ERR_LIMIT`, `PC_ERR_CANCELLED`, …) and take
  an optional trailing `pc_error *err` (may be `NULL`) that receives the code, `errno` if relevant,
  and a fixed `char[256]` message (no allocation). Propagate with the `pc_fail`/`pc_fail_errno`
  macros. Out-params are written **only on success**. Error messages that tests or the control
  protocol expose must match the reference text.
- **Input-derived numbers are hostile.** Check bounds before indexing (`get`-style accessors, not raw
  `[i]`); use `pc_size_mul`/`pc_size_add` and `checked`/`saturating` helpers for lengths, offsets and
  counts; cap allocations sized by input (`PC_MAX_ALLOC`, pixel/dimension limits); guard division by
  zero; use the saturating float→int casts (`pc_f32_to_u8`, …) from `src/base/num.h` (Rust `as`
  saturates, C `as` is UB).
- **Bound recursion** with depth limits or seen-sets (documents can be deep or cyclic). Treat lock
  and thread-join failures as errors, never crashes.
- **No `assert` on input.** `assert` is for provably-impossible internal invariants only, and must be
  compiled out of shipping behaviour. Unfinished features return `PC_ERR_UNSUPPORTED`.
- **Prove it.** Every crash fix ships with a small synthetic regression test that failed before it.
  The engine's `panic_hunt` test fuzzes every command with adversarial params and must stay green.

## Rules

- **Warnings are errors.** `make check` builds with
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes` plus `-Werror`, and must be
  clean. `make sanitize` (ASan + UBSan) and, for threaded code, `make tsan` must be green too; run
  `make analyze` (clang static analyser) when the module parses untrusted input.
- **Layering** is enforced by `tools/check-layers.sh` (run first in `make check`): a module in
  `src/<module>/` includes headers only from its own or a lower layer (plus `src/base` for everyone),
  and SDL/OpenGL/Nuklear headers appear only in `src/ui` and `src/app`. Adding a module or a
  dependency is a design change: update the table in `tools/check-layers.sh` and `REWRITE_PLAN.md` §3
  together.
- **No format or colour assumptions.** Bit depth (8/16/32f) and colour model (RGB/Gray/CMYK/Lab…) are
  runtime data. Never introduce a `u8`-only pixel path in a public API; colour conversions go through
  `src/cms`. Test at several depths.
- **Commands, not handlers:** new user-visible behaviour is an engine command with tests, and the UI
  dispatches it by id (checklist below).
- **Tests are the gate.** Every change comes with tests. Format code needs round-trip,
  synthetic-generator, oracle and malformed-input tests; pixel code is tested at 8, 16 and 32-bit. If
  you touch `psd`, `io`, `codecs`, `compose`, `gpu`, `text` or `format`, run the real-file corpus
  tests (`tools/corpus.sh fetch all`, then `make check`). Never commit corpus files; see
  `docs/development.md` › Test corpora. Keep the PSD corpus results and the parity floor
  (`PC_UI_PARITY_FLOOR` in `src/ui/parity.h`) from regressing.
- **UI is thin and data-driven.** UI state lives in `src/ui/state.*` (serialised to JSON) so the
  control channel can read and drive it. Colours and radii come from theme tokens
  (`pc_ui_tokens`, `src/ui/theme.h`), never hard-coded.
- **Verify UI changes visually** with an offscreen snapshot (`photocraft-ui-snapshot`, or
  `photocraft --snapshot OUT.png` under `SDL_VIDEO_DRIVER=offscreen`), or with `ui.screenshot` on a
  `--control` session. Demo images must be public-domain art.

## C conventions (REWRITE_PLAN §4)

- **Naming.** Public functions and types `pc_<module>_<name>`; enum constants `PC_<MODULE>_<NAME>`;
  macros `PC_*`. Opaque types `typedef struct pc_doc pc_doc;` where invariants matter; plain structs
  for plain data. Internal helpers are `static`. Public API in `src/<module>/<module>.h`.
- **Memory ownership.** `pc_x_new(…, pc_x **out, pc_error *err)` / `pc_x_free(pc_x *x)` (which accepts
  `NULL`); `_init`/`_fini` for stack use. A returned `const T *` is borrowed; say so in the header.
  Byte input is `(const uint8_t *data, size_t len)` — never trust a terminating NUL in file data.
  `realloc` only through `pc_grow`. Reference-counted sharing uses C11 `<stdatomic.h>`.
- **Numeric parity.** Keep `f32` vs `f64` exactly as the reference has them; never `-ffast-math`; use
  the `float` math functions and the rmath helpers (`pc_rs_minf`/`pc_rs_maxf`, `pc_cbrtf`, …) where
  the reference's codegen matters. Byte order is always explicit (`pc_rd_be32`, `pc_wr_le16`); never
  `memcpy` a struct to/from a file. The numeric gotchas in REWRITE_PLAN §4 are required reading
  before porting any pixel code.
- **Style.** 4-space indent, ≤ 110 columns (`.clang-format`). Files stay under ~1,500 lines; split by
  responsibility. Comments explain *why*; headers document purpose, failure modes, ownership and
  thread safety.
- **Commits:** small and focused; message `C17 <chunk-id>: <summary>`. Keep the tree building
  (`make check`) at every commit. Append a dated line to the progress log in `REWRITE_PLAN.md` §8 and
  record intentional behaviour differences from the reference in §7.

## Adding a command

1. **Find the id.** Search `src/ui/menu_catalog.c` for the Photoshop menu item. Using its exact id
   makes the menu item live with no extra UI work. A command without a Photoshop menu entry uses a
   descriptive id in the same style (`layer.smartFilter.delete`) and an empty menu path.
2. **Algorithm** goes in the lowest module that fits (`src/algo` for imaging, `src/paint`,
   `src/vector`, `src/text`, `src/cms`), with unit tests. It takes depth-agnostic surfaces
   (`src/raster`), works per tile, and is deterministic (seeded randomness; identical results with 1
   or N pool threads).
3. **Command** goes in an engine module (`src/engine/<area>_cmds.c` exposing a
   `pc_engine_<area>_specs()` function, registered in `register_modules()` in
   `src/engine/command.c`). Prefer a **new** module over growing a shared file. Fill
   in: `id`, `label`, `menu` path and Photoshop's default `shortcut`; a params doc string such as
   `{"radius":px=4,"mode":"a|b"="a"}` (agents read this through `commands`/`command_list`); an
   `enabled` predicate; and `run`, which returns a JSON result and records exactly **one** history
   step (use coalescing for drags and typing sessions).
4. **Never panic on input.** The `run` closure and everything it calls must return `Err` for any
   params or document state — validate params, check bounds, reject absurd sizes before allocating.
   Add a graceful-failure test (empty / out-of-range / wrong-type params → error, not a crash); the
   `panic_hunt` test must stay green.
5. **Respect the context:** active selection (feathered), layer vs mask target, locks, the colour
   model and depth.
6. **Tests** in the module: behaviour, undo/redo, disabled states, bad params, several depths.
7. **Dialog** (if it has parameters): filters and adjustments get schema-driven dialogs with live
   preview (`src/ui/filter_dialog.c`, `src/ui/shell/dialogs.c`).
8. Regenerate the menu checklist with `make parity` and commit the updated
   `docs/parity-checklist.md`. When parity rises, raise `FLOOR` in `src/ui/parity.c` (never lower it).
