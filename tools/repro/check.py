#!/usr/bin/env python3
"""Reproducibility check: build the targets twice from scratch (two fresh output bases, --config=reproducible-check:
no disk or remote cache) and compare every output file bit for bit.

    bazel run //tools/repro:check                      # //:kernel //:boot //:gpu
    bazel run //tools/repro:check -- //:kernel         # just these
    bazel run //tools/repro:check -- --keep            # keep the two output bases (under $TMPDIR) afterwards

Each build compiles everything (XNU, every driver, both Rust bridges): expect 10+ minutes and ~2 GB per output base
while it runs. Prints the "INFO: Elapsed / processes" lines of both builds and the comparison."""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def outputs(bazel, base, workspace, targets):
    files = {}
    query = subprocess.run([bazel, "--output_base=" + base, "cquery", "--config=reproducible-check",
                            "--output=files", " + ".join(targets)], cwd=workspace, capture_output=True, text=True, check=True)
    exec_root = subprocess.run([bazel, "--output_base=" + base, "info", "execution_root"], cwd=workspace,
                               capture_output=True, text=True, check=True).stdout.strip()
    for line in query.stdout.split():
        path = Path(exec_root, line)
        items = [p for p in path.rglob("*") if p.is_file()] if path.is_dir() else [path]
        for p in items:
            files[str(p.relative_to(exec_root))] = hashlib.sha256(p.read_bytes()).hexdigest()
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("targets", nargs="*", default=["//:kernel", "//:boot", "//:gpu"])
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY") or sys.exit("run with bazel run")
    bazel = os.environ.get("BAZEL", "bazel")
    scratch = Path(tempfile.mkdtemp(prefix="ioswitch-repro-"))
    results = []
    try:
        for n in (1, 2):
            base = str(scratch / "base{}".format(n))
            build = subprocess.run([bazel, "--output_base=" + base, "build", "--config=reproducible-check",
                                    "--experimental_convenience_symlinks=ignore", *args.targets],
                                   cwd=workspace, capture_output=True, text=True)
            for line in build.stderr.splitlines():
                if line.startswith("INFO: Elapsed") or " processes" in line:
                    print("build {}: {}".format(n, line))
            if build.returncode:
                sys.exit(build.stderr[-4000:])
            results.append(outputs(bazel, base, workspace, args.targets))
        a, b = results
        differ = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
        print("{} output files, {} differ".format(len(a), len(differ)))
        for k in differ[:50]:
            print("  differs:", k)
        sys.exit(1 if differ else 0)
    finally:
        if not args.keep:
            for n in (1, 2):
                subprocess.run([bazel, "--output_base=" + str(scratch / "base{}".format(n)), "clean", "--expunge"],
                               cwd=workspace, capture_output=True)
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
