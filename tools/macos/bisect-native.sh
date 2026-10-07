#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Launches a game repeatedly with the isolated test profile, each time restoring one
# piece of older native-backend behaviour (CITROSIS_NATIVE_COMPAT), and reports which
# runs get past early boot. Each run is killed after $SECONDS_PER_RUN seconds.
#   tools/macos/bisect-native.sh [game.nsp] [config ...]   e.g. ... none none none
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
game=${GAME_PATH:-}
if (($#)) && [[ $1 == *.nsp || $1 == *.xci || $1 == *.nca ]]; then game=$1; shift; fi
[[ -n "$game" && -f "$game" ]] || { echo 'Supply an existing game path as an argument or GAME_PATH.' >&2; exit 2; }
per_run=${SECONDS_PER_RUN:-40}
d="$root/build-macos/native-debug"
out="$d/bisect-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
echo "== Building (incremental), logs in $out"
./build-citrosis-macos.sh --qt --jobs "${BUILD_JOBS:-1}" >"$out/build.log" 2>&1 || { tail -30 "$out/build.log"; exit 1; }

if (($#)); then
    configs=("$@")
else
    configs=("dynarmic" "all" "none" "el1" "regs" "unmap" "sctlr" "clrex" "tlb" "icache" "simd"
             "around" "uncache" "kicks" "clock")
fi
# Dynarmic comparison profile: same data, CPU backend switched to Dynarmic.
rm -rf "$d/config-dynarmic"
cp -R "$d/config" "$d/config-dynarmic"
sed -i '' 's/^cpu_backend=.*/cpu_backend=0/' "$d/config-dynarmic/citron/qt-config.ini"
run=0
for config in "${configs[@]}"; do
    run=$((run + 1))
    printf -v key "%02d-%s" "$run" "$config"
    env_args=(XDG_DATA_HOME="$d/data" XDG_CONFIG_HOME="$d/config" XDG_CACHE_HOME="$d/cache"
              CITROSIS_NATIVE_STATS=1)
    case "$config" in
        none) ;;
        dynarmic) env_args=(XDG_DATA_HOME="$d/data" XDG_CONFIG_HOME="$d/config-dynarmic"
                            XDG_CACHE_HOME="$d/cache") ;;
        clock) env_args+=(CITROSIS_STANDARD_CLOCK=1) ;;
        *) env_args+=(CITROSIS_NATIVE_COMPAT="$config") ;;
    esac
    env "${env_args[@]}" build-macos/bin/citron.app/Contents/MacOS/citron "$game" \
        >"$out/$key.stdout" 2>&1 &
    pid=$!
    sleep "$per_run"
    kill "$pid" 2>/dev/null; sleep 2; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    cp "$d/data/citron/log/citron_log.txt" "$out/$key.log" 2>/dev/null
    panics=$(grep -c "Userspace PANIC" "$out/$key.log")
    faults=$(grep -c "Native guest fault\|Unhandled native\|Native mapping did not" "$out/$key.log")
    running=$(grep -c "NotifyRunning" "$out/$key.log")
    if ((panics > 0)); then result=PANIC; elif ((faults > 0)); then result=FAULT; elif ((running > 0)); then result=BOOTED; else result=STUCK; fi
    stats=$(grep "Native exits/s" "$out/$key.log" | tail -1 | sed 's/.*Native exits/exits/')
    printf '%-8s %-6s %s\n' "$config" "$result" "$stats" | tee -a "$out/summary.txt"
done
echo "== Done: $out/summary.txt"
