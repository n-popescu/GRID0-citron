#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Citrosis contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject incomplete deployments and references to build-machine libraries."""

import argparse
import plistlib
import subprocess
from pathlib import Path


def verify(app: Path, native: bool = False) -> None:
    with (app / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
    required = [
        executable,
        app / "Contents/Frameworks/libMoltenVK.dylib",
    ]
    if not native:
        required.append(app / "Contents/PlugIns/platforms/libqcocoa.dylib")
    for path in required:
        if not path.is_file():
            raise RuntimeError(f"Missing bundle component: {path}")

    # Detect Mach-O files by magic, including framework binaries without a
    # filename suffix. Resolve symlinks to avoid checking each version twice.
    magic = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
    binaries = set()
    for path in (app / "Contents").rglob("*"):
        if not path.is_file():
            continue
        with path.open("rb") as source:
            if source.read(4) in magic:
                binaries.add(path.resolve())

    for binary in sorted(binaries):
        ids = subprocess.check_output(["otool", "-D", str(binary)], text=True).splitlines()[1:]
        deps = subprocess.check_output(["otool", "-L", str(binary)], text=True).splitlines()[1:]
        for entry in deps:
            # Universal files repeat an unindented heading for each slice.
            if not entry.startswith(("\t", " ")):
                continue
            dependency = entry.strip().split(" (", 1)[0]
            if native and ("Qt" in dependency or "libqcocoa" in dependency):
                raise RuntimeError(f"Qt dependency in native frontend: {dependency}")
            # A dylib's own install ID is not a dependency on an external file.
            if dependency in ids:
                continue
            if dependency.startswith("/") and not dependency.startswith(("/usr/lib/", "/System/Library/")):
                raise RuntimeError(f"External dependency in {binary}: {dependency}")
            locations = {
                "@rpath/": app / "Contents/Frameworks",
                "@loader_path/": binary.parent,
                "@executable_path/": executable.parent,
            }
            for prefix, directory in locations.items():
                if dependency.startswith(prefix) and not (directory / dependency[len(prefix):]).is_file():
                    raise RuntimeError(f"Missing dependency in {binary}: {dependency}")
    print(f"Verified bundle components and {len(binaries)} Mach-O dependency lists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--native", action="store_true", help="Verify a Qt-free Swift frontend")
    args = parser.parse_args()
    verify(args.app, args.native)
