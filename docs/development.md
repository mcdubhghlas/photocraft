# Development guide

PhotoCraft is written in **ISO C17 + POSIX.1-2008** and built with one POSIX `Makefile`. See
[`REWRITE_PLAN.md`](../REWRITE_PLAN.md) for the authoritative plan and
[`AGENTS.md`](../AGENTS.md) for the product rules.

## Prerequisites

- A C17 compiler (gcc or clang) and **GNU make ≥ 3.81** (`gmake` on the BSDs; macOS ships 3.81) and
  `pkg-config`.
- Required libraries (development packages): SDL3, FreeType, HarfBuzz, FriBidi, libunibreak, libpng,
  libjpeg-turbo, libtiff, libwebp, zlib, libzstd, libblake3.
- Optional libraries, auto-detected and overridable as `make` variables: OpenEXR (`WITH_EXR`),
  libheif (`WITH_HEIF`), libjxl (`WITH_JXL`), xcb/xcb-randr/xcb-image (`WITH_X11SCREEN`), dbus-1
  (`WITH_COLORD`), and WAMR for plug-ins (`WITH_PLUGINS`, vendored).
- For interactive use on Linux, SDL3 must be built with X11 and/or Wayland support (on Gentoo,
  `USE="X wayland dbus"` on `media-libs/libsdl3`; `dbus` lets file dialogs use the desktop portal).

Vendored libraries (yyjson, Nuklear, pocketfft, WAMR) live in `third_party/` and are built
automatically; no download step is needed for them.

## Build and run

```sh
make                                          # build everything into build/default/
make -j8                                      # parallel build
build/default/bin/photocraft image.psd        # desktop app
build/default/bin/photocraft-cli info image.psd
make clean
```

`make` builds the static library `build/default/libphotocraft.a` and these programs under
`build/default/bin/`:

- `photocraft` — the desktop application (SDL3 frame loop + control server).
- `photocraft-cli` — the headless command line (installed by `make install`).
- `photocraft-ui-shell` — the development shell root (interactive, or one offscreen snapshot).
- `photocraft-ui-snapshot` — renders one themed widget panel offscreen (design reviews).
- `photocraft-spike` — the GUI-stack spike (`--snapshot OUT.png [--scale S]`).
- `photocraft-parity` — the headless menu-parity report generator (used by `tools/parity.sh`).

The GUI programs are built only when SDL3 and FreeType are found. Override the build directory with
`BUILD=...` (the default is `build/default`); `make check` and the sanitizers use their own dirs.

Optional features:

```sh
make WITH_HEIF=0 WITH_EXR=0        # force a feature off
make WITH_PLUGINS=0                # build src/plugins without WAMR (loading reports "unavailable")
```

The desktop app's flags: `photocraft [--control <port>] [--control-token <64hex> |
--control-token-file <path>] [--safe-gpu] [--scale S] [--theme ID] [--font PATH] [--snapshot OUT.png]
[--open FILE | FILES...] [--version]`.

## Test

```sh
make test          # build and run every tests/<module>/test_*.c
make check         # layer check + strict-warning build (-Werror + the full warning set) + test
make sanitize      # the suite under ASan + UBSan (-fsanitize=address,undefined)
make tsan          # the suite under ThreadSanitizer (for threaded code)
make analyze       # clang static analyser (or cppcheck) over the sources
make valgrind      # the suite under valgrind (skipped where the CPU/libc make it impossible)
```

`make check` is the final gate before a change lands; it runs `tools/check-layers.sh` first, then
rebuilds everything with strict warnings as errors and runs all tests. For threaded code, `make tsan`
too; for untrusted-input parsers, `make analyze`.

While iterating, build and run a single test binary directly — each `tests/<module>/test_*.c` compiles
to its own executable linked against `libphotocraft.a`:

```sh
make build/default/tests/psd/test_parity        # build one test
./build/default/tests/psd/test_parity           # run it
```

### The test harness

`tests/harness.h` is the whole framework — one header, no dependencies. It provides `CHECK`,
`CHECK_EQ_INT` (signed), `CHECK_EQ_UINT` (unsigned), `CHECK_NEAR`, `CHECK_EQ_STR`, `CHECK_EQ_MEM`,
`CHECK_CONTAINS` and `RUN(test_fn)`. Each test file is one translation unit with a `main` that calls
`RUN(...)` for each test and returns the failure count. See the header's own comment for the exact
API.

Commands must **never crash** on bad input. New commands come with a graceful-failure test
(empty / out-of-range / wrong-type params → error, not an abort), and the engine's `panic_hunt` test
fuzzes every command with adversarial params. Format modules use round-trip, synthetic-generator,
oracle and malformed-input (truncate-at-every-offset) tests. Pixel code is tested at 8, 16 and 32-bit.

### GL and offscreen snapshots

GUI tests and snapshots run **headless** with `SDL_VIDEO_DRIVER=offscreen` (EGL + Mesa): no display,
no window, no focus stealing, and the same works in CI. Tests that need a real GL context are opt-in
and gated on the `PC_TEST_GL` environment variable, so the ordinary suite stays displayless:

```sh
PC_TEST_GL=1 SDL_VIDEO_DRIVER=offscreen ./build/default/tests/ui/test_shell_control
```

Render a UI frame to a PNG without a window:

```sh
SDL_VIDEO_DRIVER=offscreen build/default/bin/photocraft --snapshot /tmp/ui.png --scale 2 --open photo.jpg
SDL_VIDEO_DRIVER=offscreen build/default/bin/photocraft-ui-snapshot   # one themed widget panel
SDL_VIDEO_DRIVER=offscreen build/default/bin/photocraft-spike --snapshot /tmp/spike.png --scale 1.5
```

Verify every visual change this way (or with `ui.screenshot` on a `--control` session), at several
window sizes and in every theme. Demo images must be public-domain art.

## Shell tooling (tools/)

The `tools/*.sh` scripts replace the Rust `cargo xtask` subcommands. They are POSIX sh.

| Tool | What it does |
|---|---|
| `tools/check-layers.sh` | Enforce the module dependency graph (REWRITE_PLAN §3). Run first by `make check`. |
| `tools/corpus.sh list\|fetch\|verify [all\|<name>]` | Fetch/verify the pinned, sha256-checked real-file corpora into `corpus/`. `make corpus` = `tools/corpus.sh fetch all`. |
| `tools/parity.sh [--stdout]` | Regenerate `docs/parity-checklist.md` from the C menu catalog (`src/ui/menu_catalog.c`) and the live command registry, via `photocraft-parity`. `make parity` runs it. |
| `tools/scorecard.sh [--check]` | Regenerate `docs/scorecard.md` from committed sources (`scorecard/*.toml`, `perf/budgets.toml`, `perf/baseline.json`, the corpus floors in `tests/io/`, the prefs audit). `--check` fails if stale. `make scorecard` runs it. |
| `tools/stats.sh` | Count C source/header lines and `RUN(test_*)` registrations per module. `make stats`. |
| `tools/perf.sh [--quick] [...]` | Performance runner. **No C bench harness exists yet**: it validates arguments and lists the scenarios/budgets but does not run benchmarks. `make perf`. |
| `tools/golden.sh CMD PATH...` | Record reference outputs from the Rust tree as committed goldens in `tests/golden/` (needs Cargo + `tools/rustref`; only usable while the Rust reference still exists). |

`docs/parity-checklist.md` and `docs/scorecard.md` are generated — never edit them by hand; regenerate
and commit the result (CI checks the scorecard is current).

## Fonts (craft-fonts)

Font assets shared by the Crafting Apps live in
[storytold/craft-fonts](https://github.com/storytold/craft-fonts), never in this repo (Inter and
JetBrains Mono under `assets/fonts/` are the only exceptions). The rules are in
`../craftrules/standards/fonts.md` (a sibling checkout; the repo is not public, so ask a maintainer).

craft-fonts is read **at run time** from the `CRAFT_FONTS_DIR` environment variable — it is not a
build step:

```sh
git clone https://github.com/storytold/craft-fonts ../craft-fonts
CRAFT_FONTS_DIR="$PWD/../craft-fonts" build/default/bin/photocraft
```

`src/text/craft_fonts.c` reads `$CRAFT_FONTS_DIR/fonts/manifest.txt` and loads the fonts at startup.
A build define `PC_CRAFT_FONTS_DIR` sets a default path. Unset, no craft-fonts are loaded and the app
falls back to the system's CJK fonts (and everything still works). A bad path warns, or errors when
`CRAFT_FONTS_REQUIRED` is set. Tests that need the fonts skip cleanly when they are absent.
`PHOTOCRAFT_NO_SYSTEM_FONTS` makes the text engine ignore system fonts (used by tests).

## Driving the app programmatically

Start the app with a private token file; it then accepts authenticated JSON lines on
`127.0.0.1:<port>` (loopback only):

```sh
build/default/bin/photocraft --control 7878 --control-token-file .private/control.token &
TOKEN=$(tr -d '\r\n' < .private/control.token)
printf '%s\n' "{\"id\":\"auth\",\"method\":\"auth\",\"params\":{\"token\":\"$TOKEN\"}}" \
  '{"id":1,"method":"engine.execute","params":{"command":"layer.newAdjustmentLayer.hueSaturation","params":{"hue":30}}}' \
  '{"id":2,"method":"ui.screenshot","params":{"path":"evidence/shot.png"}}' | nc 127.0.0.1 7878
```

The TCP transport is `src/app/control_server.c`; the `ui.*` handlers are in `src/ui/shell/control.c`.
See [`control-protocol.md`](control-protocol.md) for every method. `ui.pointer` takes events in
*document* coordinates and runs them through the same tool state machines as real input.

## Headless CLI

`photocraft-cli` drives the engine without a window:

```sh
photocraft-cli convert in.psd out.pcraft                 # any supported format -> any
photocraft-cli info out.pcraft                            # JSON: size, mode, depth, layer tree
photocraft-cli run in.png --cmd layer.new.layer --params '{"name":"Ink"}' \
                          --cmd filter.blur.gaussianBlur --params '{"radius":3}' --out out.psd
photocraft-cli run --new '{"width":800,"height":600}' --cmd document.inspect
photocraft-cli batch --actions actions.json --in photos/ --out done/ --format jpg
photocraft-cli droplet grade.pcdroplet in/ --out out/
photocraft-cli commands --filter blur                    # the command registry
```

`actions.json` holds the steps of a recorded action (`[["<id>", {…}], …]`, `[{"command":"<id>",
"params":{…}}, …]`, bare ids, or wrapped in `{"actions":…}`/`{"steps":…}`); an Actions set uses
`--action <name>`. Binary Photoshop `.atn` files are not yet supported by the CLI (the engine's `.atn`
importer is not exported). `batch` refuses an `--out` folder equal to `--in` unless `--in-place` is
given.

## MCP (agents)

`photocraft-cli mcp` serves MCP on stdio using `src/automation` — an own JSON-RPC implementation, no
async runtime. In **headless** mode it drives an in-process engine session; in **bridge** mode it
forwards every tool to a running desktop app over the control protocol:

```sh
# headless, with capability-scoped file roots
photocraft-cli mcp --automation-read-root /work --automation-write-root /work
# bridge to a running app
photocraft --control 7878 --control-token-file .private/control.token &
photocraft-cli mcp --bridge 127.0.0.1:7878 --control-token-file .private/control.token
```

Omitting a root flag denies that direction of filesystem access. Tools: `session_list`; `doc_open`,
`doc_new`, `doc_place`, `doc_export_layers`, `doc_export_data_sets`, `doc_import_data_sets`,
`doc_save`, `doc_export`, `doc_inspect`, `doc_render_preview`, `doc_select`, `doc_close`;
`command_list`, `command_run`, `command_batch`; `jobs_list`, `jobs_cancel`; and (bridge only)
`ui_inspect`, `ui_screenshot`, `ui_pointer`, `ui_menu_invoke`, `ui_set`, `control_call`. See
[`control-protocol.md`](control-protocol.md#mcp-bridge). Without MCP, `photocraft-cli serve
[--port N]` keeps a headless session open and answers the same JSON-lines envelope.

## Native format (.pcraft)

`src/format` defines the lossless native bundle: a ZIP of STORE entries, or a directory with the same
layout — `manifest.json` (the versioned document tree, with migrations), `tiles/<blake3>.zst` and
`blobs/<blake3>.zst` (zstd-compressed, content-addressed objects), and PNG previews. Re-saving writes
only changed objects, garbage-collects unreferenced ones, and is atomic (temp file + `fsync` +
`rename`). Autosave and crash recovery write snapshots into a recovery directory on a background
thread. `src/io` routes `.pcraft` through this module on import/export.

## Logs and environment variables

The desktop app writes `log` records to standard error and to a rotating log file in the config
directory (Linux `$XDG_CONFIG_HOME/photocraft/` or `~/.config/photocraft/`; macOS
`~/Library/Application Support/Photocraft/`). A start from a desktop menu has no terminal, so that
file is what to attach to a bug report.

| Variable | Effect |
|---|---|
| `PHOTOCRAFT_LOG` | Log level for stderr and the log file (`src/base/log.c`) |
| `PHOTOCRAFT_CONTROL_PORT` | Same as `--control <port>` |
| `PHOTOCRAFT_CONTROL_TOKEN` / `PHOTOCRAFT_CONTROL_TOKEN_FILE` | Control bearer token / token file (avoid the inline token on shared systems) |
| `PHOTOCRAFT_THREADS` | Override the thread-pool size |
| `PHOTOCRAFT_LOCALE` | Override the UI language for this launch (unsupported tags fall back to English) |
| `PHOTOCRAFT_NO_PRINT` | Make `file.print` / `file.printOneCopy` refuse to spool (`dryRun` still works); CI sets it |
| `PHOTOCRAFT_NO_SYSTEM_FONTS` | Ignore system fonts (tests) |
| `PHOTOCRAFT_FX_NOCACHE` | Bypass the CPU layer-effect map cache (parity checks) |
| `PHOTOCRAFT_SKIP_LIB_CHECK` | Skip the startup shared-library preflight |
| `CRAFT_FONTS_DIR` / `CRAFT_FONTS_REQUIRED` | craft-fonts location / treat a bad checkout as an error |
| `SDL_VIDEO_DRIVER=offscreen` | Headless GL for snapshots and `PC_TEST_GL` tests |
| `PC_TEST_GL` | Opt in to the GL-backed UI tests |
| `PHOTOCRAFT_SLOW_TESTS` | Run the timing/slow tests that are otherwise skipped |

## Graphics startup and device loss

`--safe-gpu` starts with the CPU renderer for one launch. Before creating the GL context the app
writes and locks a `gpu-starting.json` marker in the config directory and clears it once the first
frames render; a launch that finds an unlocked marker knows the previous start died inside the driver
and records that for recovery. The SDL/GL app has a single GL path (no wgpu backend chain), so the
Rust reference's adapter-ranking and DX12 specifics do not apply (REWRITE_PLAN §7, P1). If the GL
device is lost while running, the canvas switches to the CPU compositor and posts a notice;
`ui.gpu.simulateLoss` triggers this from the control channel.

## Test corpora

Real files are the best oracles but are large binaries, so they never enter this repository. They live
in `corpus/` (gitignored), fetched at **pinned commits** and verified against committed **sha256
manifests** (both under `tools/corpus/`). Fetch and verify with:

```sh
tools/corpus.sh list               # where each corpus lives, its pin, present or missing
tools/corpus.sh fetch all          # fetch everything missing or stale (verified copies are left alone)
tools/corpus.sh verify all         # re-verify
make corpus                        # = tools/corpus.sh fetch all
```

The corpora include our own Photoshop-authored oracle PSDs
([photocraft-corpus](https://github.com/storytold/photocraft-corpus)) and the psd-tools, ag-psd and
PngSuite sets. The corpus tests skip cleanly when `corpus/` is absent; once fetched, `make check`
(and CI) runs them. Never commit corpus files; new oracles go to photocraft-corpus, then the pin is
bumped here. Run the corpus tests when you touch `psd`, `io`, `codecs`, `compose`, `gpu`, `text` or
`format`.

## Reference outputs (while the Rust tree exists)

`tools/rustref/` is a temporary Cargo project that links the reference crates and dumps
machine-readable outputs (decoded pixels, composites, metadata, command results) for differential
tests. `tools/golden.sh CMD PATH...` records them as committed JSON in `tests/golden/<cmd>/` (pixel
outputs as `raw_sha256`); the C tests compare against those goldens. Both need the Rust tree and Cargo
and are used only until the reference is deleted — after that, the goldens are all that remain of it.
