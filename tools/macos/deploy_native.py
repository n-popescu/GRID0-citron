#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bundle transitive Homebrew dependencies without deploying Qt."""
import argparse
import os
import plistlib
import shutil
import subprocess
from pathlib import Path
from verify_bundle import verify

def run(*args):
    return subprocess.check_output(args, text=True)

def dependencies(path):
    ids = run('otool', '-D', str(path)).splitlines()[1:]
    return [line.strip().split(' (', 1)[0] for line in run('otool', '-L', str(path)).splitlines()[1:]
            if line.startswith((' ', '\t')) and line.strip().split(' (', 1)[0] not in ids]

def deploy(app, entitlements):
    frameworks = app / 'Contents/Frameworks'
    frameworks.mkdir(exist_ok=True)
    with (app / 'Contents/Info.plist').open('rb') as source:
        executable = app / 'Contents/MacOS' / plistlib.load(source)['CFBundleExecutable']
    queue = [(executable, executable)]
    queue += [(p, p) for p in frameworks.glob('*.dylib')]
    origins = {}
    visited = set()
    while queue:
        original, binary = queue.pop(0)
        if binary in visited:
            continue
        visited.add(binary)
        os.chmod(binary, 0o755)
        for dependency in dependencies(binary):
            if dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if 'Qt' in dependency or 'libqcocoa' in dependency:
                raise RuntimeError(f'Native app unexpectedly links Qt: {dependency}')
            if dependency.startswith('@loader_path/'):
                source = original.parent / dependency[len('@loader_path/'):]
            elif dependency.startswith('/'):
                source = Path(dependency)
            elif dependency.startswith(('@rpath/', '@executable_path/')):
                # These are already bundle-relative; the verifier checks them.
                continue
            else:
                raise RuntimeError(f'Unsupported dependency: {dependency}')
            source = source.resolve(strict=True)
            name = source.name
            destination = frameworks / name
            if name in origins and origins[name] != source:
                raise RuntimeError(f'Library name collision: {name}')
            if not destination.exists():
                origins[name] = source
                shutil.copy2(source, destination)
                run('install_name_tool', '-id', '@executable_path/../Frameworks/' + name, str(destination))
                queue.append((source, destination))
            run('install_name_tool', '-change', dependency, '@executable_path/../Frameworks/' + name, str(binary))
    verify(app, native=True)
    for binary in sorted(visited):
        if binary != executable:
            run('codesign', '--force', '--sign', '-', str(binary))
    run('codesign', '--force', '--sign', '-', '--entitlements', str(entitlements), str(app))
    run('codesign', '--verify', '--deep', '--strict', str(app))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('app', type=Path)
    parser.add_argument('--entitlements', type=Path, required=True)
    args = parser.parse_args()
    deploy(args.app.resolve(), args.entitlements.resolve())
