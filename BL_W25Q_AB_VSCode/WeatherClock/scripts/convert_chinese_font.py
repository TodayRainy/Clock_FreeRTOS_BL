#!/usr/bin/env python3
"""Convert the legacy GB2312 font text into a C ``font_chinese_t`` array."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


CHARACTER_RE = re.compile(
    r'((?:0x[0-9A-Fa-f]{2},?\s*)+)/\*"([^"]+)",\d+\*/'
)
HEX_BYTE_RE = re.compile(r"0x[0-9A-Fa-f]{2}")


def parse_font_source(path: Path) -> list[tuple[str, list[str]]]:
    """Read character names and hexadecimal glyph bytes from the source file."""
    content = path.read_text(encoding="gb2312")
    characters = []

    for byte_text, character_name in CHARACTER_RE.findall(content):
        glyph = HEX_BYTE_RE.findall(byte_text)
        if glyph:
            characters.append((character_name.strip(), glyph))
    return characters


def format_glyph(glyph: list[str], bytes_per_line: int = 16) -> list[str]:
    """Format one glyph as indented C initializer rows."""
    return [
        "            " + ",".join(glyph[offset:offset + bytes_per_line]) + ","
        for offset in range(0, len(glyph), bytes_per_line)
    ]


def write_font_array(path: Path,
                     characters: list[tuple[str, list[str]]]) -> None:
    """Write the parsed glyphs and a zero-filled end marker."""
    lines = ["static const font_chinese_t chinese_model[] =", "{"]
    for character_name, glyph in characters:
        lines.extend([
            "    {",
            f'        .name = "{character_name}",',
            "        .model = (const uint8_t[]) {",
            *format_glyph(glyph),
            "        }",
            "    },",
        ])
    lines.extend(["    {}", "};", ""])

    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", nargs="?", type=Path,
                        default=Path("chinese.txt"))
    parser.add_argument("output", nargs="?", type=Path,
                        default=Path("chinese_model.txt"))
    args = parser.parse_args()

    if not args.source.is_file():
        parser.error(f"input file not found: {args.source}")

    characters = parse_font_source(args.source)
    write_font_array(args.output, characters)
    print(f"Generated {len(characters)} characters in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
