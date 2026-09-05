# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Scaffold + MVP single-page viewing + thumbnail sidebar + zoom (with Fit Width/Fit Page modes) implemented
(see `.claude/plans` history / git log for the phased roadmap). Not yet implemented: slideshow, SVG
rendering, continuous scroll.

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

**This machine already runs a live Hipe desktop for interactive use** — `hiped --fill` on
`DISPLAY=:43` (a nested Xephyr server, not the host's own `:0`/Wayland session), currently running the
`hipe-quadrant` sample framing manager. Implications for testing:
- Target that session explicitly: `DISPLAY=:43 ./build/hipe-pdf <file.pdf>` (check
  `tr '\0' '\n' < /proc/$(pgrep hiped)/environ | grep DISPLAY` if it's moved).
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

- **Canvas is a no-op under hipecore** (`HIPE_OP_USE_CANVAS`/`CANVAS_ACTION`/`CANVAS_SET_PROPERTY` are
  accepted but do nothing when Hipe is linked against hipecore, which is the recommended/active backend
  per Hipe's status page). Since Qt5WebKit can't be assumed on the target, **canvas rendering is not a
  viable path** — pages must be rendered client-side (by this app, not by Hipe) to image bytes and pushed
  to the DOM.
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
  click-to-advance / context-click would be wired up; only one active request per (element, event) pair
  at a time.
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

Planned features (not yet implemented):
- Sidebar with page thumbnails, rendered as cheaply as possible.
- Zoom on the current page.
- Slideshow mode: current page fills the frame; click advances to the next page; context-click opens a
  dialog with next/prev/start/end/leave-slideshow options.
- SVG rendering for pages — note Hipe's DOM SVG support is unconfirmed as of this writing, so this may
  need to fall back to `<img>` tags with SVG byte data depending on what Hipe supports when implemented.
- Continuous scrolling that visually follows page-to-page but only renders pages within the current
  scroll viewport (i.e. virtualized rendering, not full-document rendering).
