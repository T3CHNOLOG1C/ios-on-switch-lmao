#!/usr/bin/env python3
"""Copy a working Hekate L4T entry into a new, separate configuration file.

The resulting entry boots the same installed Linux host. Run IOSwitch from
that Linux installation; this tool does not make a native iOS or EFI payload.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


DEFAULT_NAME = "IOSwitch (Linux)"
_HEADER = re.compile(r"\[([^\[\]\r\n]+)\]")


def _check_name(name: str) -> None:
    if not name.strip() or any(char in name for char in "[]\r\n"):
        raise ValueError("entry names must be nonempty and cannot contain brackets or newlines")
    if name.strip().casefold() == "config":
        raise ValueError("[config] is global configuration, not a Linux boot entry")


def render_entry(source: str, section: str, name: str = DEFAULT_NAME) -> str:
    """Return one validated entry, retaining option text and duplicate options.

    Blank lines and # comment lines are omitted because L4T entries must have
    uninterrupted option lines. Semicolon-disabled options are retained.
    """
    _check_name(section)
    _check_name(name)
    sections: list[tuple[str, list[str]]] = []
    current: list[str] | None = None
    for number, line in enumerate(source.lstrip("\ufeff").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("["):
            header = _HEADER.fullmatch(stripped)
            if not header:
                raise ValueError(f"invalid section header on line {number}")
            current = []
            sections.append((header.group(1), current))
        elif stripped.startswith("{"):
            # Hekate caption entries delimit boot entries just as headers do.
            current = None
        elif current is not None:
            current.append(line)

    matches = [lines for title, lines in sections if title == section]
    if not matches:
        raise ValueError(f"section [{section}] was not found")
    if len(matches) != 1:
        raise ValueError(f"section [{section}] appears more than once; choose an unambiguous source")
    lines = matches[0]

    options: dict[str, list[str]] = {}
    for line in lines:
        if line.lstrip().startswith(";"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.strip():
            raise ValueError(f"unrecognized option in [{section}]: {line!r}")
        options.setdefault(key.strip(), []).append(value.strip())

    if any(key.casefold() == "payload" for key in options):
        raise ValueError("payload entries cannot be copied as an IOSwitch Linux entry")
    if options.get("l4t") != ["1"]:
        raise ValueError("the selected entry must contain exactly one enabled l4t=1 option")
    prefixes = options.get("boot_prefixes", [])
    if len(prefixes) != 1 or not prefixes[0]:
        raise ValueError("the selected L4T entry must have one nonempty boot_prefixes option")
    if not any(value for key in ("id", "rootdev") for value in options.get(key, [])):
        raise ValueError("the selected L4T entry must identify its existing Linux root with id or rootdev")

    return f"[{name}]\n" + "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="existing working Hekate L4T INI to read")
    parser.add_argument("--section", required=True, help="exact section name, without brackets")
    parser.add_argument("--output", required=True, type=Path, help="new local INI file; existing paths are never overwritten")
    parser.add_argument("--name", default=DEFAULT_NAME, help=f"new entry name (default: {DEFAULT_NAME})")
    args = parser.parse_args(argv)
    try:
        result = render_entry(args.source.read_text(encoding="utf-8-sig"), args.section, args.name)
        # Render and validate everything before creating anything. Exclusive
        # creation also rejects an existing symlink or --output == --source.
        with args.output.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(result)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"hekate_entry: {error}", file=sys.stderr)
        return 2
    print(f"Created {args.output}. This boots the existing Linux installation; run IOSwitch there.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
