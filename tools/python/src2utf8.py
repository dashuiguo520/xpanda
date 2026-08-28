#!/usr/bin/env python3
"""Convert rAthena C/C++ source files to UTF-8 with BOM."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


EXTENSIONS = {".c", ".cpp", ".h", ".hpp"}
UTF8_BOM = b"\xef\xbb\xbf"


def convert_file(path: Path, legacy_encoding: str, dry_run: bool) -> str:
    data = path.read_bytes()
    if data.startswith(UTF8_BOM):
        return "unchanged"

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = data.decode(legacy_encoding)

    if not dry_run:
        temporary = path.with_name(path.name + ".src2utf8.tmp")
        temporary.write_bytes(UTF8_BOM + text.encode("utf-8"))
        os.replace(temporary, path)
    return "converted"


def main() -> int:
    default_source = Path(__file__).resolve().parents[2] / "src"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=default_source)
    parser.add_argument(
        "--legacy-encoding",
        default="gb18030",
        help="encoding used only when a file is not valid UTF-8 (default: gb18030)",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    root = args.directory.resolve()
    if not root.is_dir():
        parser.error(f"source directory does not exist: {root}")

    counts = {"converted": 0, "unchanged": 0, "failed": 0}
    for path in sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in EXTENSIONS):
        try:
            result = convert_file(path, args.legacy_encoding, args.dry_run)
            counts[result] += 1
            if result == "converted":
                print(f"{'would convert' if args.dry_run else 'converted'}: {path.relative_to(root)}")
        except (LookupError, OSError, UnicodeError) as error:
            counts["failed"] += 1
            print(f"failed: {path.relative_to(root)}: {error}")

    print(
        f"converted={counts['converted']} unchanged={counts['unchanged']} failed={counts['failed']}"
    )
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
