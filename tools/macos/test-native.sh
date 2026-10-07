#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Rebuilds Citrosis, runs the native-backend regression, then launches a game with the
# isolated test profile (build-macos/native-debug), exit statistics and MoltenVK warnings.
# Everything is written to build-macos/native-debug/run-<time>/.
#   tools/macos/test-native.sh [game.nsp] [--real-profile]
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
game=${GAME_PATH:-}
real_profile=0
for arg in "$@"; do
    case "$arg" in
        --real-profile) real_profile=1 ;;
        *) game=$arg ;;
    esac
done
[[ -n "$game" && -f "$game" ]] || { echo 'Supply an existing game path as an argument or GAME_PATH.' >&2; exit 2; }
out="build-macos/native-debug/run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "== Logs: $out"

echo "== Building (incremental)"
if ! ./build-citrosis-macos.sh --qt --jobs "${BUILD_JOBS:-1}" >"$out/build.log" 2>&1; then
    tail -40 "$out/build.log"; echo "BUILD FAILED (full log: $out/build.log)"; exit 1
fi
cmake --build build-macos --target citrosis-native-smoke citrosis-cpu-smoke --parallel "${BUILD_JOBS:-1}" >>"$out/build.log" 2>&1 || {
    tail -40 "$out/build.log"; echo "SMOKE BUILD FAILED"; exit 1; }

echo "== Native regression"
./build-macos/bin/citrosis-native-smoke 2>&1 | tee "$out/native-smoke.txt"
smoke=${PIPESTATUS[0]}
./build-macos/bin/citrosis-cpu-smoke 2>&1 | tee "$out/cpu-smoke.txt"
if ((smoke != 0)); then echo "NATIVE SMOKE FAILED - not launching the game"; exit 1; fi

echo "== Launching game (quit Citrosis when done; play ~2 minutes in a 3D scene)"
env_args=(CITROSIS_NATIVE_STATS=1 MVK_CONFIG_LOG_LEVEL=2)
if ((real_profile == 0)); then
    d="$root/build-macos/native-debug"
    env_args+=(XDG_DATA_HOME="$d/data" XDG_CONFIG_HOME="$d/config" XDG_CACHE_HOME="$d/cache")
    log_dir="$d/data/citron/log"
else
    log_dir="$HOME/.local/share/citron/log"
fi
env "${env_args[@]}" build-macos/bin/citron.app/Contents/MacOS/citron "$game" >"$out/stdout.log" 2>&1
echo "== Exit status: $?"
cp "$log_dir/citron_log.txt" "$out/" 2>/dev/null
grep -h "Native exits/s\|Critical\|DEVICE_LOST\|Command buffer\|mvk-error" "$out/citron_log.txt" "$out/stdout.log" 2>/dev/null | tail -30
echo "== Done. Results in $out"
