#!/usr/bin/env python3
"""Record the exact downstream UniXNU revision used by a package build."""

import argparse
import json
import subprocess
from pathlib import Path


def git(repo, *args):
    return subprocess.check_output(["git", "-C", str(repo), *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("upstream_base")
    parser.add_argument("edk2", type=Path)
    args = parser.parse_args()

    status = git(args.source, "status", "--short").decode(errors="replace").splitlines()
    if status:
        raise SystemExit("UniXNU checkout is dirty after packaging: " + "; ".join(status))
    downstream_revision = git(args.source, "rev-parse", "HEAD").decode().strip()
    merge_base = git(args.source, "merge-base", "HEAD", args.upstream_base).decode().strip()
    tree_digest = git(args.source, "rev-parse", "HEAD^{tree}").decode().strip()

    edk2_revision = None
    if (args.edk2 / ".git").exists():
        edk2_revision = git(args.edk2, "rev-parse", "HEAD").decode().strip()

    manifest = json.loads(args.manifest.read_text())
    manifest["source_inputs"] = {
        "unixnu": {
            "downstream_revision": downstream_revision,
            "upstream_base": args.upstream_base,
            "merge_base": merge_base,
            "source_tree_digest": tree_digest,
            "status": status,
        },
        "edk2_msm8998_revision": edk2_revision,
    }
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
