Licensing
=========

hipe-pdf's own code is licensed under the GNU General Public License, version 3 or later.
It is built with the MuPDF library, which is licensed under the GNU Affero General Public
License, version 3 or later. So a hipe-pdf **binary** is a combined work, distributed under
the GNU AGPL version 3 or later as a whole.

| Part | Licence | Where it is stated |
|---|---|---|
| `src/`, `assets/`, `scripts/`, `Makefile` — hipe-pdf itself | GNU GPL version 3 or later | `COPYING`, and each file's header |
| `third_party/mupdf/` — MuPDF 1.28.3, an unmodified git submodule | GNU AGPL version 3 or later (Artifex also sells commercial licences) | `third_party/mupdf/COPYING` (the same text as `licenses/AGPL-3.0.txt` here) |
| libhipe, linked statically from Hipe | MIT | Hipe's `api/LICENSE.txt` and `hipe.h` |

Copyright (c) 2026 Daniel Kos, for hipe-pdf's own code.

Why a GPL program makes an AGPL binary
--------------------------------------

Section 13 of each licence allows GPL-3 code and AGPL-3 code to be combined into one work.
Each part keeps its own licence. hipe-pdf's source on its own remains under the GPL, and
can be copied out under it. The combined program has to meet the AGPL's terms, because
MuPDF's licence requires them for the whole work that includes MuPDF.

Distributing a hipe-pdf binary
------------------------------

- **Licence text.** Ship `licenses/AGPL-3.0.txt` (the licence of the binary as a whole) and
  `COPYING`.
- **Source.** Offer the Corresponding Source for the whole binary:
  - this repository at the commit the binary was built from, including the `Makefile`;
  - MuPDF and the libraries it bundles, at the commits the submodules pin;
  - the libhipe source the binary was linked against.

  `make source-bundle` packages the first two as `build/hipe-pdf-<version>-src.tar.gz`, from
  the current commit (it refuses to run with uncommitted changes). Publish it next to the
  binary, e.g. as another asset of the same GitHub release. Then the source stays available
  even if an upstream repository moves. libhipe is published with Hipe.
- **Network use (AGPL section 13).** Anyone who modifies hipe-pdf and lets people interact
  with the modified version remotely over a network must offer those people its source.
  A Hipe client's interface is shown by a display server over a socket, and that server can
  be on another machine. So someone using a modified hipe-pdf through a remote Hipe display
  is likely such a user.
- **Third-party notices.** Keep the notices of the libraries and fonts MuPDF builds in. They
  are listed below.

This is a summary for convenience. The licence texts themselves are what apply.

Libraries inside the binary
---------------------------

MuPDF builds these from `third_party/mupdf/thirdparty/` into `libmupdf-third.a`. Each
licence is in that library's own directory there.

| Library | Licence | Note for binary distribution |
|---|---|---|
| brotli | MIT | Keep its copyright and permission notice |
| cmark-gfm | BSD 2-Clause (and others, per file) | Keep its notice |
| extract | GNU AGPL version 3 or later | As MuPDF |
| FreeType | FreeType Licence, or GNU GPL version 2 or later | Distributed here under the GPL option (version 3 or later), which is compatible with the AGPL. The FreeType Licence would also require a credit in the documentation |
| gumbo-parser | Apache 2.0 | Keep its licence and any NOTICE. Compatible with (A)GPL version 3 |
| HarfBuzz | "Old MIT" (and others, per file) | Keep its notice |
| jbig2dec | GNU AGPL version 3 or later | As MuPDF |
| Little CMS (lcms2) | MIT | Keep its notice |
| libjpeg (Independent JPEG Group) | IJG licence | The documentation must say "this software is based in part on the work of the Independent JPEG Group" |
| MuJS | ISC | Keep its notice |
| OpenJPEG | BSD 2-Clause | Keep its notice |
| zlib | zlib licence | Keep its notice in source distributions |

Fonts inside the binary
-----------------------

MuPDF compiles fonts into the binary. It uses them in place of fonts a PDF names but doesn't
embed, such as the standard 14 (Times, Helvetica, Courier and so on) and non-embedded Chinese,
Japanese or Korean text. They are in `third_party/mupdf/resources/fonts/`, each family with
its licence.

| Fonts | Licence | Note for binary distribution |
|---|---|---|
| URW base 35 (Nimbus Roman, Sans, Mono, Standard Symbols, Dingbats) | SIL Open Font License 1.1 | Include the copyright notice and the OFL (`urw/OFL.txt`) |
| Noto | SIL Open Font License 1.1 | Include the copyright notice and the OFL (`noto/COPYING`) |
| Source Han Serif | SIL Open Font License 1.1 | Include the copyright notice and the OFL (`han/LICENSE.txt`) |
| Charis SIL | SIL Open Font License 1.1 | Include the copyright notice and the OFL (`sil/OFL.txt`) |
| Droid Sans Fallback | Apache 2.0 | Keep its notice (`droid/NOTICE`) |

The OFL allows the fonts to be bundled with software under any licence. Its conditions apply
to the fonts themselves (for example, they may not be sold on their own).

MuPDF's own build options also include other libraries, such as tesseract, leptonica,
curl, zxing-cpp and zint. They are not built into hipe-pdf (`make third-party` builds
only `libs`). Check this list again if the MuPDF build configuration changes.
