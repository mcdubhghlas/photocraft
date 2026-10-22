# UI design system

## Themes

| Theme | Intent |
|---|---|
| Pro | Photoshop-style Spectrum dark: flat charcoal panels (#323232), dark tab strips, Spectrum blue accent (#378ef0), pill buttons, checkboxes, compact 12 px type |
| Pro Medium | Photoshop-style medium-gray panels and a dark canvas |
| Studio | Dark studio style: near-black, rounded cards, pill tabs, violet accent, toggles |
| Studio Light | Studio on light surfaces |
| Classic | Windows-2000 bevels, square corners, navy selection |
| Adwaita | GNOME's libadwaita light palette on the Studio layout: white header bar and cards on a grey sidebar, GNOME blue accent (#3584e4), 6 / 9 / 12 px radii, grey (not red) close button |
| Adwaita Dark | The libadwaita dark palette on the same layout |
| Solarized Dark | Ethan Schoonover's Solarized palette: base03 canvas, base02 panels, base0 text, Solarized blue accent |
| Breeze Light | An adaptation of KDE Plasma's Breeze Light on the Studio layout: cool light blue-grey surfaces a few levels from Breeze's so the app blends into a Plasma desktop, Breeze-style blue accent, 5 px radii, red close button on hover; text and status colours are darker than Breeze's for contrast |
| Breeze Dark | The same for Breeze Dark: cool dark blue-grey surfaces (darkest canvas, then dock, cards, chrome and controls) near Breeze's, blue accent, 5 px radii; secondary text and status colours are lighter than Breeze's for contrast |
| Soft Dark | Low-contrast dark on the Studio layout: muted violet-grey surfaces (never near-black), softened off-white text, periwinkle accent, generous 7/10/14 px radii; text still holds 4.5:1 on every surface (`theme.c` tests) |
| Soft Light | The light counterpart: pale violet-grey surfaces, softened dark-violet text instead of black, periwinkle accent, the same radii |

Both Breeze themes are our own values close to Plasma's (not a copy of its colour schemes) and put text readability first: every text colour holds 4.5:1 on every surface and on hovered, pressed and selected states (`theme.c` tests), so their secondary text, accent and status colours are darker (Light) or lighter (Dark) than Breeze's own, and the primary button is a deeper blue with white text.

Edit → Preferences → Interface shows an Appearance Mode selector (Sync with system, Dark, Light) above separate light and dark theme cards with PhotoCraft editor previews and radio choices. Auto is opt-in and follows the operating system while the app is running. On Linux it reads the desktop portal once and then listens for its `SettingChanged` signal (no polling, and the UI repaints only when the value changes); `gsettings` runs at most once at start-up, by absolute path, when the portal gives no answer. A missing system appearance falls back to Dark. New installs keep Photoshop's default: Dark mode with Pro Medium (Studio Light is the saved light theme). Existing saved single-theme preferences migrate to a fixed Dark or Light mode with their chosen theme. The header appearance button cycles Auto → Light → Dark; its icon shows a monitor, moon or sun for the selected mode. Window → Theme and `ui.set {"theme":"classic"}` select a theme and fix the mode to its light or dark family.

Window → Theme and macOS PhotoCraft → Appearance also offer Sync with system. Its checkmark follows the saved Auto mode rather than the displayed palette; choosing a manual theme selects its fixed appearance mode.

The toolbar's foreground and background colour chips follow the `Tokens::round_chips` flag. Pro and Classic leave it off and draw Photoshop's overlapping squares with Default Colors and Switch Colors above them. Studio, Studio Light, Solarized Dark, Adwaita, Adwaita Dark, Breeze Light, Breeze Dark, Soft Dark and Soft Light (the Studio-layout themes) turn it on and draw large round chips with a curved Switch Colors arrow centred right under them, its heads touching the chips: stacked vertically in one tool column, with Default Colors at their top-right, and side by side in two tool columns, with Default Colors at the toolbar's left edge. Another non-Pro theme can opt in by setting the flag.

## Rules

- **Colours and radii come from theme tokens.** The active theme's `pc_ui_tokens`
  (`src/ui/theme.h`, `pc_ui_tokens_for_kind()`) carries every colour and radius; widgets apply them to
  the Nuklear context with `pc_ui_apply_nk_style()` and `pc_ui_nk_color()` (`src/ui/widgets.h`). Never
  hard-code a colour in a widget. The field order and meaning of `pc_ui_tokens` mirror the reference's
  `Tokens`.
- **Shared widgets live in `src/ui/widgets.c`:** `card` (panel group; Pro renders a Photoshop tab
  strip), `value_field`, `slider`/`slider_row`, `toggle`/`checkbox`, `primary_button`/
  `secondary_button`, `dropdown` (with a chevron icon), `hairline`/`vline`, `elided_label`.
- **A label never overflows its row.** Wrap by default. Beside a fixed-width control, or anywhere the row cannot reflow, use `widgets::elided_label(ui, text, max_w)`: a longer translation shrinks with `…` and shows the full string on hover instead of running over its neighbours (#3580). A *redundant* caption may be dropped on a narrow dock where its control already carries the meaning (e.g. the Layers lock icons); an *essential* one elides. This mirrors Photoshop: a fixed layout with elision and tooltips, not per-locale reflow.
- **Icons:** Lucide SVGs in `assets/icons` (ISC licence), embedded at build time by
  `tools/embed-ui-icons.sh` (generates the icon table included via `src/ui/icon_data.h`). Add the SVG
  and rebuild; no network access or generated Rust file is a build input.
- **Fonts:** Inter (UI) and JetBrains Mono (numbers), both OFL, embedded uncompressed. The theme
  exposes the medium and semibold UI weights. CJK and other scripts come from craft-fonts at run time
  (`CRAFT_FONTS_DIR`; see `docs/development.md` › Fonts).
- **Photoshop layout grammar (Pro):**
  - Essentials dock order: Color | Swatches, then Properties | Adjustments, then Layers | Channels | Paths (Layers fills the remaining height).
  - Options-bar labels end with a colon ("Size:").
  - Document tabs read "name @ 12.5% (RGB/8)".
  - Toolbar tool groups carry a corner triangle.
- **Verify every visual change** with `ui.screenshot`, at several window sizes and in every theme.

## Adding icons

Drop the Lucide SVG into `assets/icons/` and rebuild — `tools/embed-ui-icons.sh` regenerates the
embedded icon table from every `assets/icons/*.svg` at build time:

```sh
curl -sfL --user-agent Photocraft-dev \
  -o assets/icons/<name>.svg \
  https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/<name>.svg
make        # the build re-embeds the icons; no file is hand-edited
```

Add the asset's licence/attribution row (`ATTRIBUTION.md`) in the same change.

## Interaction models (match Photoshop CC)

| Feature | Module | Behaviour |
|---|---|---|
| Type tool | `type_tool.c` | Click: point text with the placeholder "Lorem Ipsum" selected. Drag: paragraph box. Inline caret and selection drawn from the text engine layout. ⌥/⌘ word and line navigation, ↩ newline, ⌘↩ or Esc commits, a click outside commits. One history step per session (`coalesce`). A new layer is named after its text; an empty one is deleted. |
| Free Transform | `transform.c` | ⌘T. Corners and edges scale proportionally (⇧ frees them: an edge then stretches one axis); ⌥ scales about the reference point; ⌘-corner distorts; dragging outside rotates (⇧ snaps to 15°); dragging inside moves. Preview = document without the moving pixels + a textured 24×24 mesh. ↩ or a double-click commits via `edit.transform {rect, quad}`. |
| Quick layer pick | `move_mods.c` | macOS: ⌘⌥⌃-click with any tool selects the topmost visible layer with pixels under the pointer (`layer.pickAt`) without switching tools; the drag and release are swallowed. Windows/Linux have no third modifier distinct from the ⌃⌥ brush resize, so the gesture is a no-op there (use the Move tool's Auto-Select). A failed pick is a status-bar error. |
| Layers rows | `shell/panels.c` | Double-click: on the name renames in place; on the Background makes it a normal layer; on an adjustment or fill thumbnail opens its Properties; on a Smart Object thumbnail opens its contents (Edit Contents); anywhere else on the row opens Layer Style. |
| Layer masks | `shell/panels.c` | Clicking the mask thumbnail targets the mask (corner-bracket frame; the tab reads "Layer, Layer Mask/8"). Brush, eraser (paints background colour), gradient, bucket and moving the selected pixels (⌘-drag, `select.float`; the hole takes the background colour) then send `"target": "mask"`. Adjustment and fill layers target their mask automatically. |
| Path thumbnails | `vector_tool.c` | Ctrl/Cmd-click the thumbnail of a Work Path, saved path or layer path to load it as a selection; Shift adds, Alt subtracts, Shift+Alt intersects. Clicking the row name only selects the path. |
| Levels / Curves | `point_curve.c` | Histogram of the image *below* the adjustment. Curves: click to add a point, drag out to delete. Every change is a coalesced `layer.setAdjustment`, so one drag = one undo step and the canvas updates at full resolution on the GPU. |
| Shape Stroke Options | `vector_tool.c` | Direct editing: valid changes apply immediately, with one numeric drag per undo step in Properties. Escape/outside click closes the popup and retains changes; a child dropdown keeps the parent open. The options bar sets new-shape defaults; Properties edits the selected shape. |

Shape Stroke Options (updated 2026-10-11) sizes to its content and scrolls only when necessary.
It has no Apply/Cancel footer. Solid, Dashed and Dotted are full clickable rows with line
samples and visible selection; opening the editor alone never changes a shape. Invalid pending
numeric input leaves the last valid style in effect. Fill and Stroke use clickable swatches
and the existing compact colour picker, preserving alpha and offering No Color. The style
button remains available when no stroke is set; deliberately choosing a stroke colour or
style enables a 3 px stroke. Colours, borders and selection treatments come from theme tokens.
These tool defaults are serde state, but are not restored across app restarts. See
[`shape-strokes.md`](shape-strokes.md) for geometry, colour-management and persistence limits.

While editing type, hold Ctrl (Windows/Linux) or Command (macOS) for an oriented transform
frame. Drag inside to move, a corner to scale (Shift keeps proportions; Alt/Option scales about
the reference point), a middle handle to skew, or just outside the frame to rotate (Shift snaps
to 15°). Drag the reference point to move the rotation centre. These Type gestures follow
[Adobe's type guide](https://helpx.adobe.com/photoshop/using/creating-type.html), independently
of the Free Transform preference: Ctrl/Cmd never distorts a Type corner.

Without Ctrl/Cmd, paragraph handles resize the container and reflow its text. Temporary
transforms keep its logical dimensions and all character/paragraph styles. A drag begun with
Ctrl/Cmd owns the pointer until button release, even if the modifier is released first. Its
preview does not change the document; release applies one coalesced `type.edit`, and typing
continues in the same layer. Escape during a drag cancels that preview; focus loss or changes
to its source also discard it. Holding the modifier, moving the reference point, or returning
the pointer to its start creates no history entry. Ctrl/Cmd+T while typing still toggles the
Character panel.

Where the font lacks a symbol (e.g. ∠ ↦ ▔), draw it with the painter or use a Lucide icon; never ship
missing-glyph boxes. Check every new panel with the offscreen snapshot tool (`docs/development.md`).

## Title bar

The app's top bar (shell chrome, `src/ui/shell/chrome.c`) starts with the brand mark (the app icon,
`src/ui/brand.c`), then the menus, the document title and the workspace controls, like Photoshop's Ps
tile and menu row. The menu bar is an in-window bar on every platform (the macOS native menu bar is
deferred; REWRITE_PLAN §2 D19).

- **Linux:** the window can run without OS decorations, so there is one bar, not two stacked:
  Minimize, Maximize/Restore and Close (red on hover, `caption_close` tokens) sit flush in the
  window's top-right corner, the free gap between the menus and the controls drags the window, a
  double-click there maximizes it, and invisible edges resize it. Close runs File › Exit, so unsaved
  documents are asked about first.
- **macOS:** the traffic lights sit over the integrated title strip.
- **Narrow windows** drop controls that are also in a menu before anything overlaps: Discord (Help › Discord), then the theme toggle, then search (Edit › Search), then the workspace switcher narrows (Window › Workspace).

## Compact tool controls

The toolbar scrolls its tool rows when two columns still exceed the available height. Foreground
and background colours stay visible below those rows, together with Pro's Quick Mask and Screen
Mode controls. Long options rows scroll horizontally. Both scrollers bring newly focused
controls into view and retain the existing tool actions and popups.

The offscreen regression checks exercise production panels, fonts and SVG loaders across every theme,
several point viewports (e.g. 800×480 and 1280×720) and display scales (1×, 1.5×, 2×), rendered
headless with `SDL_VIDEO_DRIVER=offscreen` through the GL-backed UI tests (`PC_TEST_GL=1`, in
`tests/ui/`). Run them with the usual test gate (`PC_TEST_GL=1 SDL_VIDEO_DRIVER=offscreen` on the
relevant `tests/ui/test_*` binary; see `docs/development.md` › GL and offscreen snapshots).

Keyboard tests clear the configurable `window.togglePanels` canvas shortcut through preferences,
then send normal Tab and Space events through the app's shortcut dispatcher. They exercise
focus traversal and activation with that nondefault binding; default canvas Tab still toggles
panels. These offscreen checks do not establish native window or platform input acceptance.

## Menus

Horizontal and vertical ruler ticks, labels and pointer markers are clipped to their respective
strips. Their intersection is a clear origin square, including when the view is panned, flipped
or rotated; the origin square retains its ruler-unit context menu.

An embedding application's `panels.menu_bar` and `panels.rail` flags belong to its current
session. Restoring a saved workspace or remembered panel layout preserves both flags, including
when the serialized layout contains their values. Panel visibility, dock layout, tabs and Timeline
visibility follow the saved layout.

The Pro status bar's Document Dimensions readout follows Preferences › Units & Rulers › Rulers,
including changes made from a ruler's context menu. It uses the document resolution for physical
units, each side's own extent for percentages and the selected point-size convention for points
and picas. The readout retains the document's pixels-per-inch value.

`src/ui/menu_catalog.c` holds Photoshop's menu tree (standard command names, order, separators, default shortcuts). Items whose id matches an engine or UI command are live; others render disabled until implemented. Give new commands the catalogue's id (for example `image.imageSize`) and they light up in the right place automatically.

Menus never run off the window: the menu bar's menus and submenus scroll with arrows (`menu_nav::level`), and long right-click menus (Layers, canvas tools, Channels, Paths, document tabs) wrap their rows in `widgets::menu_scroll`, so they move up to fit and scroll only when taller than the visible window.

## Automation for visual checks

Use `ui.click {x,y}`, `ui.move`, `ui.key` and `ui.type` (synthetic input in screen points) to open menus, popups and context menus, then `ui.screenshot`.

## Font menus

The Type options bar, Character panel, Character/Paragraph Style editors and Glyphs panel
share a searchable family picker. Each visible row shows an `AaBbCc` sample rendered by
PhotoCraft's text engine in that family (script/symbol fonts use characters they support). A family whose face also covers Arabic (alef, lam, meem, ain and the sample letters) shows a short `أبجد هوز` sample left of it, shaped right-to-left by the same engine; its row reserves the extra width once that row has been painted, and Latin-only families are unchanged.
Samples are cached with a bounded cache and follow display scale and theme text colour.
Builds without system fonts preview the bundled fonts.
While the menu is open, Up/Down applies the previous/next matching family and scrolls it into
view; Enter accepts the selection and closes the menu. Escape closes it, keeping already
applied changes (Edit › Undo restores text-layer font changes).

## Preferences

Preferences has **Apply**, **OK** and **Cancel**. Apply saves the edited sections and keeps the
dialog open; it is disabled when the values match the saved preferences. OK saves and closes.
Cancel discards only changes made since the last successful Apply. A failed Apply leaves the
draft open for correction. Settings marked for the next launch still require a restart.
On Windows and Linux, System Title Bar displays “Applies at next launch.” beneath its checkbox,
including before editing and after Apply. Saving this preference does not restart the app or
change the current window's decorations; the next launch reads the saved choice.

## High DPI and 4K displays

Edit → Preferences → Interface → UI Scale applies immediately. Auto follows the operating
system's display scale, including fractional scales such as 125%, 150% and 175% (on Linux,
Wayland's fractional scale or X11's `Xft.dpi`), and updates when the window moves between
monitors. A 4K display the system runs at 100% stays at 100% (#1278). The fixed choices (75% to
300%) set an absolute UI scale, for example 200% on an unscaled 4K display. Canvas zoom shortcuts
continue to control the document independently of UI scaling.

Interface → UI Font Size changes interface text independently of UI Scale and canvas zoom.
Tiny, Small (the default), Medium and Large use 10/12, 1, 14/12 and 16/12 of the theme's original
font sizes, preserving the relative sizes of headings, captions and numeric fields. Apply or OK
updates text immediately; the choice persists across launches and theme changes. CJK fallback
fonts use the same size, including fonts loaded after the preference changes.

## Localisation

Strings in code stay English and are the default lookup keys. `src/ui/i18n` maps them to display text
at render time from one catalog per language (`src/ui/i18n/<code>.tsv`; the format is documented in
the header of `ja.tsv`, and the catalogs are embedded at build time). Command ids, menu paths used for
logic, the control channel, the CLI and MCP always use the English ids and labels.

Languages shipped: English (`en`, the source language) plus the catalogs under `src/ui/i18n/` —
`ja`, `zh-hans`, `zh-hant`, `es`, `ru`, `uk`, `sr`, `cs`, `fr`, `id`, `ko`, `pl`, `de`, `pt-br`, `el`,
`nl`, `it`, `hu`, `tr`, `vi`. The registry is enumerated by `pc_i18n_count()` / `pc_i18n_at()`;
`pc_i18n_lang_complete_menus()` marks a catalog that covers every menu string and translatable
literal (a test enforces it). The locale resolver distinguishes Simplified from Traditional Chinese
(`zh-TW`, `zh-HK`, `zh-MO`, `zh-Hant-*` all resolve to `zh-hant`), and each language's plural rule is
read with `pc_i18n_lang_plural()`.

- Lookups (`src/ui/i18n.h`): `pc_i18n_tr(lang, s)` for plain strings; `pc_i18n_tr_ctx(lang, ctx, s)`
  when one English word needs different translations; `pc_i18n_tr_id(lang, command_id, label)` for
  menu items (keyed by command id, English label as the fallback); `pc_i18n_trn(lang, n, one, other)`
  for plurals; placeholders like `{name}` are filled afterwards and may be reordered by translators.
- The language is Preferences › Interface › Language (`interface.language`: `auto` or a language code;
  `auto` follows the system locale, an unknown code falls back to `auto`). `PHOTOCRAFT_LOCALE`
  overrides it for one launch.
- Language changes apply without a restart; Preferences previews the chosen language until Apply or OK
  commits it. Cancel restores the committed language.
- To add a language: add `src/ui/i18n/<code>.tsv`, register it (code, native name, catalog, plural
  rule), and add it to the Makefile's `I18N_CATS` so its catalog is embedded. The catalog tests
  (well-formed, no duplicates, placeholders and ellipses agree, command ids exist) pick it up.
- Translations are clean-room: written from the meaning of the English text in ordinary vocabulary,
  never from another product's localisation resources.

Localised so far: menus, the command palette, dialogs and panels (widgets such as `checkbox`,
`slider_row`, `dropdown` and the buttons translate their labels themselves). A test fails when a
translatable literal, a menu string, a blend mode name, a brush section name or a generated
preference label has no entry in a language marked complete. Not translated: status-bar
messages and errors (they stay English, also for agents), names that are user data (layers, styles,
documents), and strings assembled by hand that were not routed through `pc_i18n_trn`/placeholders.
Not done yet: full right-to-left layout inside panels and dialogs, and locale-aware number and date
formats. The UI language is detected from `LANG`/`LC_*` (and the macOS preferred languages).

Camera Raw has explicit coverage across the shipped languages, including Korean and
Simplified Chinese. Its contextual `cameraRaw` entries distinguish tonal regions,
colour-band names and font-weight Light. The title uses a `{layer}` placeholder; user layer
names and the universal RGB/Lab channel symbols stay unchanged. Collapse IDs use English
source keys, so switching languages preserves open sections. Mixer tabs wrap within the
panel width. Catalog coverage and actual shell language-switch tests enforce these rules.

Native CJK font fallback follows the selected UI script and resets its cache when switching
languages.

## Arithmetic in numeric fields

Click a numeric value and type an expression, then press Enter or move focus to finish.
Numeric fields accept `+`, `-`, `*`, `/`, remainder `%`, powers `^` or `**`, parentheses,
scientific notation, and `pi` / `tau`. For example, `1920/2` gives `960`, and `(30+15)*2`
gives `90`. Shared value fields apply plain numbers live and arithmetic when editing
finishes, preserving rounded/integer workflows. Field limits and integer rounding
still apply; invalid expressions, division by zero, and non-finite results are rejected.
Expressions are evaluated locally as arithmetic, never as scripts.
