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


## Running

```sh
./build/hipe-pdf [file.pdf]
```

Given a file argument, it opens that document ("embedded" mode -- no Open button). Given no
argument, it starts empty and shows an Open button that pulls a document in through Hipe's FIFO
mechanism: a native file dialog when running top-level, or a framing-manager-mediated file
shell when running framed.


Wishlist for features:
- A sidebar to allow page thumbnails to be navigated. Page thumbnails should be rendered as lean as possible; efficiency over aesthetics to a certain point.

- zooming of current page

- A slideshow button to allow current page to occupy full frame, click to advance to next page, context-click to pop up dialog allowing next page, prev page, start, end, leave slideshow.

- Use SVG rendering. Hipe may or may not yet support DOM SVGs vs existing support for <img>s with svg byte data at time of implementation.

- eventual continuous scrolling allowing one page to appear to follow the next linearly, but not actually rendering pages outside the current scroll view.



