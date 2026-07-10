#!/usr/bin/env python3
"""Count nonzero pixels in binary P6 PPM captures.

This is dependency-free so agents can use it in ordinary build/test shells.
It reports the gate used by DC2 title captures: dimensions, pixel count,
nonzero pixel count, and maximum byte value.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def _read_token(f) -> bytes:
    token = bytearray()

    while True:
        ch = f.read(1)
        if not ch:
            raise ValueError("unexpected EOF while reading PPM header")
        if ch == b"#":
            f.readline()
            continue
        if ch.isspace():
            continue
        token.extend(ch)
        break

    while True:
        ch = f.read(1)
        if not ch or ch.isspace():
            break
        if ch == b"#":
            f.readline()
            break
        token.extend(ch)

    return bytes(token)


def read_ppm(path: Path) -> tuple[int, int, int, bytes]:
    with path.open("rb") as f:
        magic = _read_token(f)
        if magic != b"P6":
            raise ValueError(f"{path}: unsupported PPM magic {magic!r}; expected P6")

        width = int(_read_token(f))
        height = int(_read_token(f))
        maxval = int(_read_token(f))
        if width <= 0 or height <= 0:
            raise ValueError(f"{path}: invalid dimensions {width}x{height}")
        if maxval <= 0 or maxval > 255:
            raise ValueError(f"{path}: unsupported maxval {maxval}; expected 1..255")

        expected = width * height * 3
        data = f.read(expected)
        if len(data) != expected:
            raise ValueError(f"{path}: expected {expected} payload bytes, got {len(data)}")

        return width, height, maxval, data


def count_nonzero_pixels(data: bytes) -> int:
    return sum(
        1
        for i in range(0, len(data), 3)
        if data[i] != 0 or data[i + 1] != 0 or data[i + 2] != 0
    )


def describe(path: Path) -> str:
    width, height, maxval, data = read_ppm(path)
    pixels = width * height
    nonzero = count_nonzero_pixels(data)
    max_byte = max(data) if data else 0
    return (
        f"{path} width={width} height={height} pixels={pixels} "
        f"nonzero={nonzero} maxval={maxval} maxbyte={max_byte}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ppm", nargs="+", type=Path, help="P6 PPM file(s) to inspect")
    args = parser.parse_args(argv)

    ok = True
    for ppm in args.ppm:
        try:
            print(describe(ppm))
        except Exception as exc:
            ok = False
            print(f"ERROR: {exc}", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
