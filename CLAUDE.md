# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Scaffold + MVP single-page viewing + thumbnail sidebar (auto-scrolls to keep the current page's
thumbnail in view) + zoom (with Fit Width/Fit Page modes; also Ctrl+wheel and Ctrl +/-/0, not in
slideshow) + slideshow mode implemented (see
`.claude/plans` history / git log for the phased roadmap), plus keyboard navigation
(PageUp/PageDown/Home/End/arrows), scroll-wheel page-turning at scroll limits, and an invisible
selectable-text overlay atop the raster render (Acrobat/PDF.js style — see "Text rendering approach"
below). The README's original wishlist is done, apart from continuous scroll and SVG rendering, which were
decided against (see "Project intent" below).

Note: `HIPE_OP_GET_GEOMETRY` reports a scroll-independent position — it does not change as an ancestor
element is scrolled (confirmed empirically; see `scroll_thumbnail_into_view` in `main.cpp`). Any future
code computing on-screen/visibility geometry across a scrollable ancestor needs to account for this
directly (compare against that ancestor's own scroll position, not by adding it to reported coordinates).

## Text rendering approach

Pages are rendered as raster PNG (`PdfDocument::renderPagePng`, via MuPDF) — not DOM-level SVG. This was
a deliberate choice after evaluating DOM-SVG construction (building an `<svg>` subtree via Hipe opcodes,
which a separate hipecore fix was going to enable): raster is simpler (one render path, no dual
raster/DOM sync) and responsive enough to keep. To still get selectable/copyable text, an invisible text
layer (`#textLayer` in `main.cpp`, `PdfDocument::pageTextSpans` in `pdf_document.cpp`) is layered on top
via MuPDF's structured-text extraction (`fz_new_stext_page_from_page_number`) — one absolutely-positioned,
`color:transparent` `<span>` per line, real text content, positioned/sized from the line's bbox scaled to
match the raster. Confirmed working end-to-end (drag-select + copy verified via `xsel --primary` showing
real extracted text). Font-size is still estimated from line height (the overlay is drawn in the display
server's sans-serif, not the PDF's own fonts), but each line is then measured with `HIPE_OP_MEASURE_TEXT`
and scaled horizontally (`transform:scaleX`, as PDF.js does) to the exact width it has on the page — see
`update_text_layer`. Both ends of every line land within ~1.5px; inside a line the two fonts' letter
proportions can still differ slightly. `HIPE_PDF_DEBUG=1` logs the measured error per page (via
`HIPE_OP_GET_RANGE_GEOMETRY`).

**Find in page comes from the framing manager, not this app**: periscope's Find dialog sends
`HIPE_OP_FIND_TEXT` at the active client's frame, which searches `#textLayer` (the only selectable text —
`body` is `-webkit-user-select:none`, so the toolbar and labels are skipped). The app's only part is a
`#textLayer span::search-text` rule making matches a translucent highlight over the raster instead of the
theme's opaque one. It searches the current page only (the text layer holds one page), and not in
slideshow (no text layer there) or when running top-level without a framing manager.

## libhipe usage notes

- **Sends are buffered** (`hipe_set_buffered`). libhipe flushes whenever the app waits or checks for an
  instruction; anywhere else the screen must be current before the app goes quiet — before a blocking
  `renderPagePng`, or before a `poll()` of its own — needs an explicit `hipe_flush()`.
- **Locations are freed**: the text and link layers are rebuilt every render, so their elements'
  locations go back via `clear_and_free()` (`HIPE_OP_CLEAR` alone leaves them allocated on both sides).
- **Round trips go through `await_reply()`/`reply_float()`**, which survive the server disappearing
  mid-request (empty reply, `g_should_exit` set) instead of dereferencing a missing argument.
- **Waiting on our own fds** (slideshow idle fade, FIFO drain) uses `poll()` including
  `hipe_session_fd()`, after first draining `hipe_next_instruction()` non-blocking, per the libhipe docs.

## Build

```sh
git submodule update --init --recursive   # first time only: fetches vendored MuPDF (third_party/mupdf)
make third-party                          # builds third_party/mupdf's static libmupdf.a/libmupdf-third.a (slow, ~2 min, needs ~2GB RAM at -j4 — see below)
make                                      # builds build/hipe-pdf
```

Requires `~/hipe/api` already installed (`cd ~/hipe/api && make && sudo make install`) so `-lhipe` and
`<hipe.h>` resolve from `/usr/local/{lib,include}`.

**Memory note**: this machine is a resource-constrained Raspberry Pi shared with other running
sessions. Building MuPDF's harfbuzz/freetype sources at `-j4` can spike to several hundred MB per
`cc1plus` process; if `free -h` shows available memory getting tight, rebuild with `make -j2` (or `-j1`)
inside `third_party/mupdf` instead of `-j4`.

The resulting `build/hipe-pdf` binary is dynamically linked only against `libc`/`libm` (verify with
`ldd`) — MuPDF and libhipe are statically linked in, per the README's portability goal.

## Running / testing end-to-end

`./build/hipe-pdf <file.pdf>` connects to whatever Hipe session is already running (via the usual
env-var/keyfile auto-detection in `hipe_open_session`) — it does not start `hiped` itself.

**This machine already runs a live Hipe desktop for interactive use** — `hiped --fill` on a nested
Xephyr server (not the host's own `:0`/Wayland session), running a framing manager at top level
(periscope as of October 2026; `hipe-quadrant` earlier). The display number moves (`:43`, later `:54`),
and other sessions run their own hiped instances on other displays, so look it up rather than assume;
`:43` below stands for whichever it currently is. **That desktop is shared with other sessions' work**:
for anything driven by `xdotool` input, start a private stack instead (`Xephyr :<n> -screen 1024x768 -ac`
on the host display, then `hiped --fill --socket <path> --keyfile <path>` with `QT_QPA_PLATFORM=xcb` and
`WAYLAND_DISPLAY` unset, then `periscope --keyfile <path>` with `HIPE_SOCKET`/`HIPE_KEYFILE` pointing at
that hiped, then hipe-pdf with `HIPE_SOCKET` and `HIPE_KEYFILE=<periscope's keyfile>`). The Xephyr window
appears on the host desktop, so real mouse/keyboard input can reach it too. Implications for testing:
- Target that session explicitly: `DISPLAY=:43 ./build/hipe-pdf <file.pdf>` (check each hiped's
  `tr '\0' '\n' < /proc/<pid>/environ | grep DISPLAY` to find it).
  `scrot`/`grim` against the host's own `:0` will screenshot the *host's* desktop, not Hipe's — confirm
  the right target with `ps aux | grep hiped` and its `/proc/<pid>/environ` first.
  Screenshotting Hipe's own display: `DISPLAY=:43 scrot <path>.png`.
- Since Quadrant already holds the single-use top-level key, a plain `hipe_open_session(0,0,0,...)`
  call from another client still succeeds but opens a **second, separate top-level OS window** at the
  same `0,0` geometry — it does not error out. That second window can end up stacked behind Quadrant's
  (plain `XRaiseWindow`/`xdotool windowraise` did not change this in testing), so a from-behind occlusion
  is expected/benign during ad hoc testing on this shared desktop, not a bug in this app. For a
  reliable visual check that doesn't depend on window stacking, render a page directly through
  `PdfDocument::renderPagePng()` to a standalone PNG file (bypassing Hipe entirely) and inspect that.
- A sample PDF exists at `~/periscope/periscope-screenshot.pdf` for smoke-testing (a screenshot of this
  same Hipe/Quadrant desktop, taken by the separate `periscope` project).

## Hipe display server and API

The Hipe display server and its API live at `~/hipe` (a separate git repo, with its own CLAUDE.md).
This project's `api` dependency referenced below is that codebase — consult `~/hipe/api` and
`~/hipe/CLAUDE.md`/`README.md` for the API surface when implementing against it.

The **current, per-opcode API reference** (more up to date than `~/hipe/api/doc/api_reference.txt`)
is the hipe.tech site source at `~/public_html/hipe/files/content/3.documentation/4.Hipe API/` — one
`.txt` (HTML fragment) file per `HIPE_OP_*` instruction, plus overview files `03.libhipe functions.txt`,
`06.Hipe instructions.txt`, and `07.Interprocess communication.txt`. Also check
`~/public_html/hipe/files/content/5.status/1.Status.txt` for current backend status/limitations. Treat
`~/public_html/hipe/files/manual.html` in that same tree as unrelated — it's the FolderCMS manual for
the hipe.tech site itself, not Hipe API docs.

Facts pulled from that doc tree relevant to this PDF viewer, since they resolve the README's open
questions:

- **Canvas works under hipecore since Hipe 2.11** (2D context only, drawn in C++; a raw RGBA buffer can
  be written with `HIPE_OP_SET_SRC` and mime type `image/x-raw-rgba`). It was a no-op when this app's
  rendering path was chosen, and the hipe.tech status page may still say so. Considered and declined in
  October 2026: pages are still rendered client-side to PNG and pushed to an `<img>`, which is simpler
  and works on both backends.
- Getting rendered page bytes on screen means: `HIPE_OP_APPEND_TAG` an `<img>` element, then
  `HIPE_OP_SET_SRC` with `arg[0]` = raw image bytes and `arg[1]` = mime type (defaults to `image/png` if
  omitted; any other format, including `image/svg+xml`, must set the mime type explicitly). This settles
  the README's "SVG vs `<img>`" question in favor of `<img>` + `SET_SRC`, regardless of whether the
  bytes are rasterized (PNG) or vector (SVG) — the choice between those two is about the PDF-rendering
  library used, not about Hipe support.
  - `HIPE_OP_SET_STYLE_SRC` is the equivalent for CSS properties like `background-image` if a page
    background rather than an `<img>` is preferred for a given layer.
- Scrolling/viewport math for the planned continuous-scroll feature: `HIPE_OP_GET_SCROLL_GEOMETRY`
  (returns scroll position + total scrollable size via `HIPE_OP_GEOMETRY_RETURN`), `HIPE_OP_SCROLL_TO` /
  `HIPE_OP_SCROLL_BY` (absolute/relative, pixels or `%`), and `HIPE_OP_GET_GEOMETRY` for an element's
  on-screen box. Virtualized rendering (only render pages near the viewport) has to be driven by polling/
  reacting to these, since Hipe has no scroll-event notification of its own beyond these query ops.
- `HIPE_OP_EVENT_REQUEST` (arg0 = DOM event name without "on", e.g. `"click"`) is how slideshow
  click-to-advance / context-click would be wired up. Under hipecore every request adds its own
  listener (the same event is then reported once per request, even with the same requestor), so cancel
  with `HIPE_OP_EVENT_CANCEL` before re-requesting; Qt5WebKit keeps one request per (element, event)
  pair. Optional arg1 (hipecore) lists which matching events have their default action cancelled, e.g.
  wheel `*,4` for Ctrl+wheel; `contextmenu` is cancelled unless arg1 is `-`.
- `HIPE_OP_DIALOG` is how the slideshow's context-click popup (next/prev/start/end/leave) would be
  implemented, but **only works if this app runs as a child frame under a framing manager that
  implements dialogs** — a top-level client gets the dialog from Hipe itself, but per the framing-manager
  section in `~/hipe/CLAUDE.md`, whether *this* app runs top-level or framed depends on how it's launched.
- `HIPE_OP_TAKE_SNAPSHOT` gives a free "export current page as PDF or PNG" for whatever is on screen
  (streamed back via `HIPE_OP_FILE_RETURN`), which is a plausible reuse for a "save/print current page"
  feature but is unrelated to the core need of parsing/rendering an *input* PDF.
- **Open gap, not resolved by the Hipe docs**: Hipe has no PDF parsing/rendering of its own (it only
  moves opaque bytes into `<img>`/style `src`s). This app must bring its own PDF rendering — parsing the
  PDF and rasterizing/vectorizing each page to bytes happens entirely in this codebase (or a linked
  library), which is the main open architectural decision given the README's "avoid dynamic dependencies"
  constraint (rules out most system-installed PDF libs unless statically linked).

## Project intent (from README.md)

A minimal PDF viewer for the Hipe display server, written in C++. Key constraints:

- The Hipe API is the main dependency; avoid other dynamic dependencies to maximize portability.
- Prefer lean/efficient implementations over aesthetics where the two trade off (e.g. thumbnail rendering).

The README's original wishlist, and what became of it (the wishlist itself was removed from the README in
October 2026, once the README became the public GitHub page):
- Thumbnail sidebar, zoom, slideshow with a context-click menu: done.
- SVG rendering: dropped in favour of raster PNG plus an invisible text layer (see "Text rendering
  approach").
- Continuous scrolling with virtualized rendering (only pages near the viewport rendered): decided
  against -- it would complicate the single-page design (rendering, text/link layers, slideshow, wheel
  page-turning) for little gain.
