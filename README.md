PDF viewer utility for Hipe display server.

This is a minimal PDF viewer built in C++. It should use the hipe API as its main dependency
and avoid dynamic dependencies for maximum portability.


## Building

Assumes Hipe is already installed -- i.e. `libhipe` and `<hipe.h>` are on the compiler's
default search path (normally installed to `/usr/local/{lib,include}` by `hipe/api`). You also
need a C++17 compiler and `make`. Nothing else: PDF rendering comes from a vendored copy of
MuPDF that bundles its own freetype/harfbuzz/zlib/jbig2dec/openjpeg, so there are no `-dev`
packages to install.

```sh
git submodule update --init --recursive   # fetch the vendored MuPDF (third_party/mupdf, pinned tag)
make third-party                          # build MuPDF's static libmupdf.a / libmupdf-third.a
make                                      # build build/hipe-pdf
```

`make third-party` is the slow step (~2 min) and can spike to several hundred MB per compiler
process. On a memory-constrained machine, build MuPDF with fewer jobs:

```sh
cd third_party/mupdf && make build=release libs -j2   # or -j1
```

All build output -- both `build/*.o`/`build/hipe-pdf` and the MuPDF objects/archives -- is
toolchain- and architecture-specific and does **not** carry between machines. If you copied the
tree from another box rather than cloning it, start by discarding all of it:

```sh
make distclean     # clean + clean-third-party (rm -rf build/*.o build/hipe-pdf third_party/mupdf/build)
make third-party
make
```

(`make clean-third-party` alone just wipes `third_party/mupdf/build`; `distclean` also clears
the top-level `build/`. Both leave the checked-out MuPDF source intact.) Symptoms of stale
cross-arch artifacts: `ld` reporting `skipping incompatible .../libmupdf.a` and then falling
back to a system `libmupdf` with `undefined reference to fz_*` / `FT_*`.

The resulting `build/hipe-pdf` statically links MuPDF and libhipe; only libc/libm/libstdc++
remain dynamic (verify with `ldd build/hipe-pdf`).


## Features

- **Page view** with a sidebar of page thumbnails (which can be hidden), and a busy cursor and
  "Loading..." placeholder while a slow page renders. A page too complex to render in time is
  shown at a lower resolution instead.
- **Navigation:** toolbar buttons, PageUp/PageDown/Home/End, the arrow keys, clicking a
  thumbnail, and the scroll wheel, which turns the page when you're already at its top or
  bottom.
- **Zoom:** toolbar buttons, Fit Width and Fit Page, Ctrl+wheel, and Ctrl +, Ctrl - and Ctrl 0.
  Clicking the zoom percentage resets it to 100%.
- **Selectable text:** page text can be selected and copied, and a framing manager's Find (such
  as periscope's) searches it.
- **Links:** links within the document jump to their page; links to web pages are opened
  through Hipe (`HIPE_OP_OPEN_LINK`).
- **Slideshow:** the page fills the frame, against a background matched to the slide. Click to
  advance. A right-click, or the Menu button that appears when the mouse moves, opens a menu
  (previous, next, start, end, leave) showing the slide number, the time, and how long the
  talk and the current slide have run.
- **Opening documents:** from the command line, with the Open button, or sent from another
  app ("open with...").


## Running

```sh
./build/hipe-pdf [file.pdf]
```

Given a file argument, it opens that document ("embedded" mode -- no Open button). Given no
argument, it starts empty and shows an Open button that pulls a document in through Hipe's FIFO
mechanism: a native file dialog when running top-level, or a framing-manager-mediated file
shell when running framed.


## Licence

hipe-pdf's own code is licensed under the GNU GPL, version 3 or later (`COPYING`). It is
built with MuPDF, which is licensed under the GNU AGPL, version 3 or later, so a hipe-pdf
binary is distributed under the AGPL version 3 or later as a whole
(`licenses/AGPL-3.0.txt`). See `LICENSE.md` for the details, for the libraries and fonts
inside the binary, and for `make source-bundle`, which packages the source to publish with a
binary.
