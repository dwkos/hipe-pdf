#!/usr/bin/env python3
# Copyright (c) 2026 Daniel Kos
#
# This file is part of hipe-pdf, licensed under the GNU General Public License
# version 3 or later. See COPYING and LICENSE.md.

"""Converts assets/icon.png into src/icon_data.hpp's embedded byte array.

Run after regenerating assets/icon.png (see gen_icon.py) to pick up changes:

    python3 assets/png_to_header.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_PATH = os.path.join(HERE, "icon.png")
HEADER_PATH = os.path.join(HERE, "..", "src", "icon_data.hpp")

LICENCE_NOTICE = """/*  Copyright (c) 2026 Daniel Kos

    This file is part of hipe-pdf.

    hipe-pdf is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    hipe-pdf is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with hipe-pdf.  If not, see <https://www.gnu.org/licenses/>.

    A hipe-pdf binary links the MuPDF library and is distributed under the
    GNU Affero General Public License version 3 or later as a whole; see
    LICENSE.md.
*/

"""


def main():
    data = open(PNG_PATH, "rb").read()
    lines = []
    for i in range(0, len(data), 20):
        chunk = data[i:i+20]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines)

    header = LICENCE_NOTICE + f"""#pragma once

#include <cstddef>

/* App icon sent via HIPE_OP_SET_ICON at startup (see main.cpp). Generated from
 * assets/icon.png -- to change the icon, edit assets/gen_icon.py, regenerate
 * assets/icon.png, then re-run assets/png_to_header.py to refresh this file
 * rather than hand-editing the array below. */

static const unsigned char kAppIconPng[] = {{
{body}
}};

static const std::size_t kAppIconPngLen = sizeof(kAppIconPng);
"""
    with open(HEADER_PATH, "w") as f:
        f.write(header)
    print(f"wrote {HEADER_PATH} ({len(data)} bytes embedded)")

if __name__ == "__main__":
    main()
