#!/usr/bin/env python3
"""Converts assets/icon.png into src/icon_data.hpp's embedded byte array.

Run after regenerating assets/icon.png (see gen_icon.py) to pick up changes:

    python3 assets/png_to_header.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_PATH = os.path.join(HERE, "icon.png")
HEADER_PATH = os.path.join(HERE, "..", "src", "icon_data.hpp")

def main():
    data = open(PNG_PATH, "rb").read()
    lines = []
    for i in range(0, len(data), 20):
        chunk = data[i:i+20]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    body = "\n".join(lines)

    header = f"""#pragma once

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
