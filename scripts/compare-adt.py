#!/usr/bin/env python3
"""Compare two Apple-format device trees by node path and property bytes."""

import argparse
import hashlib
from pathlib import Path
import struct
import sys


def parse_node(data, offset):
    props, children = struct.unpack_from("<II", data, offset)
    offset += 8
    node = {"props": {}, "children": []}
    for _ in range(props):
        key = data[offset:offset + 32].split(b"\0")[0].decode("ascii", "replace")
        offset += 32
        length, = struct.unpack_from("<I", data, offset)
        offset += 4
        node["props"][key] = data[offset:offset + length]
        offset += (length + 3) & ~3
    for _ in range(children):
        child, offset = parse_node(data, offset)
        node["children"].append(child)
    name = node["props"].get("name", b"")
    node["name"] = name.split(b"\0")[0].decode("ascii", "replace")
    return node, offset


def flatten(path):
    data = path.read_bytes()
    root, end = parse_node(data, 0)
    if end != len(data):
        raise ValueError(f"{path}: parsed {end} of {len(data)} bytes")
    result = {}

    def walk(node, prefix):
        current = f"{prefix}/{node['name']}"
        result[current] = node
        for child in node["children"]:
            walk(child, current)

    walk(root, "")
    return result


def value_text(value):
    if value is None:
        return "missing"
    if not value:
        return "(flag)"
    if len(value) <= 16:
        return f"`{value.hex()}`"
    if all(32 <= byte < 127 or byte == 0 for byte in value):
        text = value.rstrip(b"\0").decode("ascii").replace("\0", "\\0").replace("|", "\\|")
        return f"`{text}`"
    digest = hashlib.sha256(value).hexdigest()[:12]
    return f"bytes={len(value)}, sha256={digest}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--old", type=Path, required=True)
    parser.add_argument("--new", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    old = flatten(args.old)
    new = flatten(args.new)
    old_only = sorted(set(old) - set(new))
    new_only = sorted(set(new) - set(old))
    changes = []
    for path in sorted(set(old) & set(new)):
        old_props = old[path]["props"]
        new_props = new[path]["props"]
        for key in sorted(set(old_props) | set(new_props)):
            before = old_props.get(key)
            after = new_props.get(key)
            if before != after:
                changes.append((path, key, before, after))

    lines = [
        "# ADT comparison",
        "",
        f"- Old: `{args.old.name}` ({args.old.stat().st_size} bytes, "
        f"sha256 `{hashlib.sha256(args.old.read_bytes()).hexdigest()}`)",
        f"- New: `{args.new.name}` ({args.new.stat().st_size} bytes, "
        f"sha256 `{hashlib.sha256(args.new.read_bytes()).hexdigest()}`)",
        f"- Nodes: old={len(old)}, new={len(new)}, old-only={len(old_only)}, "
        f"new-only={len(new_only)}, changed-properties={len(changes)}",
        "",
        "## Nodes only in old",
        "",
    ]
    lines += [f"- `{path}`" for path in old_only] or ["- None"]
    lines += ["", "## Nodes only in new", ""]
    lines += [f"- `{path}`" for path in new_only] or ["- None"]
    lines += ["", "## Property differences", ""]
    if changes:
        lines += ["| Node | Property | Old | New |", "| --- | --- | --- | --- |"]
        lines += [
            f"| `{path}` | `{key}` | {value_text(before)} | {value_text(after)} |"
            for path, key, before, after in changes
        ]
    else:
        lines.append("None.")
    text = "\n".join(lines) + "\n"

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
        print(args.output)
    else:
        print(text, end="")
    if old_only:
        print(f"old-only nodes: {', '.join(old_only)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
