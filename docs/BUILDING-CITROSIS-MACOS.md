# Building Citrosis on macOS

Citrosis now defaults to a standalone SwiftUI/AppKit macOS frontend. The app
links directly to the emulator core and uses a CAMetalLayer for Metal or Vulkan
through MoltenVK, with the existing CPU backends and GRID0+ services. Qt is not
loaded by this frontend. The legacy Qt frontend remains available with `--qt`
for features that have not yet migrated.

## Requirements

- An Apple Silicon Mac, Xcode 26 or later (including the macOS SDK and Swift compiler), and Homebrew.
- CMake 3.22 or newer, Ninja, and Python 3.
- Homebrew FFmpeg, glslang, autoconf, automake, and GNU libtool.
- Internet access on the first configure, for the pinned CPM sources and MoltenVK.

The script sets a macOS 14 deployment target for project sources. Homebrew
and autotools dependencies can require a newer macOS release; the deployment target alone
does not make a package compatible with older Macs. Intel and universal builds
need separate validation; do not combine arm64 and x86_64 in a single build tree.

## Build

From the repository root:

```sh
./build-citrosis-macos.sh --setup
./build-citrosis-macos.sh
open build-macos/bin/citrosis-mac.app
```

The native build uses the architecture of the active compiler. Use a native
Apple Silicon terminal and Homebrew rather than running these commands in
Rosetta. Submodule initialization is unnecessary for the CPM build. Downloads
and generated files stay under `build-macos`; deleting that directory also
deletes the dependency cache. Configuration does not install Git hooks unless
`CITRON_INSTALL_GIT_HOOKS=ON` is explicitly requested.

One compiler job is the default to keep memory use reasonable. Override with
`--jobs N`. Keep this build independent from Android, iOS, Windows, and Linux
build directories.

```sh
./build-citrosis-macos.sh --configure-only
./build-citrosis-macos.sh --jobs 3 -- -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

Extra arguments after `--` are passed to CMake and override script defaults.
LTO and profiling default off. Web services and OpenSSL default on to preserve
the online integration. The legacy `--qt` option supplies the Qt SVG CMake path.

## Native macOS frontend

The Swift app has list/grid library views, artwork, search, native open/folder
panels, sidebar settings, SF Symbols, Liquid Glass buttons on macOS 26+, and
short animations that respect Reduce Motion. The view switcher and toolbar use
native glass icon bubbles. During emulation, controls float over the game with
a renderer label instead of occupying a separate header. AppKit owns the menu bar and event
loop; Settings opens a persistent native window. GRID0+ friends and account/server
settings have native entry points, profile pictures, an add-friend field, and
incoming/sent request actions. Network interfaces use a native picker populated
from the available adapters. macOS 14–25 use standard bordered controls.

On first launch, `qt-config.ini` is copied to `citrosis-native.ini`. The original
profile is preserved. Game folders and controller mappings are imported; existing
per-game overrides are loaded at game launch. Native library folders and view mode
are stored in UserDefaults. Keys, firmware, saves, installed content, and shader
caches use the existing Citron data directories.

Input settings support eight players, keyboard/controller binding capture, sticks,
motion, default mappings, and vibration. The game overlay shows live FPS and fades
after three seconds of pointer inactivity. Mouse binding forwarding is not yet supported.

Native library context menus include per-title save/mod/shader folders, properties,
RomFS/ExeFS extraction, integrity checks and installed update/DLC removal. File >
Install Files to NAND accepts NSP/NCA updates and DLC. Content work has progress and
cancellation, while extraction refuses existing destinations. Input settings use a
controller diagram with grouped mappings, calibration and reusable INI profiles.
The latest content/profile workflows require user runtime validation.

Native frontend parity is still in progress: profile editing, interactive applet UI,
multiplayer room management and remaining property-page tools have not yet migrated. The core keeps their services, but this is
not yet a complete replacement for every Qt workflow. Do not equate a successful
native build or library smoke check with full gameplay/online validation.

Build the legacy frontend explicitly with `./build-citrosis-macos.sh --qt`.
That option requires Homebrew `qtbase` and `qtsvg`. Keep a separate build directory
if switching frontends frequently.

## Deploy a local app

```sh
./build-citrosis-macos.sh --package
open build-macos/dist/Citrosis.app
```

Packaging stages a separate bundle, copies transitive dynamic dependencies without
Qt using `tools/macos/deploy_native.py`, checks the Mach-O dependency lists for external build
paths, ad-hoc signs the app with the JIT and Apple Hypervisor entitlements, verifies the signature,
and creates `build-macos/dist/Citrosis-arm64.zip`. Deployment or
signature failures stop the script. A successful package replaces the previous
generated `Citrosis.app`. The development bundle remains in `bin/citrosis-mac.app`.
Bundle display name and identifier are Citrosis and `org.citrosis.emulator`;
the native executable and CMake target are `citrosis-mac`.

This is local ad-hoc signing, not Developer ID signing or notarization. A future
distribution pipeline must validate hardened runtime signing separately.
Set `-DCITROSIS_MACOS_VERSION=major.minor.patch` when preparing a release.

MoltenVK uses the existing pinned `v1.4.0-ryujinx` build. To supply another build:

```sh
./build-citrosis-macos.sh -- \
  -DUSE_SYSTEM_MOLTENVK=ON \
  -DMOLTENVK_LIBRARY=/absolute/path/to/libMoltenVK.dylib
```

The GUI embeds that dylib in `Contents/Frameworks`. The command-line frontend
can use `LIBVULKAN_PATH=/absolute/path/to/libMoltenVK.dylib`. Changing the Vulkan
implementation needs separate graphics testing; a successful compile does not
establish game compatibility.

## Native ARM64 execution on Apple Silicon

Apple Silicon builds include an experimental hardware execution backend using
Apple's Hypervisor framework. Select **Apple native ARM64 (experimental)** in
**configuration window (⌘,) → CPU → Backend**, then restart the game. Dynarmic remains
the default while compatibility is being validated. This backend runs 64-bit
guest instructions directly on ARM64 vCPUs, with existing Horizon service
emulation and the existing Vulkan/MoltenVK renderer. GRID0(+) is unchanged.

The implementation uses 4 KB guest translation tables inside a VM, independently
of macOS's 16 KB host pages. It keeps guest TLS and registers separate from host
TLS, intercepts supervisor calls and Switch timing registers, and invalidates
native mappings when guest permissions or GPU cache tracking change. GPU-backed
memory is downloaded before CPU cache invalidation and access is retried.

Use the build script to sign both the development and deployed app with
`com.apple.security.hypervisor`; an unsigned binary cannot create the VM. Native
execution requires a 64-bit application, multicore emulation, and an inactive
GDB debugger. Unsupported configurations or VM creation failures select
Dynarmic. Disable compilation with `-DCITROSIS_ENABLE_HYPERVISOR=OFF` if needed.

The standalone regression runs actual guest ARM64 without game files:

```sh
cmake --build build-macos --target citrosis-native-smoke
./build-macos/bin/citrosis-native-smoke
```

It checks SVC dispatch, TLS/SIMD/x18 preservation, 4 KB remapping, rejection of
stale mappings after invalidation, WFE continuation, two-core exclusive accesses,
and scheduler interruption during supervisor calls.
Native execution removes dynamic instruction translation; it does not guarantee
a particular frame-rate gain, since graphics, shader compilation, service calls,
and VM exits can still dominate. Full gameplay and performance comparisons need
separate validation.

On the Apple M2 test machine, Splatoon 3 11.3.0 passed the launch overlay,
animated its opening splash at 60 FPS, and continued through the 3D intro to character selection at an
observed 23–26 FPS after the WFE trap handler was added. Shader compilation caused
large temporary dips. These are observations from different scenes, not an
A/B benchmark or a full gameplay compatibility result. Existing MoltenVK
limitations and unsupported texture-format warnings remain.

## Codebase map and remaining macOS work

Local validation on October 4, 2026 used an Apple M2, macOS 27, AppleClang 21,
CMake 4.4.3, Qt 6.11.2, and FFmpeg 9.0.2. The GUI, command-line frontend, room
server, and shader tool built successfully. The Cocoa GUI launched with GRID0+
visible, the command-line help/version checks passed, and MoltenVK detected
the Apple M2. A synthetic ARM64 program executed loads/stores through both
Dynarmic callbacks and a page table. The deployed bundle passed ad-hoc signature
verification, the frontend's Vulkan startup check, and a dependency audit of
62 Mach-O files. Splatoon 3 11.3.0 subsequently passed the launch overlay and
rendered its opening scene after the fixes below. Online servers,
older macOS releases, Intel Macs, and the full unit suite have not been tested.

| Area | Location | macOS status |
| --- | --- | --- |
| Build and dependencies | `CMakeLists.txt`, `CMakeModules`, `externals` | CPM source dependencies, Homebrew Qt/FFmpeg, bundled MoltenVK |
| Desktop UI | `src/citron`, `src/citron_cmd` | SwiftUI/AppKit macOS frontend, legacy Qt app, and SDL command-line frontend |
| Existing Swift UI | `src/ios` | iOS-specific bridge and rendering view; a reference for later macOS work |
| CPU and kernel | `src/core/arm`, `src/core/hle/kernel` | Dynarmic and experimental Apple Hypervisor ARM64 on macOS; Linux/Android NCE remains separate |
| Guest memory | `src/common/host_memory.cpp`, `src/core/memory.cpp` | macOS uses the generic backing buffer without a fastmem arena |
| Graphics | `src/video_core`, `src/shader_recompiler` | Vulkan/Metal surface path and MoltenVK portability handling |
| Audio and input | `src/audio_core`, `src/input_common`, `src/hid_core` | CoreAudio via cubeb/SDL; libusb's Darwin frameworks linked |
| Networking and online services | `src/network`, `src/core/internal_network`, `src/web_service`, `src/core/hle/service/{acc,friend,ssl}`, `src/citron/grid0_friends_dialog.cpp` | GRID0 friends, private-server redirects, authentication, and CA trust retained |
| Tests and delivery | `src/tests`, `.github/workflows/release.yml` | Release job uses the same build/package script and requires macOS success |

The Dynarmic configuration now leaves its optional fastmem pointer disabled
when the host provides no arena, instead of enabling fastmem at address zero.
The pinned Dynarmic fork also needed an ARM64 Release fix: its cache insertions
were inside `assert()`, which removes those side effects with `NDEBUG`. The
versioned patch is checked and applied for both CPM and submodule builds.
The synthetic regression fails against the original fork and passes with the
patch, covering cache reuse, invalidation, and the emulator's tagged, strided,
absolute-offset page-table configuration:

```sh
cmake --build build-macos --target citrosis-cpu-smoke
./build-macos/bin/citrosis-cpu-smoke
```

Splatoon's first pipeline exposed a second crash: split descriptor sets could
pass a null set-0 layout when only set 1 had bindings. The renderer now creates
a valid empty set-0 layout and skips updates for its absent descriptor template.
macOS interface discovery reads the default IPv4 router from SystemConfiguration
instead of trying Linux's `/proc/net/route`.

Boot validation used a separate cloned data/config directory. The opening scene
rendered at up to 59 FPS, but this is not a full gameplay or visual-correctness
validation: MoltenVK still lacks geometry shaders, shader cull distance and
transform feedback. No Swift/Liquid Glass work or online protocol changes are part of this
initial build preparation.

## Developer probes

`tools/macos/test-native.sh`, `bisect-native.sh`, `ab-native.sh`, and
`benchmark-native.sh` are opt-in diagnostic tools for the legacy Qt executable.
Supply your own game path as an argument where supported or through `GAME_PATH`;
no game content or personal paths ship with the repository. Their disposable
profile under `build-macos/native-debug` must already contain the required user
keys and configuration. These scripts can launch/stop games and modify that
isolated profile; they are not part of the normal build or automated CI checks.
Build probes default to one job (`BUILD_JOBS` overrides this).

The native release workflow selects an installed Xcode with macOS SDK 26 or later.
A runner without that SDK fails explicitly; local packaging does not verify GitHub
Actions, Intel builds, or non-macOS platform behavior.
