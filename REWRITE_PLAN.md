# REWRITE_PLAN.md: PhotoCraft hard fork, Rust → C17

**This file is the single source of truth for the rewrite.** If you are an agent (or a person)
picking this up, read §0–§4, find the first `[ ]` chunk in §5 whose dependencies are `[x]`, and do
it. Nothing else in the repo outranks this file for rewrite work: where `AGENTS.md`, `docs/` or
`../craftrules` say "Rust only", "commands live in `crates/engine`", "run `cargo xtask …`", they
describe the **reference implementation** we are replacing, not the target.

Status legend: `[ ]` not started · `[-]` in progress · `[x]` completed.
A `[-]` line carries `(who, YYYY-MM-DD)`; a `[x]` line carries the date it was finished.

## 0. Goal and scope (decided, don't relitigate)

- **Hard fork.** PhotoCraft is rewritten in ISO C17 + POSIX.1-2008. Upstream rules and maintainer
  expectations no longer apply. The Rust tree stays in place, untouched, as the **behavioural
  reference** until chunk Z3 deletes it. Never edit Rust code except the temporary reference
  dumper (`tools/rustref/`, chunk B4).
- **Feature parity with the Rust app** on Linux, macOS and the BSDs: same commands (ids, params,
  behaviour), same file fidelity (PSD, TIFF, PNG, JPEG, WebP, EXR, HEIF, RAW, Affinity, SVG, `.pcraft`),
  same control protocol and MCP server, same CLI.
- **Dependencies:** "only what is necessary", not "none". Mature C libraries are used for codecs,
  text, windowing and GPU (§2). The project's own domain code (PSD, RAW, CMS, compositor,
  algorithms, engine) is ported, not replaced, because behaviour must match the reference.
- **Work is chunked** (§5). One chunk ≈ one focused session, 1k–12k Rust lines. Chunks are ordered by
  dependency: everything a chunk needs is above it.

## 1. How to do a chunk (the loop)

1. Pick the first `[ ]` chunk whose inputs are done. Change it to `[-] (your-name, date)` and commit
   that one-line change alone if other agents are working in parallel.
2. Read the listed Rust files **and** their tests (`crates/<crate>/tests/`, `#[cfg(test)]` modules,
   `*_tests.rs`). The tests are the behavioural contract. Read the crate's `README.md` if present.
3. Write the C module under `src/<module>/` (map in §3). Public API in `src/<module>/<module>.h`
   (or a few headers if the module is large), private helpers `static` in `.c` files.
4. Port the tests to `tests/<module>/test_*.c`. Every Rust test either gets a C equivalent or a
   line in §7 saying why not. Add differential tests against `tools/rustref` where outputs are
   numeric or byte-level (pixels, encoded files, JSON).
5. `make check` (build with strict warnings + all tests) and `make sanitize` (ASan + UBSan) must be
   green; `make tsan` too if the chunk uses threads. Run `make analyze` when the module parses untrusted input.
6. Flip the line to `[x] (date)`, add a dated line to §8 (what landed, test counts, deviations,
   anything left open), and record intentional behaviour differences in §7.
7. Keep the tree building at every commit. Commit message: `C17 <chunk-id>: <summary>`.

**If a chunk turns out too big,** split it in this file (`E10a`, `E10b`, …) before starting, keeping
the file list exact. **If you find a missing dependency** between chunks, reorder here and say why
in §8. **If you have to make a design decision** not covered by §2–§4, add it to §2 with a one-line
reason; later chunks follow it.

## 2. Decisions

| # | Decision | Reason |
|---|---|---|
| D1 | ISO C17 (`-std=c17`), `_POSIX_C_SOURCE=200809L`. No GNU extensions. | Portability; requested. |
| D2 | Platforms: Linux, macOS, FreeBSD/OpenBSD. **Windows and the web (wasm) build are dropped.** | POSIX target; the web build was egui+wasm-bindgen, which has no C equivalent worth the cost. Revisit Windows via MinGW after Z4 if wanted. |
| D3 | Build: one POSIX `Makefile` with `pkg-config`. Targets `all test check sanitize analyze install clean`. Optional features as make variables: `WITH_HEIF`, `WITH_EXR`, `WITH_PLUGINS`, `WITH_GUI` (default 1 when the library is found). | Requested; no CMake. |
| D4 | GUI: **SDL3** (window, input, pen tablet, clipboard, file dialogs, drag-and-drop, locale) + **OpenGL 3.3 core** + **Nuklear** (vendored single-header immediate-mode toolkit). | egui is immediate-mode; Nuklear is the closest C equivalent, so the 92k-line UI ports screen by screen. SDL3's pen API replaces the `unsafe` tablet crate, and its dialogs replace `rfd`. |
| D5 | GPU compositor: GLSL 330 shaders on the same GL context. The CPU compositor stays the oracle. | Replaces wgpu/WGSL; GL 3.3 runs on macOS (4.1 core), Mesa and the BSDs. |
| D6 | Parallelism: own `pc_pool` (pthreads, parallel-for over tile/row ranges). | Replaces rayon. Must be deterministic: results identical with 1 or N threads. |
| D7 | JSON: **yyjson** 0.13.0, vendored in `third_party/yyjson/` (changed from cJSON in B2), wrapped by `src/base/json.*` for serde_json parity: exact u64/i64/f64 distinction, serde_json float text (shortest digits, zmij layout: `1.0`, `1e+20`, `1e-7`; f32 values via `yyjson_mut_float`), `Value` key sorting (`PC_JSON_SORT_KEYS`), last-duplicate-wins lookup, depth limit 127, errors "… at line L column C". Modules use yyjson's API directly for building/walking. | cJSON stores every number as a double: u64 ids above 2^53 and the int/float distinction (`as_u64` on `1.0`) would be lost. yyjson keeps both and has an exact shortest float printer. |
| D8 | Own domain code is ported 1:1 in behaviour: `cms`, `psd`, `raw`, `affinity`, `compose`, `algo`, `paint`, `vector`, `doc`, `ops`, `engine`. Not swapped for lcms2/libraw/etc. | Exact numeric parity with the reference and the Photoshop oracles. |
| D9 | Codecs use libraries: libpng, libjpeg-turbo, libtiff, libwebp, OpenEXRCore (C API of OpenEXR ≥ 3.1), libheif. The in-tree VP8 encoder (`codecs/vp8/`) is **not** ported: libwebp encodes lossy WebP. | Mature, fuzzed decoders for untrusted files. Byte output of encoders may differ; pixels must round-trip (§7). |
| D10 | Text: FreeType (outlines, metrics), HarfBuzz (shaping), FriBidi (bidi), libunibreak (line breaking). | Replaces parley/skrifa/swash. No ICU: dictionary line breaking for Thai/Lao/Khmer/Burmese/Japanese falls back to UAX #14 rules (§7). |
| D11 | FFT: **pocketfft** (C, BSD-3, vendored). FFTW is GPL and incompatible with MIT/Apache. | Replaces rustfft. |
| D12 | SVG import: **plutosvg + plutovg** (C, MIT, vendored). | Replaces usvg/resvg. librsvg would drag in glib, cairo and Rust. |
| D13 | Plug-ins: **WAMR** (wasm-micro-runtime, interpreter mode, Apache-2.0 with LLVM exception), optional via `WITH_PLUGINS`. | Replaces wasmi. Same sandbox: no WASI, bounded memory, fuel/time limits. |
| D14 | MCP server and control channel: own JSON-RPC over stdio/TCP with `poll()`; no async runtime. | Replaces tokio + rmcp. The protocol is small. |
| D15 | Hashing/compression: libblake3, zlib (deflate + crc32), libzstd. base64 and IEEE half floats are small internal helpers in `src/base/`. | Replaces blake3, flate2, crc32fast, ruzstd, base64, half. |
| D16 | Workspace sandboxing (cap-std) → `openat`/`O_NOFOLLOW`/`O_DIRECTORY` relative to a directory fd. | POSIX equivalent of capability-based paths. |
| D17 | Vendored code lives in `third_party/<name>/` with its LICENSE, and gets an `ATTRIBUTION.md` row. Only for libraries that aren't commonly packaged (yyjson, Nuklear, pocketfft, plutosvg/plutovg, optionally WAMR). | Clean-room and licence hygiene carry over. |
| D18 | LGPL libraries (FriBidi, libheif) are linked dynamically only. FreeType is used under the FTL (credit in About + README). | Licence compatibility with MIT OR Apache-2.0. |
| D19 | The macOS native menu bar (`muda`), Apple-event file opening, and Wayland portal colour picking are dropped for the first pass; the in-window menu bar is used everywhere. | Need Objective-C or D-Bus; not C17/POSIX. Listed in §7; can return as isolated platform files later. |
| D20 | The build requires **GNU make ≥ 3.81** (`gmake` on the BSDs; macOS ships 3.81). Use only 3.81 features: no `$(file)`, `!=`, `.ONESHELL`. | POSIX make has no pattern rules or `wildcard`; out-of-tree builds of hundreds of files need them. |
| D21 | XSI interfaces are allowed per file: `#define _XOPEN_SOURCE 700` as the first line, with a comment naming the function (e.g. `realpath` in `src/base/file.c`). Everything else stays at `_POSIX_C_SOURCE=200809L`. | glibc hides XSI functions under plain POSIX, even ones Linux/macOS/BSD all have. |
| D22 | Floating point must behave like Rust: `FLT_EVAL_METHOD == 0` is enforced in `src/base/status.h` (32-bit x86 needs `-msse2 -mfpmath=sse`), and the Makefile passes `-ffp-contract=off` (no FMA fusion; clang allows it by default, which changes results on aarch64). Never use `-ffast-math`. | Found by the 32-bit build in B2 follow-up: x87 excess precision changed a result. Rust never contracts or widens. |
| D23 | GUI snapshots and GUI tests run headless with `SDL_VIDEO_DRIVER=offscreen` (EGL + Mesa); `photocraft-spike --snapshot OUT.png [--scale S]` is the pattern U1's snapshot tool follows. Linux builds of SDL3 need X11 and/or Wayland support compiled in (Gentoo: `USE="X wayland dbus"` on media-libs/libsdl3) for interactive use; dbus lets file dialogs use the desktop portal. | Proven in B3: no display or focus stealing needed to verify rendering, and the same works in CI. |
| D24 | Codecs: the formats the reference took from the `image` crate (GIF, BMP, TGA, ICO, QOI, Radiance HDR) are implemented in `src/codecs/` from their public specifications. The GIF encoder quantises with its own median cut (the reference used NeuQuant via the `gif` crate). | No commonly packaged C library covers them all; each is small. Pixels round-trip as the tests require; encoded bytes differ (§7). |
| D25 | Codecs: TIFF is own code, not libtiff (changes D9): `tiff_ifd` (the bounds-checked directory walker) is ported as is, and the chunk codecs are the ones the reference's `tiff` build had (none, LZW, Deflate, PackBits; horizontal and floating-point predictors) for reading and writing. | The tests pin behaviour defined by that walker (pages and SubIFDs, cut-off chunks decode with a warning, hostile counts refused before allocation, Photoshop's private tags verbatim); libtiff's own structure checks, error handlers and directory model would differ, and the codecs are a few hundred lines. |
| D26 | Codecs: PNG pixels are decoded by libpng with every ancillary chunk but tRNS ignored; iCCP, eXIf, pHYs, text and APNG frame counts come from our own chunk walk (as the reference does for text). PNG writing is own code (the reference's adaptive filter, parallel deflate bands, Adam7). | libpng validates ICC profiles on read and write and drops profiles the reference round-trips verbatim; its chunk memory caps would also differ from the reference's text budget. |
| D27 | Codecs: flat OpenEXR goes through OpenEXRCore (C API; link `-lOpenEXRCore-*` only); deep parts are read by the ported walker over the raw bytes, with an own header parser, because the reference reads them that way (NONE/RLE/ZIPS) and its tests pin those errors. | Keeps deep-EXR behaviour and messages identical while flat files use the maintained library. |

## 3. Layout and module map

```text
Makefile                 the only build file (D3)
src/base/                NEW: status/errors, checked math, alloc, buffers, strings, file I/O, atomic write,
                         thread pool, logging, JSON wrapper, base64, half floats, hashing/compression wrappers
src/<module>/            one directory per Rust crate (table below); public header src/<module>/<module>.h
src/cli/  src/app/       photocraft-cli and the desktop app (executables)
tests/harness.h          the test framework (one header, no dependencies)
tests/<module>/          test_*.c per module; tests/fixtures/ small committed inputs; tests/golden/ reference outputs
tools/rustref/           TEMPORARY Rust reference dumper (deleted in Z3)
tools/*.sh               corpus fetch, layer check, parity/scorecard generators (Z1)
third_party/<name>/      vendored libraries (D17)
```

| Rust crate | C module | Layer | Rust crate | C module | Layer |
|---|---|---|---|---|---|
| (std glue) | `src/base` | L0 | `doc` | `src/doc` | L1 |
| `geom` | `src/geom` | L0 | `ops` `paint` `algo` `text` `vector` | `src/ops` `src/paint` `src/algo` `src/text` `src/vector` | L2 |
| `cms` | `src/cms` | L0 | `compose` `format` | `src/compose` `src/format` | L3 |
| `color` | `src/color` | L0 | `gpu` | `src/gpu` | L3 (needs GL; only `src/ui` and `src/app` use it) |
| `raster` | `src/raster` | L0 | `io` `plugins` | `src/io` `src/plugins` | L4 |
| `psd` | `src/psd` | L0 (standalone: base only) | `engine` | `src/engine` | L5 |
| `codecs` + `heif` | `src/codecs` | L0 (standalone: base only) | `automation` | `src/automation` | L6 |
| `raw` | `src/raw` | L0 (standalone) | `ui-egui` | `src/ui` | L6 |
| `affinity` | `src/affinity` | L0 (standalone) | `apps/photocraft` + `tablet` | `src/app` | app |
| `testkit` | `tests/harness.h`, `tools/perf` | test | `apps/photocraft-cli` | `src/cli` | app |

**Layering rule (same as Rust):** a module includes headers only from its own or lower layers; `psd`,
`codecs`, `raw`, `affinity`, `cms` include only `src/base`. Nothing below `src/ui` includes SDL,
GL or Nuklear. `tools/check-layers.sh` (B4) enforces it in `make check`.

## 4. C conventions (every chunk follows these; B1 implements the helpers)

**Naming.** Public functions and types `pc_<module>_<name>` (`pc_psd_read`, `pc_doc_layer_add`);
enum constants `PC_<MODULE>_<NAME>`; macros `PC_*`. Opaque types `typedef struct pc_doc pc_doc;`
when invariants matter; plain structs for plain data. Internal functions are `static`.

**Errors.** Fallible functions return `pc_status` (`PC_OK == 0`; `PC_ERR_INVALID`, `PC_ERR_NOMEM`,
`PC_ERR_IO`, `PC_ERR_FORMAT`, `PC_ERR_UNSUPPORTED`, `PC_ERR_LIMIT`, `PC_ERR_CANCELLED`, …) and take an
optional last parameter `pc_error *err` (may be `NULL`) that receives the code, `errno` if relevant,
and a message (fixed `char[256]`, no allocation). Rust `Result<T, E>` → status + out-param.
`pc_fail(err, code, fmt, ...)` and `pc_fail_errno(...)` are macros that evaluate to `code` (so the
clang analyser can follow error returns); pass a constant code, and write `(void)pc_fail(...)` when
using one as a statement. Out-params
are written **only on success**. Rust error *messages* that tests or the control protocol expose must
match the reference text.

**Never crash** (carries over from AGENTS.md, it outranks features). Library code never calls
`abort`, `exit`, `assert` or anything that can terminate the process. Every Rust code path that
returns `Err` returns a status; every Rust test that checks "no panic on bad input" becomes a C test
that checks the status, and runs under ASan/UBSan. The engine's `panic_hunt` fuzz test is ported in E12.

**Memory ownership.**
- `pc_x_new(…, pc_x **out, pc_error *err)` / `pc_x_free(pc_x *x)`; `free(NULL)`-style: `_free`
  accepts `NULL`. Objects with internal buffers have `_init`/`_fini` for stack use.
- A returned `const T *` is **borrowed** and valid until the owner is mutated or freed; say so in the
  header. A returned non-const pointer that the caller must free says "caller frees with `pc_x_free`"
  (or `free()` for plain strings and buffers).
- Byte input is `(const uint8_t *data, size_t len)`; never trust a terminating NUL in file data.
- Sizes are `size_t`, computed with `pc_size_mul`/`pc_size_add` (overflow → `PC_ERR_LIMIT`).
  Allocations sized by input are capped (`PC_MAX_ALLOC`, pixel and dimension limits as in Rust).
- `realloc` only through `pc_grow` (keeps the old pointer on failure). Functions that build several
  resources use a single `cleanup:` label.
- Rust `Arc` / COW tiles → explicit reference counts with C11 `<stdatomic.h>` (`__STDC_NO_ATOMICS__`
  is a build error, not a fallback). Rust `Rc<RefCell<>>` patterns → single owner + borrowed pointers.

**Numeric parity gotchas (read before porting any pixel code).**
- Rust `as` float→int casts **saturate** and map NaN→0; in C that's UB. Always use
  `pc_f32_to_u8`/`pc_f32_to_u16`/`pc_f32_to_i32`… (saturating, NaN→0) from `src/base/num.h`.
- Rust integer `+ - *` in release wraps (or panics in debug); C signed overflow is UB. Use unsigned
  types or `pc_*_checked`/`wrapping` helpers where the Rust code relies on either.
- Keep `f32` vs `f64` exactly as Rust has them; no `-ffast-math`; use `float` math functions
  (`sqrtf`, `powf`) where Rust calls `f32` methods. `f32::round` = `roundf` (half away from zero);
  `f32::max/min` = `fmaxf/fminf` (NaN-ignoring); `clamp` = hand-written helper (same NaN behaviour
  as Rust: NaN stays NaN).
- Reductions in parallel must combine in the same order regardless of thread count (D6).
- Byte order is always explicit (`pc_rd_be32`, `pc_wr_le16`, …); never `memcpy` a struct to/from a file.
- Rust's float `Iterator::sum` starts from **−0.0**, not +0.0 (an empty or all-−0.0 sum stays −0.0).
  Start C accumulators that port `.sum()` at `-0.0`.
- LLVM rewrites `powf`/`pow` with a constant exponent: `x.powf(0.5)` → `fabs(sqrt(x))`,
  `x.powf(2.0)` → `x * x`. Write those forms explicitly; glibc's `pow` gives different last bits.
- Rust std does not use the C library for every maths function: `cbrt`/`cbrtf` are its own
  (CORE-MATH / FreeBSD). Use `pc_cbrt`/`pc_cbrtf` from `src/base/rmath.h`. Functions Rust *does*
  import from libm (`pow`, `sin`, `cos`, `atan2`, `hypot`, `tanh`, …) match glibc as is. When a hash
  sweep differs in the last bits, check `nm -D` on the rustref binary to see which functions it imports.
- `a.min(b)` / `a.max(b)` → `pc_rs_minf`/`pc_rs_maxf` (rmath.h), not `fminf`/`fmaxf`: on x86-64 rustc
  returns the FIRST operand when they compare equal (0.0 vs −0.0), but when the second operand is a
  literal constant LLVM emits a bare minss/maxss and the SECOND wins (`(-0.0).max(0.0)` is +0.0): use
  `pc_rs_minf_k`/`pc_rs_maxf_k` there. Only the sign of zero differs, but it shows in hash sweeps.
- `x.powi(n)` with constant `x` is folded by LLVM as `(float)pow((double)x, n)`, so `(6.0f32/29.0).powi(3)`
  is one ulp above `x*x*x`. Hard-code such constants from rustref output (see `src/color/convert.c`).

**Style.** C17, 4-space indent, ≤ 110 columns (`.clang-format` from B1). Files stay under ~1,500
lines; split by responsibility. Comments explain *why*; headers document purpose, failure modes,
ownership and thread safety. Strict warnings are errors in `make check`
(`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes`, plus `-Werror` only there).

**Tests.** `tests/harness.h` gives `CHECK`, `CHECK_EQ_INT` (signed), `CHECK_EQ_UINT` (unsigned),
`CHECK_NEAR`, `CHECK_EQ_STR`, `CHECK_EQ_MEM`, `CHECK_CONTAINS` and `RUN(test_fn)`; see its header
comment. Every `tests/<module>/test_*.c` is its own executable (one translation unit, linked
against `libphotocraft.a`); `make test` builds and runs them all. Code that uses threads must also
pass `make tsan`. Corpus
tests (real PSDs etc.) are opt-in: `make corpus` (= `tools/corpus.sh fetch all`) puts the pinned,
sha256-verified files in `corpus/`; corpus tests skip cleanly when it is absent.

**Reference outputs (while the Rust tree exists).** `tools/rustref` (Cargo, Rust 1.95 via its
`rust-toolchain.toml`) prints the reference's view of a file: `psd-info`, `decode`, `composite`,
`vectors`. `tools/golden.sh CMD PATH...` records them as committed JSON in `tests/golden/<cmd>/`
(pixel outputs as `raw_sha256`); C tests compare against those. Generate the goldens a chunk
needs *before* Z3. Existing: psd-info for corpus/psd, psd-tools, photoshop (737 files); decode for
corpus/pngsuite (178).

## 5. Chunks

### Phase 0: bootstrap

- [x] **B0** · inventory, decisions, this plan (2026-10-09)
- [x] **B1** (2026-10-09) · build system and `src/base` → `Makefile`, `src/base/`, `tests/harness.h`, `.clang-format`
  - `Makefile` with every target in D3, pkg-config detection, `-std=c17`, strict warnings in `check`,
    `sanitize` (ASan+UBSan, feature-detected), `analyze` (clang `--analyze` or cppcheck if present).
  - `src/base`: `status.h` (pc_status, pc_error), `num.h` (checked size math, saturating casts,
    clamp/round helpers per §4), `mem.h` (pc_alloc/pc_calloc_array/pc_grow, PC_MAX_ALLOC), `buf.h`
    (growable byte buffer, string builder), `bytes.h` (bounded big/little-endian reader/writer cursor),
    `file.h` (bounded read of a whole file, EINTR/partial-I/O-safe read/write loops, atomic write via
    temp file + `fsync` + `rename`), `pool.h` (D6), `log.h`.
  - Tests for every helper, including overflow, partial I/O (pipe/socketpair), EINTR, and the
    "out-param untouched on failure" contract.
- [x] **B2** (2026-10-09) · `src/base` wrappers: JSON (yyjson, D7), base64, half floats, blake3, zlib deflate/inflate +
  crc32, zstd decode → `src/base/`. Bounded output sizes; tests with malformed and truncated input.
- [-] **B3** (claude, 2026-10-09; X11 window/widgets/typing verified by the user; waiting on mouse-draw + Open… check; pen deferred to P2, no tablet attached) · GUI stack spike (de-risks D4/D5 before anything depends on it): `src/ui/spike/` opens an
  SDL3 window with a GL 3.3 context, draws Nuklear widgets with FreeType-rendered text at HiDPI,
  reads pen pressure from SDL3, opens an SDL3 file dialog. Linux verified; record the results for macOS
  and the BSDs, or note "untested". If the stack fails a requirement, update D4 before U1.
- [x] **B4** (2026-10-09) · tooling → `tools/`
  - `tools/corpus.sh fetch|verify`: the corpus pins and sha256s from `xtask/src/corpus_pins.rs`, using `curl` and `sha256sum`/`shasum`.
  - `tools/check-layers.sh`: the §3 include rules, run by `make check`.
  - `tools/rustref/` (seeded in B2: a standalone Cargo project printing serde_json/half/base64 vectors
    for the `src/base` tests; turn it into subcommands here): a temporary Cargo binary that links the reference crates and dumps
    machine-readable outputs (decoded pixels as raw + JSON metadata, composites, command results) for
    differential tests. Starts with `psd-info`, `decode`, `composite`; later chunks add subcommands.
  - `tools/golden.sh`: regenerates `tests/golden/` from `tools/rustref` (needs Cargo; run only while
    the Rust tree exists, outputs are committed).

### Phase 1: L0 foundation and standalone formats (~38,154 lines)

Needs B1 (all) and B2 (psd, codecs, affinity). F6–F17 are independent of F1–F5 and can run in parallel with them.

- [x] **F1** (2026-10-09) · geom: points, rects, affine, mesh warp · ~1,019 lines → `src/geom/`
  - `crates/geom/src/`: lib.rs, warp.rs
- [x] **F3** (2026-10-09) · cms (1/2): ICC parsing, curves, CLUTs, pipelines, transforms · ~2,705 lines → `src/cms/`
  - `crates/cms/src/`: clut.rs, curve.rs, gamut.rs, lib.rs, math.rs, pipeline.rs, profile.rs, transform.rs
- [x] **F4** (2026-10-09) · cms (2/2): built-in profiles, synthesis, ICC writing, LUT files · ~1,693 lines → `src/cms/`
  - `crates/cms/src/`: builtin.rs, lut3d.rs, lutfile.rs, synth.rs, write.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/cms/tests/`: bench.rs, cms.rs, regen.rs
- [x] **F2** (2026-10-09) · color: colour models, conversions, blend-mode math · ~1,115 lines → `src/color/`
  - `crates/color/src/`: blend.rs, convert.rs, lib.rs
- [x] **F5** (2026-10-09) · raster: pixel formats, COW tiles, surfaces, interrupt flag · ~1,037 lines → `src/raster/`
  - `crates/raster/src/`: interrupt.rs, lib.rs
- [x] **F6** (2026-10-09, with F7) · psd (1/4): header, byte I/O, RLE/ZIP compression, image resources, file skeleton · ~2,916 lines → `src/psd/`
  - `crates/psd/src/`: blend.rs, compression.rs, error.rs, file.rs, hdr.rs, header.rs, image_data.rs, io.rs, lib.rs, metadata.rs, pixels.rs, resources.rs, tree.rs
- [x] **F7** (2026-10-09, with F6) · psd (2/4): layers, tagged blocks, descriptors, paths, slices, builder · ~3,974 lines → `src/psd/`
  - `crates/psd/src/`: builder.rs, descriptor.rs, filter_effects.rs, layer.rs, path.rs, slices.rs, tagged.rs
- [x] **F8** (2026-10-09) · psd (3/4): layered TIFF (Photoshop TIFF) · ~2,331 lines → `src/psd/`
  - `crates/psd/src/`: tiff.rs
- [x] **F9** (2026-10-09) · psd (4/4): ABR brushes, PAT patterns, ACO/ASE swatches, GRD gradients, test generator · ~2,587 lines → `src/psd/`
  - (done early, in F6/F7: testgen.rs as `tests/psd/testgen.h`, and all the integration tests listed below)
  - `crates/psd/src/`: abr.rs, aco.rs, ase.rs, grd.rs, patterns.rs, testgen.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/psd/tests/`: builder.rs, corpus.rs, layer_block_order.rs, malformed.rs, proptests.rs, roundtrip.rs, tree.rs
- [x] **F10** (2026-10-09, with F11–F13) · codecs (1/4): image model, format sniffing, options, orientation, fidelity, PNM · ~2,340 lines → `src/codecs/`
  - `crates/codecs/src/`: codecs/mod.rs, codecs/pnm.rs, codecs/via_image.rs, error.rs, fidelity.rs, format.rs, image.rs, lib.rs, options.rs, orientation.rs
- [x] **F11** (2026-10-09) · codecs (2/4): PNG (libpng decode, own writer: D26) and JPEG (libjpeg-turbo) · ~944 lines → `src/codecs/`
  - `crates/codecs/src/`: codecs/jpeg.rs, codecs/png.rs
- [x] **F12** (2026-10-09) · codecs (3/4): TIFF (own reader/writer: D25) · ~1,564 lines → `src/codecs/`
  - `crates/codecs/src/`: codecs/tiff.rs, codecs/tiff_ifd.rs
- [x] **F13** (2026-10-09) · codecs (4/4): WebP (libwebp, replaces the in-tree VP8 coder), EXR + deep EXR (OpenEXRCore), HEIF (libheif), web export · ~3,936 lines → `src/codecs/`
  - `crates/codecs/src/`: codecs/deep_exr.rs, codecs/exr.rs, codecs/heif.rs, codecs/vp8/bool_encoder.rs, codecs/vp8/mod.rs, codecs/vp8/predict.rs, codecs/vp8/tables.rs, codecs/vp8/transform.rs, codecs/webp.rs, web.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/codecs/tests/`: camera_raw.rs, common/mod.rs, common/tiffgen.rs, decode_warnings.rs, deep_exr.rs, detect_caps.rs, fidelity.rs, heif.rs, limits.rs, malformed.rs, metadata.rs, orientation.rs, png_jpeg.rs, roundtrip.rs, tiff_bench.rs, tiff_pages.rs, tiff_photoshop.rs, tiff_regressions.rs, webp_lossy.rs
  - `crates/heif/src/`: lib.rs
- [x] **F14** (2026-10-09, with F15–F16) · raw (1/3): TIFF/EP containers, lossless JPEG, camera table · ~1,696 lines → `src/raw/`
  - `crates/raw/src/`: cameras.rs, error.rs, lib.rs, ljpeg.rs, par.rs, tiff.rs, tiffep.rs
- [x] **F15** (2026-10-09) · raw (2/3): CR2, NEF, ORF, RW2, ARW, DNG + opcodes · ~2,051 lines → `src/raw/`
  - `crates/raw/src/`: cr2.rs, dng.rs, nefc.rs, opcodes.rs, orf.rs, rw2.rs, sony.rs
- [x] **F16** (2026-10-09) · raw (3/3): sensor model, demosaic, colour, develop, previews, test generator · ~2,840 lines → `src/raw/`
  - `crates/raw/src/`: color.rs, demosaic.rs, develop.rs, preview.rs, sensor.rs, testgen.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/raw/tests/`: dng.rs, hostile.rs, vendor_codecs.rs, vendors.rs
- [x] **F17** (2026-10-09) · affinity: Affinity Photo container (zstd), model, raster/shape/text import · ~3,406 lines → `src/affinity/`
  - `crates/affinity/src/`: access.rs, container.rs, geometry.rs, lib.rs, model.rs, paint.rs, preview.rs, raster.rs, shapes.rs, stream.rs, synth.rs, tests.rs, text.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/affinity/tests/`: real_files.rs

### Phase 2: document model (L1) (~4,603 lines)

Needs F1, F2, F5.

- [x] **D1** (2026-10-09, with D2) · doc (1/2): document, layers, masks, blend modes, blend-if, adjustments, effects · ~2,430 lines → `src/doc/`
  - `crates/doc/src/`: adjust.rs, advanced.rs, blend_if.rs, effects.rs, lib.rs, mode.rs
- [x] **D2** (2026-10-09) · doc (2/2): vector, text, text styles, patterns, video, slices, comps, variables, analysis · ~2,173 lines → `src/doc/`
  - `crates/doc/src/`: analysis.rs, comps.rs, pattern.rs, slices.rs, text.rs, text_styles.rs, variables.rs, vector.rs, video.rs

### Phase 3: L2 libraries (history, paint, algorithms, vector, text) (~51,128 lines)

Needs D1–D2. L13–L15 also need F6–F7 (PSD type data). Chunks in this phase are independent of each other except L4 first in algo.

- [ ] **L1** · ops: undo/redo history, tile deltas · ~609 lines → `src/ops/`
  - `crates/ops/src/`: lib.rs, tiles.rs
- [ ] **L2** · paint (1/2): brush tips, dynamics, stroke rendering, RNG, tiles · ~4,089 lines → `src/paint/`
  - `crates/paint/src/`: brush.rs, brush_tests.rs, dynamics.rs, lib.rs, presets.rs, render.rs, rng.rs, tile.rs
- [ ] **L3** · paint (2/2): mixer, procedural, colour replace, retouch, background eraser · ~1,624 lines → `src/paint/`
  - `crates/paint/src/`: bg_erase.rs, mixer.rs, procedural.rs, replace.rs, retouch.rs
- [ ] **L4** · algo (1/8): image core, histogram, tone, resample, transform, pyramids · ~4,913 lines → `src/algo/`
  - `crates/algo/src/`: fxutil.rs, histogram.rs, image.rs, lib.rs, params_ext.rs, pyramid.rs, resample.rs, tests.rs, tests_ext.rs, tone.rs, transform.rs
- [ ] **L5** · algo (2/8): blurs (incl. FFT motion blur), sharpen, noise, denoise · ~4,917 lines → `src/algo/`
  - `crates/algo/src/`: blur.rs, blur/motion_apply.rs, blur/motion_bench.rs, blur/motion_fft.rs, blur/motion_fft_tests.rs, blur/motion_tests.rs, blur2.rs, denoise.rs, noise.rs, selection_blur.rs, shape_blur.rs, shape_blur_tests.rs, sharpen.rs
- [ ] **L6** · algo (3/8): distort, stylize, pixelate, artistic, render, other, gallery · ~6,165 lines → `src/algo/`
  - `crates/algo/src/`: artistic.rs, artistic_fx.rs, distort.rs, distort2.rs, gallery.rs, oil.rs, other.rs, other2.rs, pixelate.rs, render.rs, render2.rs, stylize.rs, stylize2.rs, tests_mosaic.rs, trap.rs, video.rs, warp.rs
- [ ] **L7** · algo (4/8): selection, magnetic lasso, segmentation, matting · ~5,751 lines → `src/algo/`
  - `crates/algo/src/`: magnetic.rs, magnetic/tests.rs, matting.rs, segment/focus.rs, segment/gmm.rs, segment/grabcut.rs, segment/maxflow.rs, segment/mod.rs, segment/quick.rs, segment/sky.rs, segment/slic.rs, segment/subject.rs, segment/tests.rs, selection.rs
- [ ] **L8** · algo (5/8): retouch, inpaint, content-aware, Poisson, seam carving, red-eye, stacks · ~3,581 lines → `src/algo/`
  - `crates/algo/src/`: content_aware.rs, erase.rs, inpaint.rs, paint.rs, poisson.rs, redeye.rs, retouch.rs, seam.rs, stack.rs
- [ ] **L9** · algo (6/8): Camera Raw develop, HDR, lens, wide-angle, perspective, vanishing point, relight · ~3,964 lines → `src/algo/`
  - `crates/algo/src/`: camera_raw.rs, hdr.rs, lens.rs, perspective.rs, relight.rs, vanishing.rs, wideangle.rs
- [ ] **L10** · algo (7/8): features, panorama, scan crop, EXIF, quantize, vectorscope · ~3,513 lines → `src/algo/`
  - `crates/algo/src/`: exif.rs, features.rs, panorama.rs, photo_util.rs, quantize.rs, scancrop.rs, vectorscope.rs
- [ ] **L11** · algo (8/8): Liquify, Puppet Warp · ~2,050 lines → `src/algo/`
  - `crates/algo/src/`: liquify.rs, puppet.rs
- [ ] **L12** · vector: paths, shapes, flatten, stroke, edit, rasterise, trace · ~2,944 lines → `src/vector/`
  - `crates/vector/src/`: edit.rs, flatten.rs, lib.rs, raster.rs, shapes.rs, stroke.rs, tests.rs, trace.rs
- [ ] **L13** · text (1/3): fonts, Craft fonts, glyph outlines, rasterisation, optical kerning · ~1,872 lines → `src/text/`
  - `crates/text/src/`: craft_fonts.rs, fonts.rs, glyphs.rs, lib.rs, optical.rs, raster.rs, render.rs
- [ ] **L14** · text (2/3): layout (HarfBuzz/FriBidi/libunibreak), CJK, vertical, warp, spell · ~3,539 lines → `src/text/`
  - `crates/text/src/`: cjk.rs, layout.rs, spell.rs, tests.rs, vertical_tests.rs, warp.rs
- [ ] **L15** · text (3/3): PSD type layers, EngineData, styles · ~1,597 lines → `src/text/`
  - `crates/text/src/`: engine_data.rs, psd.rs, psd_styles.rs

### Phase 4: compositor and native format (L3) (~12,078 lines)

Needs Phase 3 (compose uses vector; format uses codecs).

- [ ] **C1** · compose (1/3): CPU compositor core, bounds, masks, PS blend, multichannel, proxy · ~4,283 lines → `src/compose/`
  - `crates/compose/src/`: bounds.rs, fill_layout.rs, lib.rs, masks.rs, multichannel.rs, proxy.rs, psblend.rs, tests.rs
- [ ] **C2** · compose (2/3): adjustment layers, patterns, gradient fills, shape split, advanced blending · ~2,416 lines → `src/compose/`
  - `crates/compose/src/`: adjust.rs, advanced.rs, advanced_tests.rs, gradient_fill.rs, pattern.rs, shape_split.rs, stroke_tests.rs
- [ ] **C3** · compose (3/3): layer effects (styles) · ~1,955 lines → `src/compose/`
  - `crates/compose/src/`: effects.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/compose/tests/`: artboard.rs
- [ ] **C4** · format (1/2): zip container, atomic writes, finite checks, store · ~2,055 lines → `src/format/`
  - `crates/format/src/`: atomic.rs, atomic/tests.rs, finite.rs, lib.rs, read.rs, store.rs, zip.rs
- [ ] **C5** · format (2/2): manifest, doc conversion, migration, autosave · ~1,369 lines → `src/format/`
  - `crates/format/src/`: autosave.rs, convert.rs, manifest.rs, migrate.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/format/tests/`: analysis.rs, autosave.rs, common/mod.rs, corruption.rs, layer_labels.rs, roundtrip.rs

### Phase 5: file I/O and plug-ins (L4) (~14,434 lines)

Needs Phase 4 and F6–F17.

- [ ] **I1** · io (1/5): flat formats, pixels, raw, linked files, layered TIFF, channels · ~2,037 lines → `src/io/`
  - `crates/io/src/`: channel_map.rs, flat.rs, lib.rs, linked.rs, multichannel_map.rs, pixels.rs, raw.rs, tiff_layers.rs
- [ ] **I2** · io (2/5): PSD import, tagged blocks, adjustment + effect mapping · ~3,638 lines → `src/io/`
  - `crates/io/src/`: adjust_map.rs, blocks.rs, effects_map.rs, psd_import.rs
- [ ] **I3** · io (3/5): PSD export, smart objects · ~2,505 lines → `src/io/`
  - `crates/io/src/`: psd_export.rs, smart_map.rs
- [ ] **I4** · io (4/5): vector, text, comps, slices, annotations, patterns, brushes, gradients · ~3,308 lines → `src/io/`
  - `crates/io/src/`: abr_map.rs, abr_map/tests.rs, annotations_map.rs, comps_map.rs, gradient_bake.rs, pattern_map.rs, slices_map.rs, text_import.rs, text_styles_map.rs, vector_map.rs
- [ ] **I5** · io (5/5): Camera Raw settings, Affinity import, SVG import (plutosvg) · ~1,853 lines → `src/io/`
  - `crates/io/src/`: affinity.rs, camera_raw_map.rs, svg.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/io/tests/`: adjust_blend_roundtrip.rs, affinity.rs, affinity_corpus.rs, annotations.rs, artboard_guide_order.rs, banded.rs, cancel.rs, channels.rs, color.rs, common/mod.rs, composite.rs, comps_artboards.rs, corpus.rs, corpus_regressions.rs, effects_multi.rs, flat.rs, heif.rs, heif_unsupported.rs, lab16.rs, large_psb.rs, layer_labels.rs, modes.rs, orientation.rs, psd32_export.rs, psd_block_layout.rs, psd_empty_channels.rs, psd_kerning.rs, psd_structure.rs, psd_type_bounds.rs, psd_vertical_type.rs, raw.rs, resolution_axes.rs, roundtrip.rs, smart_corpus.rs, svg.rs, text_corpus.rs, text_styles.rs, tiff_corpus.rs, tiff_layers.rs, tiff_pages.rs, vector.rs, xmp_export.rs
- [ ] **I6** · plugins: manifest, registry, WebAssembly runtime (WAMR), apply · ~1,093 lines → `src/plugins/`
  - `crates/plugins/src/`: apply.rs, lib.rs, manifest.rs, registry.rs, runtime.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/plugins/tests/`: abi.rs

### Phase 6: engine (L5) (~72,562 lines)

Needs Phase 5. E1 and E2 first; E3–E12 in any order. E12 also ports `crates/engine/tests/` that are left, including `panic_hunt.rs` (every command, adversarial params, no crash).

- [ ] **E1** · engine (1/12): Session, command registry, jobs, pixels, canvas geometry, snapping · ~5,652 lines → `src/engine/`
  - `crates/engine/src/`: build_info.rs, canvas_geom.rs, canvas_geom/tests.rs, commands.rs, hidden_target.rs, inspect.rs, jobs.rs, jobs_tests.rs, lib.rs, pixels.rs, snap.rs, tests.rs
- [ ] **E2** · engine (2/12): preferences, preset store, built-in presets · ~7,136 lines → `src/engine/`
  - `crates/engine/src/`: prefs.rs, prefs/tests.rs, preset_import_cmds.rs, preset_store.rs, preset_store/tests.rs, presets/clone_source.rs, presets/gradients.rs, presets/mod.rs, presets/patterns.rs, presets/shapes.rs, presets/styles.rs, presets/swatches.rs, presets/tests.rs, presets/tools.rs
- [ ] **E3** · engine (3/12): File, Image, Mode, print, document presets · ~5,036 lines → `src/engine/`
  - `crates/engine/src/`: document_preset_cmds.rs, file_cmds.rs, file_cmds/tests.rs, image_cmds.rs, migrate_cmds.rs, mode_cmds.rs, mode_cmds/tests.rs, multichannel_cmds.rs, multichannel_cmds/tests.rs, print_cmds.rs, print_cmds/tests.rs, wia_cmds.rs
- [ ] **E4** · engine (4/12): Edit, fill, float, free transform, stamp, copy · ~5,105 lines → `src/engine/`
  - `crates/engine/src/`: edit_cmds.rs, edit_menu_cmds.rs, edit_menu_cmds/tests.rs, fill_cmds.rs, fill_key_cmds.rs, float_cmds.rs, float_cmds/tests.rs, layer_copy_cmds.rs, layer_copy_cmds/tests.rs, stamp_cmds.rs, transform_cmds.rs
- [ ] **E5** · engine (5/12): Layer menu, multi-layer, navigation, labels, groups, masks, align · ~4,608 lines → `src/engine/`
  - `crates/engine/src/`: align_cmds.rs, group_view_cmds.rs, layer_label_cmds.rs, layer_label_cmds/tests.rs, layer_menu_cmds.rs, layer_menu_cmds/tests.rs, layer_multi_cmds.rs, layer_nav_cmds.rs, mask_view_cmds.rs, mask_view_cmds/tests.rs
- [ ] **E6** · engine (6/12): layer styles, artboards, smart objects · ~3,953 lines → `src/engine/`
  - `crates/engine/src/`: artboard_cmds.rs, artboard_cmds/tests.rs, layer_style.rs, smart_cmds.rs, smart_cmds/tests.rs, smart_cmds/unpack.rs
- [ ] **E7** · engine (7/12): Select, smart select, magnetic, cutout, channels · ~4,708 lines → `src/engine/`
  - `crates/engine/src/`: channel_cmds.rs, channel_cmds/tests.rs, cutout_cmds.rs, magnetic_cmds.rs, magnetic_cmds/tests.rs, select_extra_cmds.rs, selection_cmds.rs, smartselect_cmds.rs
- [ ] **E8** · engine (8/12): adjustments, colour, display colour, proofing, swatches, analysis · ~7,302 lines → `src/engine/`
  - `crates/engine/src/`: adjust_cmds.rs, adjust_cmds/tests.rs, adjust_params.rs, analysis_cmds.rs, analysis_cmds/tests.rs, color_cmds.rs, display_color.rs, display_color_tests.rs, proof_sim.rs, swatch_cmds.rs, swatch_cmds/tests.rs
- [ ] **E9** · engine (9/12): Filter menu · ~5,985 lines → `src/engine/`
  - `crates/engine/src/`: distort_cmds.rs, distort_cmds/tests.rs, filters.rs, filters_ext.rs, filters_ext/tests.rs, fx_view_cmds.rs, gallery_cmds.rs, gallery_cmds/tests.rs, lens_cmds.rs, photo_cmds.rs, render_cmds.rs, trap_cmds.rs, vp_cmds.rs, warp_cmds.rs
- [ ] **E10** · engine (10/12): painting, brushes, eraser, retouch, red-eye, symmetry, patterns, gradients, picking · ~8,495 lines → `src/engine/`
  - `crates/engine/src/`: brush_cmds.rs, brush_cmds/tests.rs, brush_key_cmds.rs, brush_preset_cmds.rs, eraser_cmds.rs, eraser_cmds/tests.rs, gradient_fill_cmds.rs, gradient_fill_cmds/tests.rs, paint_cmds.rs, pattern_cmds.rs, pick_cmds.rs, redeye_cmds.rs, retouch_cmds.rs, retouch_cmds/content_aware_move.rs, retouch_cmds/patch.rs, retouch_cmds/pattern_stamp.rs, retouch_cmds/tests.rs, symmetry_cmds.rs
- [ ] **E11** · engine (11/12): type and vector commands · ~6,016 lines → `src/engine/`
  - `crates/engine/src/`: path_edit_cmds.rs, type_caret_cmds.rs, type_cmds.rs, type_extra_cmds.rs, type_extra_cmds/tests.rs, type_spell_cmds.rs, type_styles_cmds.rs, type_styles_cmds/tests.rs, vector_cmds.rs, vector_cmds/tests.rs
- [ ] **E12** · engine (12/12): actions, automate, variables, comps, slices, web, notes, timeline, video, plug-ins, extra · ~8,566 lines → `src/engine/`
  - `crates/engine/src/`: actions_cmds.rs, automate_cmds.rs, automate_cmds/tests.rs, comps_cmds.rs, comps_cmds/tests.rs, extra_cmds.rs, frame_cmds.rs, notes_cmds.rs, plugin_cmds.rs, plugin_cmds/tests.rs, slice_cmds.rs, slice_cmds/tests.rs, timeline_cmds.rs, variables_cmds.rs, variables_cmds/tests.rs, video_cmds.rs, video_cmds/tests.rs, web_cmds.rs, web_cmds/tests.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/engine/tests/`: affinity.rs, brush_smoothing.rs, camera_raw_noise_clamp.rs, close_document.rs, command_lookup.rs, empty_selection_combine.rs, eraser_locked.rs, fill_lock_transparency.rs, filter_locked.rs, flatten_background.rs, gallery_colours.rs, gauss_small_sigma.rs, guide_layout_limit.rs, hdr_gamma_direction.rs, keyboard_shortcut_bulk.rs, layer_delete_last.rs, panic_hunt.rs, path_blur_speed_clamp.rs, photoshop_oracles.rs, pixel_lock_edits.rs, prefs_usage.rs, rendering_preferences.rs, smart_psd.rs, svg_smart_object.rs, text_gamma.rs, undo_keeps_active_layer.rs, unicode_colors.rs

### Phase 7: automation and CLI (L6) (~3,711 lines)

Needs E1–E12. After A3 the CLI matches the Rust one for `convert/info/run/batch/commands/mcp`; diff its output on the corpus.

- [ ] **A1** · automation (1/2): JSON-RPC, control protocol bridge, budgets, security, files · ~1,427 lines → `src/automation/`
  - `crates/automation/src/`: bridge.rs, budgets.rs, files.rs, lib.rs, rpc.rs, security.rs
- [ ] **A2** · automation (2/2): MCP server, workspace, headless sessions · ~1,704 lines → `src/automation/`
  - `crates/automation/src/`: headless.rs, server.rs, workspace.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/automation/tests/`: agent_tasks.rs, inactive_save.rs, layered_save_dirty.rs, mcp.rs
- [ ] **A3** · photocraft-cli: convert / info / run / batch / commands / mcp · ~580 lines → `src/cli/`
  - `apps/photocraft-cli/src/`: lib.rs, main.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `apps/photocraft-cli/tests/`: cli.rs, orientation.rs

### Phase 8: UI and GPU compositor (~96,311 lines)

Needs B3 and Phase 7 (the control channel). U1–U4 in order, then G1–G2, then U5–U10 in any order. Visual check: offscreen snapshot PNGs, as in the Rust `snapshot` example (U1 ports it).

- [ ] **U1** · UI shell (1/10): app state, theme tokens, icons, widgets, sizing, i18n · ~8,045 lines → `src/ui/`
  - `crates/ui-egui/src/`: brand.rs, credits.rs, enable_rules.rs, i18n/catalog.rs, i18n/live_tests.rs, i18n/mod.rs, i18n/system.rs, icon_data.rs, icons.rs, lib.rs, links.rs, parity.rs, proxy.rs, sizing.rs, state.rs, theme.rs, widgets.rs
- [ ] **U2** · UI shell (2/10): menus, menu catalog, keyboard shortcuts, control channel · ~9,089 lines → `src/ui/`
  - `crates/ui-egui/src/`: control.rs, hold_keys.rs, hold_keys/tests.rs, menu_catalog.rs, menu_nav.rs, menu_nav_tests.rs, menus.rs, native_menu.rs, shortcut_capture_tests.rs, shortcut_dispatch.rs, shortcut_dispatch/tests.rs, shortcuts.rs, view_cmds.rs, view_cmds/tests.rs
- [ ] **U3** · UI shell (3/10): window chrome, docking, tabs, work area, notices, dialogs, workspaces · ~5,500 lines → `src/ui/`
  - `crates/ui-egui/src/`: chrome_ui.rs, dialogs.rs, dock.rs, dock_tests.rs, gpu_status.rs, jobs_ui.rs, jobs_ui_tests.rs, monitor_status.rs, notices.rs, props_layout.rs, props_layout_tests.rs, tab_strip.rs, titlebar.rs, work_area.rs, workspace_ui.rs
- [ ] **U4** · UI canvas (4/10): canvas view, GPU canvas, rulers, scrollbars, zoom/rotate, cursors · ~10,880 lines → `src/ui/`
  - `crates/ui-egui/src/`: canvas.rs, canvas/tabs_tests.rs, channel_view.rs, gpu_canvas.rs, gpu_canvas_mip_tests.rs, input_tests.rs, outline.rs, rotate_view.rs, rulers.rs, scrollbars.rs, tool_cursor.rs, tool_cursor/tests.rs, tool_feedback.rs, transform_tex.rs, wheel_nav.rs, zoom_tool.rs
- [ ] **G1** · GPU compositor (1/2): GL 3.3 context, plan, tiles, blend shaders · ~3,982 lines → `src/gpu/`
  - `crates/gpu/src/`: bounds.rs, health.rs, lib.rs, plan.rs
- [ ] **G2** · GPU compositor (2/2): layer-effect shaders · ~714 lines → `src/gpu/`
  - `crates/gpu/src/`: fx.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/gpu/tests/`: device_loss.rs, parity.rs, vector_mask.rs
- [ ] **U5** · UI panels (5/10): Layers, Channels, Swatches, Color, Comps, Variables, Timeline, Actions, presets · ~12,173 lines → `src/ui/`
  - `crates/ui-egui/src/`: actions.rs, blend_dropdown_keys_tests.rs, channels_panel.rs, color_picker_ui.rs, comps_ui.rs, hidden_layer_tests.rs, layer_menu_ui.rs, layer_menu_ui/color_tests.rs, layer_pct_slider_tests.rs, layer_pick_ui.rs, layer_props_ui.rs, layer_reveal.rs, layer_reveal_tests.rs, layer_row_ui.rs, layer_row_ui/edit_tests.rs, layer_row_ui/rename_tests.rs, layer_row_ui/tests.rs, layer_transfer.rs, layer_transfer/tests.rs, layer_tree_ui.rs, layer_tree_ui/tests.rs, mask_thumbs_ui.rs, mask_thumbs_ui/tests.rs, palette.rs, panels.rs, preset_files_ui.rs, preset_panels.rs, quick_pick.rs, screen_picker.rs, swatches_ui.rs, timeline_ui.rs, variables_ui.rs
- [ ] **U6** · UI tools (6/10): move, transform, crop, marquee/lasso, direct select, snapping, artboards, slices · ~9,720 lines → `src/ui/`
  - `crates/ui-egui/src/`: artboard_ui.rs, canvas_tool_menu.rs, clip_line_ui.rs, clip_line_ui/tests.rs, crop_ui.rs, direct_select.rs, direct_select_tests.rs, lasso_ui.rs, lasso_ui/tests.rs, magnetic_lasso_ui.rs, magnetic_lasso_ui/tests.rs, marquee_tests.rs, move_auto_select_tests.rs, move_mods.rs, move_ui.rs, path_selection_tests.rs, polygon_lasso_tests.rs, slice_ui.rs, snap_ui.rs, stroke_constraint.rs, transform_tool.rs, transform_undo_tests.rs
- [ ] **U7** · UI tools (7/10): brushes, painting, stylus, eraser, retouch, fill, gradient · ~7,308 lines → `src/ui/`
  - `crates/ui-egui/src/`: brush_panel.rs, brush_panel_tests.rs, brush_picker.rs, brush_preview.rs, brush_resize.rs, brush_sections.rs, brushes_tab.rs, eraser_ui.rs, fill_ui.rs, fill_ui_tests.rs, gradient_ui.rs, opacity_keys.rs, paint_mouse.rs, patch_preview.rs, pencil_tests.rs, retouch_ui.rs, stamp_tests.rs, stroke_trail.rs, stylus.rs
- [ ] **U8** · UI tools (8/10): type tool, type panels, CJK fonts, vector/pen UI, warps (perspective, puppet, liquify, wide-angle) · ~9,080 lines → `src/ui/`
  - `crates/ui-egui/src/`: cjk_fonts.rs, distort_ui.rs, liquify_ui.rs, perspective_ui.rs, point_curve.rs, puppet_ui.rs, type_font_picker_tests.rs, type_panels_ui.rs, type_tool.rs, type_tool_tests.rs, type_transform.rs, vector_ui.rs, wide_angle_ui.rs
- [ ] **U9** · UI dialogs (9/10): adjustments, filters, layer style, Color Range, Camera Raw, analysis · ~12,794 lines → `src/ui/`
  - `crates/ui-egui/src/`: adjust_dialog.rs, adjust_editors.rs, adjust_editors_tests.rs, adjust_preview.rs, adjust_preview_tests.rs, adjust_ui.rs, analysis_ui.rs, blend_preview.rs, blend_preview/tests.rs, camera_raw_detail_ui.rs, camera_raw_preview_ui.rs, camera_raw_scope_ui.rs, camera_raw_ui.rs, color_range_ui.rs, filter_dialog.rs, gallery_ui.rs, layer_style.rs, plugin_ui.rs, rgb_histogram.rs, smart_ui.rs, tone.rs
- [ ] **U10** · UI dialogs (10/10): New, Open/Save/Export, file dialogs, document props, Preferences, discard, rasterize · ~7,026 lines → `src/ui/`
  - `crates/ui-egui/src/`: discard_ui.rs, doc_props_ui.rs, export_dialog.rs, file_dialog.rs, file_dialog/tests.rs, file_open.rs, file_open/tests.rs, file_ui.rs, new_doc_ui.rs, prefs_ui.rs, rasterize_prompt.rs, rasterize_prompt_tests.rs, tiff_options_ui.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/ui-egui/tests/`: adjust_preview_gpu.rs, arbitrary_rotation_preview.rs, camera_raw_histogram.rs, camera_raw_localization.rs, camera_raw_scope.rs, camera_raw_zoom.rs, canvas_16f.rs, checker_canvas.rs, color_managed_canvas.rs, crop_beyond_canvas.rs, drag_preview_canvas.rs, export_as_scaling.rs, full_res_preview.rs, gpu_device_loss.rs, layout_perf.rs, live_stroke_canvas.rs, paint_bucket_all_layers.rs, support/layout_doc.rs, system_locale.rs, variables_data_sets.rs

### Phase 9: desktop app (~6,235 lines)

Needs Phase 8.

- [ ] **P1** · desktop app (1/2): main, app dirs, logging, startup, control server, UI state persistence · ~2,937 lines → `src/app/`
  - `apps/photocraft/src/`: app_dirs.rs, app_icon.rs, control_server.rs, crash_guard.rs, gpu_startup.rs, linux_libs.rs, logging.rs, main.rs, ui_state.rs
- [ ] **P2** · desktop app (2/2): OS services, monitor profile, screen colour, cursor, tablet (SDL3 pen), macOS bits · ~3,298 lines → `src/app/`
  - `apps/photocraft/src/`: apple_events.rs, cursor.rs, mac_menu.rs, mac_window.rs, monitor_profile.rs, screen_color.rs, services.rs, tablet.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `apps/photocraft/tests/`: mac_menu_appkit.rs, startup_without_display.rs
  - `crates/tablet/src/`: appkit.rs, lib.rs, macos.rs, x11.rs, xi.rs
  - integration tests (this is the crate's last chunk; port any not yet done): `crates/tablet/tests/`: monitor_macos.rs

### Phase 10: tooling, docs, cutover

- [ ] **Z1** · replace `xtask` (4.3k Rust lines): `tools/parity.sh` (regenerates `docs/parity.md` from
  the C menu catalog), `tools/scorecard` (scorecard/*.toml → `docs/scorecard.md`), `tools/perf`
  (benchmarks from `perf/budgets.toml` + `crates/testkit/src/perf.rs`, 24–36 MP release runs),
  `make dist` / packaging scripts in `packaging/` for Linux (AppImage/tarball), macOS (.app) and FreeBSD.
- [ ] **Z2** · documentation: rewrite `README.md`, `docs/architecture.md`, `docs/development.md`,
  `docs/contributing.md`, `docs/control-protocol.md` (verify unchanged), `docs/plugins.md` for the C tree;
  new `AGENTS.md` for C contributors (conventions from §4, never-crash, layering, tests).
- [ ] **Z3** · cutover: run the full differential suite one last time, then delete `Cargo.toml`,
  `Cargo.lock`, `crates/`, `apps/`, `xtask/`, `tools/rustref/`, `clippy.toml`, `rustfmt.toml`, `.cargo/`,
  Rust-only `examples/` and `book/` content; rewrite `.github/workflows/` (Linux + macOS + FreeBSD:
  `make check`, `make sanitize`, `make test-corpus`) and the `Dockerfile`.
- [ ] **Z4** · final audit: `make check`, `make sanitize`, valgrind on the test suite, `make analyze`,
  corpus run, fuzz each parser (psd, codecs, raw, affinity, format, cms, JSON params) for a fixed budget
  with libFuzzer or AFL++ where available; review every `third_party/` licence; update §7 and the README's
  known limitations.

## 6. Not ported (by decision)

- **X1** · web build (dropped: see decisions) · ~366 lines
  - `apps/photocraft-web/src/`: main.rs, web.rs
  - Not ported (D2). `apps/photocraft-web` is dropped.
- **T1** · testkit + perf harness · ~199 lines
  - `crates/testkit/src/`: lib.rs, perf.rs
  - Folded into B1 (`tests/harness.h`) and Z1 (`tools/perf`).
- `crates/tablet` (unsafe AppKit/XInput2 pen code) · folded into P2: SDL3's pen API gives pressure and tilt on every platform (D4).

## 7. Intentional differences from the Rust implementation

Add one line per difference: `chunk · what differs · why · user-visible?`

- D2 · no Windows build, no web build · POSIX/C target · yes
- D9 · lossy WebP and JPEG encoders produce different bytes (libwebp/libjpeg-turbo vs the in-tree coders); decoded pixels stay within the Rust tests' tolerances · library choice · byte-level only
- D10 · no dictionary line breaking (Thai, Lao, Khmer, Burmese, Japanese words) · no ICU · yes, in text layout of those scripts
- D19 · no macOS global menu bar, no Apple-event "Open With", no Wayland portal colour picker · not C17/POSIX · yes
- B2 · zlib/zstd output bytes differ from flate2 (miniz_oxide) and ruzstd at the same level; decompressed data is identical · library choice · no (byte-level only)
- B2 · JSON parse errors: same kind, same "at line L column C" format and the same position for most inputs, but some wording differs from serde_json ("trailing comma is not allowed" vs "trailing comma", "expected value" vs "expected `:`", surrogate/control-character messages; "01" is reported at column 1, serde says 2) · yyjson's messages · only in error text
- B2 · `"-0"` parses as integer 0 (serde_json: float -0.0), so `as_u64("-0")` succeeds · yyjson · edge case only
- B2 · zstd frames needing a window over 128 MiB are rejected by default (ruzstd allows 100 MiB+ up to its own cap); callers pass a smaller limit where the reference does (Affinity: 64 MiB) · libzstd's safe default · only for hostile/odd files
- B2 · `pc_f16_from_f64` rounds through f32 (the `half` crate does this on x86 with F16C, directly on aarch64, and with a quirky software path elsewhere; the reference only calls `from_f32`) · reference is platform-dependent · no
- F3 · `pc_profile_content_hash` is the first 8 bytes of BLAKE3 over the profile bytes (Rust: std `DefaultHasher`, SipHash-1-3, whose output is unspecified across Rust versions) · stable, already a dependency · no (in-memory cache keys only)
- F4 · `pc_lut3d_from_transform` returns `PC_ERR_INVALID` for a non 3→3/3→1 transform or a size outside 2..=256 (Rust asserts/panics, and has no upper bound) · never crash; caps a hostile allocation · no
- F4 · the shipped `photocraft-coated-cmyk.icc` differs from what today's generator writes (Rust or C) in 2 CLUT bytes by one code; it was made by an older Rust toolchain. Both implementations load the shipped file, so colours are unchanged · history · no
- F6 · psd model: Rust enums with an `Unknown(v)` variant (ColorMode, Compression, SectionType, BlendMode) are stored as their raw value / 4-byte key · C has no sum types; round trips are unchanged · no
- F6 · psd: places where the reference would panic (indexing a short plane in `rgba8`/`composite_rgba8`, `u16`/`u32` length casts) return `invalid data: channel size mismatch` / limit errors; builder caller mistakes are `PC_ERR_INVALID` (same message as the reference's `Invalid`) · never crash · no
- F6 · psd: builder and channel zip/zip-prediction bytes differ (zlib vs miniz_oxide, see B2); raw and RLE output is byte-identical to the reference (`tests/psd/test_parity.c`) · library choice · byte-level only
- F2 · `pc_cmyk_space_for_profile` caches by profile content (16 entries, FIFO; negative results too) instead of by `Arc` allocation with weak references · C has no shared byte owner · no (same object for equal bytes)
- F2 · `pc_read_sample`/`pc_write_sample` take the buffer length; an out-of-range index reads 0 / writes nothing (Rust panics) · never crash · no
- F2 · `with_cmyk_space(space, f)` is `pc_cmyk_space_enter`/`pc_cmyk_space_leave` (LIFO token); a thread that exits inside a scope leaks one reference (Rust's thread-local destructor drops it) · no closures in C · no
- F5 · a surface's tile map is an open-addressing hash table (lookups O(1) on the per-pixel path) instead of a BTreeMap; `pc_surface_next_tile` is unordered, `pc_surface_tiles_sorted` gives the reference's (tx, ty) order (used where order matters: `translated`'s collision rule) · speed, no BTreeMap in C · no
- F5 · operations that would touch more than 2^24 tiles (`fill_rect`, `write_region`, `take_tiles`, …) and outputs over `PC_MAX_ALLOC` return `PC_ERR_LIMIT`; the reference tries and runs out of memory or time. `has_tiles_in` on a huge rect checks the allocated tiles instead of walking the range · never crash · only for absurd rects
- F5 · length mismatches (`write_region`, `write_interleaved`, `read_*_into`) and a short fill pixel return `PC_ERR_INVALID`; `sample_channel` with a channel past the format's reads 0; short `px` arrays read missing channels as 0 (the reference panics or reads the neighbouring pixel) · never crash · no
- F5 · `Interrupt` is `pc_interrupt {cancel, progress, ctx}` C callbacks; NULL means none · no closures in C · no
- F9 · names in .aco/.ase/.grd/.pat/.abr models are `pc_psd_str` (UTF-8 + length, may hold U+0000) so they round-trip like Rust `String`s; error text is capped at 255 bytes (`PC_ERROR_MSG_MAX`) and an .abr warning at 1023, so a warning quoting an unusually long error is cut where Rust's is not · fixed-size `pc_error` · only in very long messages
- F9 · .abr parsing returns out-of-memory as an error (Rust aborts the process); the `padded_plane_allocation_failure_is_an_error` test is not ported (it needs a `usize::MAX` request; C reports a failed `calloc` with the same "not enough memory for the padded pattern channel" error) · never crash · no
- F10–F13 · encoded bytes differ for every writer (own PNG/TIFF/GIF/BMP/TGA/ICO/QOI/HDR/PNM writers, libjpeg-turbo, libwebp, OpenEXRCore, zlib vs miniz_oxide); every decoded-pixel tolerance in the Rust tests holds unchanged · library choice · byte-level only
- F13 · WebP lossless encoding is limited to 16383 px per side (libwebp; the in-tree VP8L writer allowed more) · library limit · only for huge WebP saves
- F13 · HEIF 10-bit planes are widened to 16-bit by libheif, not by the reference's shift; the Rust tests' thresholds still pass · libheif · tiny value differences
- F13 · HEIF 4:2:0 chroma is upsampled by libheif (nearest neighbour), the reference (heic-rs) interpolates like Apple's decoder: on hard colour edges the column before the edge differs (max 186/255 on `rgb-strips-96.heic`; `checker-64` matches exactly). The test skips the two edge columns. **Open, needs a decision:** accept, or convert YCbCr ourselves from libheif's planes · libheif · yes, on sharp chroma edges
- F13 · HEIF decodes with libheif's strict mode so a truncated grid file is an error as in the reference (libheif's default fills missing tiles) · parity · no
- F12 · TIFF strips/tiles: uncompressed data ignores StripByteCounts and a byte count past the end of the file is cut there (the tiff crate reads to EOF); a chunk that still comes up short is "missing" (Truncated warning), corrupt compressed data is an error; absurd tile sizes are refused with LimitExceeded by the scratch cap before allocating · matches the reference's tiff crate, never crash · only for damaged files
- F10 · `pc_cimage_convert` / `pc_cimage_to_rgba_f32` refuse an image whose buffer length doesn't match its size (`PC_ERR_INVALID`); Rust's `Image` can't be built inconsistent · C structs are open · no
- F10–F13 · tests: oracles from Rust-only crates (`image`, `png`, `jpeg_encoder`, `tiff` writer, `exr`, `flate2`) are replaced by libpng / libjpeg-turbo / libwebp mux / hand-built fixtures (`tests/codecs/tiffgen.h`, APNG and deep-EXR generators) or our own decoder; proptests are seeded SplitMix64 loops with the same case counts; corpus tests skip with a message when `corpus/` is absent (Rust fails); `#[ignore]` timing tests run with `PHOTOCRAFT_SLOW_TESTS=1`; `tiff_bench.rs` goes to Z1 · no Rust in the C tree · no
- F10–F13 · EXR and HEIF are optional at build time (`WITH_EXR=0` / `WITH_HEIF=0`, auto-detected); without them the formats report not readable/writable and fidelity warns WriteUnsupported (Rust always builds OpenEXR) · optional dependencies · only in such builds
- F14–F16 · camera raw: out of memory is `PC_ERR_NOMEM` (the reference aborts); error text is capped at 255 bytes, so an error quoting a very long list (e.g. "CFA plane colours [...]") is cut (warnings are not capped) · fixed-size `pc_error` · only in very long messages
- F14 · the TIFF reader decodes IFD entries from the bytes on demand instead of collecting a `Vec<Entry>`; the readable-entry count, `next` pointer and "empty if any value is unreadable" list rule are the same · no allocation per IFD · no
- F14 · `pc_raw_dump_structure` prints floats as shortest round-trip digits without an exponent (Rust `Display`); not covered by the parity golden (debugging aid) · no Rust formatter in C · debug output only
- F17 · a damaged compressed archive entry is reported by how early the decompressor notices: the reference's miniz_oxide and ruzstd decode ahead and say "corrupt compressed entry" where zlib / libzstd may finish the stream and the size or CRC check says "archive entry size mismatch" / "checksum mismatch" (all `PC_ERR_FORMAT`); the parity golden's mutation sweep counts the three as one outcome · library choice · only the wording, only for damaged files
- F17 · two archive entries with one name under different ids: the larger id wins (the reference's HashMap picks one at random) · determinism · hostile files only
- F17 · guards the reference lacks: compounds nested deeper than 256 (a cyclic compound overflowed the reference's stack) and spot colours that refer to themselves more than 64 deep read as unreadable; a pie whose end angle is far below its start stops adding turns after a million (the reference loops for ever) · never crash or hang · hostile files only
- D1–D2 · JSON (de)serialisation of doc types is `serde_json::from_value` semantics on a parsed yyjson document: error texts match serde's but carry no " at line L column C" (serde's `from_str` adds them); `from_str`-only errors (e.g. "duplicate field") don't occur because duplicates collapse to the last value first; parse errors are base's text. Messages longer than 255 bytes (unknown-variant lists of BlendMode/Adjustment/LabelColor) are cut; Debug quoting in messages escapes ASCII controls only (Rust also escapes e.g. U+200B, U+00AD) and stops at a NUL · fixed-size `pc_error`, no Rust Debug formatter · only in error messages
- D1–D2 · never-crash guards: `pc_blend_if_set` refuses a non-full pair at index ≥ 65536 (PC_ERR_LIMIT; Rust allocates); out-of-range `pc_path_effective_op` / `pc_subpath_segment` return COMBINE / zero points (Rust panics); invalid UTF-8 bytes in a document name become `_` in slice base names; tree walks are iterative, and `pc_layer_eq`/`pc_document_eq` return false and `pc_document_insert_above` PC_ERR_NOMEM when a walk stack can't grow past 128 levels (Rust can't fail there) · never crash · hostile input / out of memory only
- D1–D2 · `pc_document_walk` lets a first child share its parent's path entries (linear memory for deep chains; Rust's per-item `Vec` is quadratic); each item's path slice is the same · memory · no
- D1–D2 · Rust `String`s are `pc_str` (length-carrying, NUL-safe); `Arc<Vec<u8>>` / `Arc<Vec<f32>>` are refcounted `pc_blob`s (base); the codecs module's optional owned buffer was renamed `pc_meta_blob` to make room · C has no generics · no

## 8. Progress log

Newest last. One dated line per chunk session: chunk, what landed, test counts, open items.

- 2026-10-09 · B0 · Inventoried 667 Rust source files (~300k lines in `src/`, ~35k more in integration tests/benches/examples, 4.3k in xtask); every file assigned to exactly one chunk (generator check). Decisions D1–D19 recorded. Branch `WD-40`. No C code yet.
- 2026-10-09 · B1 · `Makefile` (all/test/check/sanitize/tsan/analyze/valgrind/install/clean; compiler flags feature-probed; `MODULES`/`PKGS`/`PROGRAMS` lists for later chunks to extend), `tests/harness.h`, `.clang-format`, `src/base`: status, num (checked math, Rust-`as` saturating casts, NaN-preserving clamp), mem, buf, bytes (endian loads/stores, sticky-error reader), file (EINTR/partial-I/O loops, bounded reads, `mkdir -p`, atomic write matching `crates/format/src/atomic.rs` semantics and messages), pool (deterministic chunking, lowest-index error, nested→serial), log. 8 test executables, 47 tests, 11,399 checks: green under `make check` (gcc 15.2), clang 23 strict, `make sanitize`, `make tsan`, `make analyze` (clang analyzer, 7 files clean). Not run: valgrind (this machine's glibc is built with `-march=znver4`; valgrind 3.27 SIGILLs on AVX-512 in glibc's own startup, even for `/bin/true`; our code is built for baseline x86-64; `make valgrind` detects this and skips), cppcheck (not installed), macOS/BSD builds (no machines). Added D20 (GNU make) and D21 (per-file XSI). Open: C4 still ports `atomic.rs`'s failure-injection tests and the Windows-only rename retry is dropped (D2).
- 2026-10-09 · B2 · `src/base`: json (yyjson wrapper, see D7), base64 (base64 0.22 STANDARD semantics and error messages), half (f16, port of half 2.7.1), hash (BLAKE3 via libblake3, hex), compress (zlib/raw/gzip inflate with flate2 `take()` semantics, deflate, crc32, zstd one-frame decode with window limit, zstd compress). Vendored yyjson 0.13.0 (`third_party/yyjson/`, sha256 of release tarball in `VERSION`, ATTRIBUTION row); Makefile gained `THIRD_PARTY` (built without our warning flags) and `PKGS = zlib libzstd libblake3`. Reference vectors from the real crates via `tools/rustref` (serde_json 1.0.149 [registry; lock says 1.0.151, same zmij formatter], half 2.7.1, base64 0.22.1): f16 exhaustive (65,536) + 2M random conversions, serde_json float text for 200k random f64 and f32, Value serialisation compact/pretty, 16 base64 cases, JSON error positions. 13 test executables, 68 tests, 11,746 checks green under `make check`, `make sanitize`, `make tsan`, clang 23 strict, `make analyze` (12 files). D7 switched cJSON → yyjson (u64/int-vs-float fidelity). Open: the JSON error-wording differences in §7 could be narrowed by mapping more yyjson messages if any caller exposes them.
- 2026-10-09 · B2 follow-up · Extra verification: a 32-bit (`-m32`) strict build compiles cleanly; with x87 maths one JSON test failed (excess precision), so D22 now rejects `FLT_EVAL_METHOD != 0` and adds `-ffp-contract=off`; with `-msse2 -mfpmath=sse` the 11 test executables that link (32-bit zstd/blake3 aren't installed here, so test_compress/test_hash weren't run 32-bit) pass.
- 2026-10-09 · B3 (in progress) · `src/ui/spike/` (gl33 loader via SDL_GL_GetProcAddress with our own Khronos typedefs, FreeType RGBA glyph atlas as an nk_user_font with HiDPI scale, Nuklear GL 3.3 renderer with scissor clipping, SDL3 event mapping incl. pen axis/touch and SDL_ShowOpenFileDialog via a user event, minimal PNG writer, `--snapshot` mode); vendored Nuklear v4.13.3 (`third_party/nuklear/`, our config in `photocraft_nuklear.h`, 32-bit draw indices). Makefile: `GUI_PKGS = sdl3 freetype2` applied to UI objects only; `make spike`; spike built by `all`/`check` when the packages exist; included in `make analyze` (17 files clean). Verified on Linux/Mesa 26.2 GL 4.6 core via the offscreen driver: snapshots at scale 1, 1.5 (clang build) and 2 render correctly (Inter text crisp at 2x, Latin-1/Extended-A, quotes, dash, euro; checkbox/property/combo/edit/progress; pressure-varying stroke); gcc + clang strict, ASan/UBSan clean except a 256-byte leak inside SDL3/Mesa that a bare SDL GL context reproduces (not ours). NOT verified: interactive window, pen pressure/tilt, file dialog, text input — the installed libsdl3 3.4.18 has no X11/Wayland driver (USE lacked X/wayland/dbus; drivers: offscreen, dummy, evdev). macOS/BSD untested. Spike limitations for U1 to fix: fixed glyph set (no on-demand atlas/CJK), no HarfBuzz shaping, no kerning.
- 2026-10-09 · B3 (in progress) · libsdl3 rebuilt with `USE="X dbus"` (drivers now x11, offscreen, dummy, evdev; XInput2 via libXi). User ran the spike interactively on X11: window, sharp text, checkbox/property/combo and typing into the edit field all work (screenshot). Canvas didn't draw because it only accepted pen events and no tablet is attached ("Pen: not seen yet"); added fixed-width mouse drawing (ignoring SDL's pen-synthesised mouse events). Pen pressure/tilt can't be verified without a tablet: carried to P2 as an open item. File dialog still to be tried.
- 2026-10-09 · B4 · `tools/corpus.sh list|fetch|verify` (POSIX sh, tested with mksh): pins in `tools/corpus/pins.txt` and manifests `tools/corpus/*.sha256` copied from xtask (now authoritative; xtask's copies are frozen); PngSuite now sha256-pinned (xtask fetched it unverified). Fetched all 7 corpora (942 files, 117 MB) in ~16 s; re-fetch skips; a tampered file fails `verify` and `fetch` repairs it. `tools/check-layers.sh` (Rust direct-dependency graph + "GUI headers only in src/ui, src/app") runs first in `make check`; a planted violation is caught on both rules. `tools/rustref` gained `psd-info`, `decode`, `composite` (links crates/psd, codecs, io, compose; errors as {"error":...}); needs rustc 1.95, installed side-by-side with `rustup toolchain install 1.95.0 --profile minimal` and pinned by `tools/rustref/rust-toolchain.toml` (the user's default stable 1.91 untouched). `tools/golden.sh` wrote 915 goldens (5.7 MB) to `tests/golden/`: psd-info for 737 PSDs (2 record the reference's truncation errors) and decode for 178 PngSuite files (16 corrupt-file errors). `make corpus` added. All gates green. Not done: composite goldens (a tolerance/size policy is needed first, decide in C1); `make test-corpus` arrives with the first module that has corpus tests (F6).
- 2026-10-09 · F1 · `src/geom/geom.{h,c}` (size, half-open rect with the reference's saturating/wrapping edge rules, tiles with floor division and an inclusive tile-range API replacing the Rust iterator, point, affine) and `src/geom/warp.{h,c}` (17 styles with names/parse incl. Unicode white space, preset maps, Bezier mesh fit/eval/split/merge/param_at/bounds, warp map/identity/to_mesh/then_affine/output_bounds). Every Rust test ported (lib.rs 8, warp.rs 6) plus edge cases (from_xywh wrap, extreme tiles, malformed meshes never crash and evaluate to NaN, patch clamps, then_affine onto itself without leaking). Bit-exact parity: `rustref geom-vectors` sweeps all styles x 5 bends x 2 distortions x 2 orientations over a 15x15 grid in and outside the box plus mesh/affine operations; the C hashes match (gcc, clang, 32-bit SSE). Also: `pc_i32_sat_add/sub`, `pc_u32_as_i32` in num.h; `pc_fail`/`pc_fail_errno` became macros (analyser couldn't see through variadic calls; it had flagged impossible paths in warp.c). Tests: 17 geom tests / 226 checks; all gates green (check, sanitize, tsan, analyze 19 files, clang). Open: serde (de)serialisation of Warp/WarpStyle/Rect belongs to C5 (manifest); geom has no corpus tests.
- 2026-10-09 · plan · Moved F2 (color) after F4: `crates/color/src/convert.rs` uses photocraft-cms (Transform, Profile, Builtin, synth::cmyk_profile), so cms must exist first. Order is now F1, F3, F4, F2, F5.
- 2026-10-09 · F3+F4 · `src/cms/`: cms (intents, signatures), math (D50, Bradford, Lab), curve, clut (tetrahedral/4-D/multilinear), pipeline (+optimize), profile (ICC v2/v4 reader, stage builders, gray_as_rgb, encode/hash), transform (refcounted; shaper, grid and exact cores; u8/u16/f32 paths through `pc_pool`; BPC; proofs; 64-entry cache), gamut, write (byte-identical ICC v4.3 writer), builtin (10 built-ins via `pthread_once`), synth (CMYK model, LM separation, GBD, B2A), lut3d, lutfile (.cube/.3dl/.look with Rust's float grammar, 8 built-in looks). Also `src/base/utf8` (lossy UTF-8/UTF-16BE like Rust), `src/base/rmath` (Rust std's own cbrt/cbrtf), `pc_f64_to_f32`; the shipped CMYK profile is embedded from `data/profiles/` at build time. Bit-exact with Rust (`rustref cms-vectors`): built-in profile bytes, transform/proof/curve/clut/profile/gamut/lut3d/lutfile/synth hash sweeps, the full CMYK regeneration (BLAKE3 equal, slow test `PHOTOCRAFT_SLOW_TESTS=1`), and `icc-info` for all 16 system ICC profiles on this machine (`tests/golden/icc-info/system.txt`). Every unit and integration test ported (`tests/cms/test_units.c` 27, `test_cms.c` 19, `test_parity.c` 10; 5,000+ checks) except the two moxcms oracles (Rust dev-dependency; bit parity covers them) and bench.rs (goes to Z1 perf). The synth mismatch came from the Rust-specific maths in §4 (−0.0 sums, `pow` rewrites, own cbrt). All gates green: check, sanitize, tsan, analyze, clang strict. Open: none for cms; the perf numbers for `bench_srgb8_to_cmyk8` come with Z1.
- 2026-10-09 · F2 · `src/color/`: color (sample types, colour modes, pixel formats, bounds-checked sample read/write, dither noise, `pc_color` → sRGB/RGBA8, serde variant names), blend (28 modes, labels, PSD keys, separable/HDR channel math, W3C non-separable modes, composite), convert (refcounted document CMYK space with a content-keyed cache and per-thread `_Thread_local` scope, managed and naive CMYK, sRGB transfer, D50 Lab, gray). `src/base/rmath.h` gained Rust min/max helpers (see §4). Bit-exact with Rust (`rustref color-vectors`: all modes × 19 edge values incl. ±0/inf/NaN, 84k random RGB blends + composites, transfer curves, Lab both ways, naive/managed/document CMYK, dither, samples at all depths, colours in all modes). Every Rust unit test ported (`tests/color/test_color.c` 25 tests incl. per-thread scopes and cache sharing under 4 threads; `test_parity.c` 5). Found and fixed two Rust-specific maths quirks (constant-operand max keeps +0.0; folded `powi`). All gates green: check, sanitize, tsan, analyze, clang strict. Open: none.
- 2026-10-09 · F5 · `src/raster/`: interrupt (cancel/progress callbacks), pixel (encode/decode, to/from RGBA, convert_pixel, RGBA8 image), surface (refcounted copy-on-write tiles in a hash table; reads by pixel, channel, strided row, region, RGBA float/8-bit with the reference's fast paths; writes by pixel, region, interleaved bytes; shared-tile fill; content/tile bounds with ring-order skipping; prune; take/put tiles; translate with tile sharing for whole-tile moves; format conversion). Bit-exact with Rust (`rustref raster-vectors`: 9 formats × region/interleaved/RGBA/RGBA8/strided/pixel reads, 6 translations, conversion to all 9 formats, take/modify/put/fill/prune: 36 hashes, all equal on the first run). Every Rust test ported (`tests/raster/test_raster.c` 22 tests incl. both proptests as 300 seeded cases each, a random insert/remove model check of the tile table, limits and bad lengths; `test_parity.c`). All gates green: check, sanitize, tsan, analyze (fixed 2 analyser findings), clang strict. Open: perf of `convert` on 24–36 MP images not measured yet (Z1); Phase 1 now continues with F6 (psd).
- 2026-10-09 · plan · F6 and F7 done as one unit: `resources` needs F7's descriptor and `file` needs F7's layer/tagged, so neither half builds alone. `tagged`→`tiff` (F8) is only a doc reference.
- 2026-10-09 · B2 correction · `pc_inflate`/`pc_inflate_count` now fail with "incomplete deflate stream" when the input ends before the stream does (and before the limit): flate2 1.1.10, the version the workspace locks, returns UnexpectedEof there; B2 had assumed the older Ok(0) behaviour. Found by the psd truncation sweep (a cut Adler-32 trailer parsed in C, failed in Rust). `tests/base/test_compress.c` updated.
- 2026-10-09 · F6+F7 · `src/psd/`: types (raw enums, rect, owned bytes), io (bounds-checked reader with the reference's exact error messages, sticky-error writer, Pascal/Unicode/legacy names), header, compression (PackBits, RLE tables, zip, 8/16/32-bit prediction, validation, 8 GiB decode cap), descriptor (full ActionDescriptor tree, depth-limited), resources (+ resolution, version info, typed views), tagged (raw blocks with padding detection, typed views, constructors, strict structure checks for PlLd/SoLd/lfx2/lnk*/FEid), filter_effects, layer (records, masks incl. the Photoshop-vs-spec field-order heuristic, channels, layer info), file (parse/write incl. Lr16/Lr32 global-block placement, open/save, accessors), pixels (RGBA8 previews), misc (hdr toning, shmd, paths and vector masks, layer tree), slices (v6 binary, v7/8 descriptors), builder. `base/utf8` gained `pc_utf8_debug_quote` (Rust `{:?}` of a lossy string; cms now uses it). Verified: all 737 `psd-info` goldens match byte for byte (incl. 2 error messages) and every parsable corpus file round-trips byte-identically (`tests/psd/test_corpus.c`); `rustref psd-vectors` (golden `tests/golden/psd-vectors.txt`): all 210 testgen cases match on decoded planes, RGBA8 previews, masks and layer-tree shape, the 84 zlib-free ones byte for byte, plus 16 builder documents and the slices/path/filter-effects/descriptor/HDR writers byte for byte. Every unit test of the F6/F7 modules ported (`test_codec.c` 29, `test_layer.c` 14) and all psd integration tests (`test_integration.c` 26: roundtrip, builder, tree, malformed incl. truncation sweeps and mutation fuzz, proptests as seeded loops, layer_block_order). All gates green: check, sanitize, tsan, analyze (fixed 8 analyser findings), clang strict. Open: F8 (layered TIFF) and F9 (ABR/PAT/ACO/ASE/GRD).
- 2026-10-09 · F8 · `src/psd/tiff.{h,c}`: ImageSourceData (37724) parse/write in both byte orders via a field-by-field transcoder (every known block layout: descriptors, TySh, legacy effects/adjustments, LMsk, grdm, vmsk, FEid, shmd, lnk2, Anno, patterns, RLE counts follow the container, samples stay big-endian), MIB8/46B8 unknown blocks kept for little-endian saves and dropped with warnings otherwise, 34377 resources, `from_psd`/`into_psd`. `pc_psd_warnings` + `psd_warn` moved to the shared psd types/io. Verified: `rustref tiff-vectors` (golden `tests/golden/tiff-vectors.txt`) matches for all 144 zlib-free testgen cases (both orders, re-parse, re-write, resources); every tiff.rs unit test ported (`tests/psd/test_tiff.c`, 16 tests incl. pattern_block).
- 2026-10-09 · F9 · `src/psd/{aco,ase,grd,patterns,abr}.{h,c}`: .aco v1/v2, .ase (groups, unknown models kept), .grd v5 (all colour classes, f32 maths as the reference), Patt/Pat2/Pat3 blocks and .pat files (offset channel placement, all sizes validated before decoding), .abr v1/v2/v6+ (budgeted samples, patterns, presets, warnings). New `pc_psd_str` (UTF-8 + length) for names. Verified: `rustref f9-vectors` (golden `tests/golden/f9-vectors.txt`, 22 lines: writer bytes, parse dumps, every truncation and a byte flip at every position of each sample file) all match; every unit test of the five modules ported (`tests/psd/test_assets.c`, 13 tests) except the usize::MAX allocation one (§7). All gates green: check, sanitize, tsan, analyze, clang strict. Note: a 30000² pattern rect at 8/16 bits is under the 8 GiB budget and really allocates (3.6–7.2 GB); the vectors only use it at 32 bits. psd crate (F6–F9) complete.
- 2026-10-09 · F10–F13 · `src/codecs/` (22 files): image model (layouts × sample types, conversions, normalised access, warnings), format sniffing/caps/extensions/MIME, options and limits, orientation (EXIF/XMP upright, banded rotate on the pool), fidelity warnings, PNM; PNG (libpng decode of pixels, own chunk walk for iCCP/eXIf/pHYs/text/acTL, own writer with adaptive filters, parallel deflate bands and Adam7: D26), JPEG (libjpeg-turbo; Adobe CMYK/YCCK, ICC, truncation → grey + warning); TIFF (ported IFD walker, own reader/writer for none/LZW/Deflate/PackBits and predictors 2/3, pages and SubIFDs, BigTIFF, Photoshop tags, camera-raw refusal: D25); WebP (libwebp + mux/demux), flat EXR (OpenEXRCore) and deep EXR (ported walker: D27), HEIF (libheif), GIF/BMP/TGA/ICO/QOI/HDR in-tree (D24), web export (indexed/animated GIF, WBMP, JPEG). Makefile: codecs `PKGS`, optional `WITH_EXR`/`WITH_HEIF` (the defines now also reach `tests/codecs/`). Every codecs integration test and unit test ported: 15 executables in `tests/codecs/`, 263 tests, 7,093 checks (PngSuite 162 files vs libpng; deep-EXR, HEIF corpora). Bugs found by the tests and fixed: TIFF associated alpha was never made straight; a failed TIFF decode freed an uninitialised image; TIFF byte counts were stricter than the tiff crate's; heif.c doubled its "HEIF:" prefixes, dropped libheif's trailing newlines, and decoded truncated grids best-effort. Analyzer work: the `cx_malformed`/`cx_unsupported`/`cx_encode` macros now evaluate to the code like `pc_fail` (the analyser had followed impossible "error returned OK" paths), plus guards in deep_exr/gif/image/tiff. All gates green: check, sanitize, tsan, analyze (79 files), clang strict, and a `WITH_EXR=0 WITH_HEIF=0` strict build. Open: HEIF chroma upsampling vs Apple/heic-rs (§7, needs a decision); `tiff_bench.rs` → Z1; no rustref codecs-vectors golden yet (encoders differ by design; decode parity is covered by the ported oracle tests and PngSuite). codecs crate complete.
- 2026-10-09 · F14–F16 · `src/raw/` (17 files): TIFF structure reader (lazy entries, bounded walk), lossless JPEG (T.81 process 14, fast lookup tables, restarts), strip/tile planes decoded in parallel, colour (DNG camera -> XYZ D50 model, McCamy CCT, Bradford, calibration interpolation), measured camera profiles (Nikon D4), opcodes (GainMap), DNG, CR2 (slices, masked black, measured CFA phase + model table), TIFF/EP (NEF/ARW/PEF, Nikon padding trim), Nikon Huffman NEF, Sony cRAW, Panasonic RW2, Olympus ORF, embedded previews, bilinear / Malvar-He-Cutler / AHD demosaicing, development (white balance incl. grey world, gain maps, gamma 1.8 LUT with tone curve, orientation). `tests/raw/testgen.h` ports the generator byte for byte. Parity: `rustref raw-vectors` (golden `tests/golden/raw-vectors.txt`, 256 lines) checks 43 synthetic files' bytes, every decoded sensor field, developed output for all three demosaicers and 5 develop options, and truncation + mutation sweeps over 8 files — all equal on the first run (gcc and clang). Notes: LLVM turns `2f64.powf(x)` into `exp2(x)`; `-ffp-contract=off` matters for the AHD/colour sums. Tests: `test_parity.c` (3 / 257), `test_units.c` (46 unit tests / 631), `test_integration.c` (dng, hostile, vendor_codecs, vendors: 38 / 292). All gates green: check, sanitize, tsan, analyze (96 files), clang strict. Open: no raw corpus (`corpus/pixls` isn't in the pinned set; the one corpus-only CR2 white-balance test isn't ported). raw crate complete.
- 2026-10-09 · F17 · `src/affinity/` (11 files): container (header, FAT chain with cycle and revision limits, zlib / zstd multi-frame extraction with the byte predictors and CRC), tagged object stream (value budget, depth limit, shared / inline / positional objects), typed access, document model (spreads, pages, artboards, layers, groups, masks, blend modes, warnings), curves with live corners, parametric shapes, paints / strokes / colours, text runs, pixel layers and placed images (tiles, crops, CMYK), embedded documents, the indexed PNG preview. Parity: `rustref affinity-vectors corpus/affinity` (golden `tests/golden/affinity-vectors.txt`, 84 lines) hashes every field of the imported model for all 21 public documents, their previews, 64 truncations and 40 seeded mutations each — all equal (the mutation hash folds the three damaged-entry messages, §7). Notes: LLVM turns `0.5f64.powf(x)` into `exp2(-x)` (gradient bias). Tests: `tests/affinity/test_affinity.c` (every unit test of tests.rs incl. the proptests as seeded loops, plus real_files.rs: 17 / 120), `test_parity.c` (1 / 86); `tests/affinity/synth.h` ports the container builder. All gates green: check, sanitize, tsan, analyze (106 files), clang strict. affinity crate complete; **Phase 1 (F1–F17) complete.**
- 2026-10-09 · D1+D2 · `src/doc/` (22 .c, 16 public headers, ~14k lines): core (ids with a shared atomic counter, locks, labels, layer masks, effects container, smart objects/filters, layer content, layers, guides, alpha channels, metadata), document (tree queries and edits: walk, path_of, locks, lookups, unused link group #961, insert/remove/shift, layer and copy names; every walk iterative, so 10,000-deep groups work), adjust, effects, fill, mode, blend_if (+advanced), vector, text (+run normalisation), text_styles (resolution/overrides/restyle through serde Values), analysis (Rust `{}`/`{:.4}` formatting, incl. Display's round-half-up shortest digits), slices (auto partition, numbering, Unicode base names), comps/artboards, variables, video/timeline, pattern (deterministic ids), and serde JSON for all 101 serialisable types (table-driven, `json_util.c` + 4 type-table files; `to_value` mode for f32 widening). Base gained `pc_str`, `pc_blob`, `pc_json_value` (normalised serde_json::Value), `pc_unicode_is_alphanumeric` (table generated by `rustref unicode-alnum` from Rust 1.95), and `pc_json_parse` now reads `-0` as -0.0 like serde_json; geom gained `pc_warp_clone`/`pc_warp_eq`; codecs' blob type renamed `pc_meta_blob`. Parity: `rustref doc-vectors` (golden `tests/golden/doc-vectors.txt`, 2,279 lines: 343 serialisations of every type/variant, 13 pretty, 4 to_value, 332 deserialisations incl. defaults and error messages, 1,587 logic vectors over 86 functions) — all match. A side probe of 34,110 mutated inputs against serde_json matched except messages cut at 255 bytes. Tests: `tests/doc/` test_core (38 / 22,026), test_data (38 / 569), test_data_slices (4 / 45), test_json (21 / 58,153), test_text_styles (11 / 110), test_parity (1 / 2,280); `tests/base/test_str_blob.c` (5 / 62); every Rust doc test ported. All gates green: check, sanitize, tsan, analyze (131 files), clang strict, `WITH_EXR=0 WITH_HEIF=0` strict, layers. **Phase 2 complete.**
