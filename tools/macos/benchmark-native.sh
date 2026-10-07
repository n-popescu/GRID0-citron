#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Controlled, opt-in renderer experiments using the disposable native profile.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
d="$root/build-macos/native-debug"
variant=${1:-baseline}
seconds=${SECONDS_PER_RUN:-90}
sample_after=${SAMPLE_AFTER:-60}
[[ $seconds =~ ^[0-9]+$ && $sample_after =~ ^[0-9]+$ ]] || exit 1
((seconds > sample_after + 2)) || { echo 'Run duration must exceed sample time by at least three seconds.' >&2; exit 1; }
game=${GAME_PATH:?Set GAME_PATH to an existing game file}
[[ -f "$game" ]] || { echo 'GAME_PATH must point to an existing game file.' >&2; exit 2; }
out="$d/performance-$(date +%Y%m%d-%H%M%S)-$variant"
mkdir -p "$out"
cp -R "$d/config" "$out/config"
python3 - "$out/config/citron/qt-config.ini" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
text = path.read_text()
# Only the disposable profile: allow scheduled shutdown to save the cache.
for key, value in [("confirmStop", "2"), ("confirmStop\\default", "false")]:
    pattern = rf"(?m)^{re.escape(key)}=.*$"
    if re.search(pattern, text):
        text = re.sub(pattern, lambda _: f"{key}={value}", text)
    else:
        text = text.replace("[UI]\n", f"[UI]\n{key}={value}\n", 1)
path.write_text(text)
PY
args=(CITROSIS_BENCHMARK=1 CITROSIS_NATIVE_STATS=1
      CITROSIS_BENCHMARK_FRAME_CSV="$out/frame-times.csv"
      XDG_CONFIG_HOME="$out/config" XDG_DATA_HOME="$d/data" XDG_CACHE_HOME="$d/cache")
case "$variant" in
    baseline) ;;
    async) args+=(MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0) ;;
    arguments-off) args+=(MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0) ;;
    prefill) args+=(MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=1) ;;
    half-res|quarter-res|mailbox|present-thread|present-async|present-fast|present-fresh|present-fast-fresh|present-precise-fresh)
        case "$variant" in
            present-async) args+=(MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0) ;;
            present-fast|present-fast-fresh) args+=(MVK_CONFIG_FAST_MATH_ENABLED=1) ;;
            present-precise-fresh) args+=(CITROSIS_INLINE_PRECISE_MATH=1) ;;
            present-fresh) args+=(CITROSIS_INLINE_PRECISE_MATH=0) ;;
        esac
        python3 - "$out/config/citron/qt-config.ini" "$variant" <<'PY'
from pathlib import Path
import re
import sys
path = Path(sys.argv[1])
key, value = {"half-res": ("resolution_setup", 1),
              "quarter-res": ("resolution_setup", 0),
              "mailbox": ("use_vsync", 1),
              "present-thread": ("async_presentation", "true"),
              "present-async": ("async_presentation", "true"),
              "present-fast": ("async_presentation", "true"),
              "present-fresh": ("async_presentation", "true"),
              "present-fast-fresh": ("async_presentation", "true"),
              "present-precise-fresh": ("async_presentation", "true")}[sys.argv[2]]
text = re.sub(rf"(?m)^{key}=.*$", f"{key}={value}", path.read_text())
text = re.sub(rf"(?m)^{key}\\default=.*$", lambda _: f"{key}\\default=false", text)
if sys.argv[2].endswith("fresh"):
    text = re.sub(r"(?m)^use_vulkan_driver_pipeline_cache=.*$", "use_vulkan_driver_pipeline_cache=false", text)
    text = re.sub(r"(?m)^use_vulkan_driver_pipeline_cache\\default=.*$", lambda _: "use_vulkan_driver_pipeline_cache\\default=false", text)
path.write_text(text)
PY
        ;;
    *) echo "Unknown variant: $variant" >&2; exit 1 ;;
esac
codesign --force --sign - --entitlements "$root/dist/macos/Citrosis.entitlements" "$root/build-macos/bin/citron.app"
env "${args[@]}" "$root/build-macos/bin/citron.app/Contents/MacOS/citron" "$game" >"$out/stdout.txt" 2>&1 &
pid=$!
cleanup() { kill "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
echo "Benchmark PID $pid; evidence: $out"
sleep "$sample_after"
if kill -0 "$pid" 2>/dev/null; then
    sample "$pid" 2 -file "$out/sample.txt" >/dev/null 2>&1 || true
fi
sleep "$((seconds - sample_after - 2))"
kill "$pid" 2>/dev/null || true
# Allow normal Qt shutdown to save the driver cache before forcing termination.
for ((grace=0; grace<15; ++grace)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
done
if kill -0 "$pid" 2>/dev/null; then
    echo 'Shutdown exceeded 15 seconds; forcing termination.' >&2
    kill -9 "$pid" 2>/dev/null || true
fi
wait "$pid" 2>/dev/null || true
cp "$d/data/citron/log/citron_log.txt" "$out/game.log"
rg 'CitrosisBenchmark|Native guest fault|Userspace PANIC|NotifyRunning' "$out/game.log" > "$out/frames.txt" || true
echo "Finished: $out/frames.txt"
