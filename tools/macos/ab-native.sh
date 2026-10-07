#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# A/B check: temporarily builds the LEGACY native backend (tools/macos/legacy-native),
# boots the game with it, then restores and rebuilds the current backend.
#   tools/macos/ab-native.sh
set -uo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
dir=src/core/arm/hypervisor
stash=$(mktemp -d "$root/build-macos/native-debug/current-backend-XXXXXX")
cp "$dir/hypervisor_vm.h" "$dir/hypervisor_vm.cpp" "$dir/arm_hypervisor.cpp" "$stash/"
restore() {
    cp "$stash/hypervisor_vm.h" "$stash/hypervisor_vm.cpp" "$stash/arm_hypervisor.cpp" "$dir/"
    echo "== Restored current backend sources; rebuilding"
    ./build-citrosis-macos.sh --qt --jobs "${BUILD_JOBS:-1}" >"$stash/rebuild.log" 2>&1 || echo "REBUILD FAILED: $stash/rebuild.log"
}
trap restore EXIT
cp tools/macos/legacy-native/hypervisor_vm.h tools/macos/legacy-native/hypervisor_vm.cpp \
   tools/macos/legacy-native/arm_hypervisor.cpp "$dir/"
# Two legacy runs with today's clock, two with the old wall clock.
SECONDS_PER_RUN=${SECONDS_PER_RUN:-40} tools/macos/bisect-native.sh none clock none clock
