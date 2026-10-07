# Native Metal renderer development

## October 7 optimization pass

The renderer now caches converted vertex/index data (64/32 MiB, 512 entries each)
using allocation identities and write versions, reuses adjacent serial compute
encoders, declares shader read-only storage resources precisely, and avoids repeated
buffer/inline-byte/depth-state binding calls. Repacking copies four bytes per kernel
invocation, including safe handling of unaligned sources and padded final elements.

Write versions cover uploads, writable CPU mappings, stream reuse, GPU transfers,
shader writes and backing texture aliases. Cached conversions use dedicated buffers;
queue ordering protects stale-buffer replacement without adding CPU waits. Whole-buffer
invalidation is conservative and can reduce cache hits when unrelated regions change.

The `Metal perf` log includes `vertex repack` and `index convert` hits/misses/KB and
`bindings skipped`, all averaged per frame. The release package built successfully;
no runtime or gameplay testing was performed, at the user's request. Stable 60 FPS and
visual correctness remain unverified. See the latest handoff for exact build evidence.


Updated October 5, 2026.

## Status

The native GPU runtime and shader backend are implemented and tested on an Apple
M2 running macOS 27. They call Metal directly; they do not create a Vulkan device
or use MoltenVK. The buffer-cache, guest-image and texture-cache adapters pass their
GPU smoke tests with Metal API and shader validation.

**October 5 (latest): the whole draw path, a `RendererMetal` and a "Metal" renderer
setting were written in one pass and have not been compiled on the Mac yet.** The
C++ was syntax-checked with g++ against the real include tree; the Objective-C++
runtime changes could not be compiled at all. Expect a build-fix round, then
validation-layer fixes. Nothing here is evidence of correct rendering or of any FPS
change. GRID0(+) code is unchanged and Vulkan remains the default renderer.

The user installed a save and reports the Splatoon plaza's 30 FPS as normal.
Compare the same gameplay scene at 1× docked resolution when measuring future
changes; do not count plaza 30 FPS as a renderer regression.

## Implemented components

- `src/video_core/renderer_metal/metal_runtime.h` / `.mm`: C++ handles owning
  native Metal objects, with Objective-C confined to the implementation. Device
  creation requires Metal 3 and tier-2 argument buffers. Compilation uses MSL 3.0
  and disables fast math.
- Command queue, ordered submissions, render/compute/blit encoders, shared
  buffers, private textures, aligned uploads and readbacks. Buffers retain their
  last submission fence even if callers discard the returned submission. CPU
  access waits for submitted GPU work and rejects access to an unsubmitted
  recording; canceled recordings release ownership. Recording/binding APIs are
  intended for one render thread.
- Render and compute pipeline caches, indexed/instanced draws, vertex layouts,
  viewport/scissor, multiple color targets, depth and depth-only passes. Native
  `CAMetalLayer` presentation acquires and presents drawables with aspect-fit
  scaling. Current format, sampler, blending and depth-state coverage is limited.
- `metal_shader.h` / `.cpp`: the shared Maxwell frontend generates IR and SPIR-V;
  pinned SPIRV-Cross generates MSL. SPIR-V here is compiler intermediate data,
  not a Vulkan renderer. Guest compute, vertex B (including optional merged
  vertex A), and fragment translation entry points return shader metadata.
  Geometry and tessellation stages explicitly report unsupported operation.
- Descriptor reflection maps sets 0 and 1 to argument buffers at slots 26 and
  27. Shared member IDs prevent texture/sampler collisions. Typed descriptor
  aliases share IDs and use Metal 3 lowering. Push constants use slot 28 and
  SPIRV-Cross's optional buffer-size table uses slot 30. Future guest resource
  binding must supply that table when `needs_buffer_sizes` is true.
- `metal_scheduler.*` maintains one ordered guest command stream with explicit
  flush and finish operations. `metal_buffer_cache.*` adapts the existing
  `VideoCommon::BufferCache` template; the complete template is instantiated in
  `video_core`, so all shared cache methods compile against the native adapter.
  Transfers, fills, overlapping copies, staged uploads/downloads, uniform/storage
  and vertex/index binding metadata are implemented. Unsupported primitive
  topologies and transform feedback fail explicitly. Unsigned-byte indices and
  quads/fans/line loops are converted; conversion currently waits for the GPU
  and runs on the CPU. Primitive-restart configuration still needs integrating.
  Texel-buffer bindings retain metadata; their shader descriptor wiring remains.
- Native textures now support mip levels, array layers, 3D regions, texture
  views with channel swizzles, buffer-backed textures and image-to-image copies.
  Buffer-backed views track their backing buffer's completion fence, preventing
  CPU access from racing shader writes. Common uncompressed color formats have
  native mappings. Compressed and packed depth/stencil guest format conversions
  remain unfinished.
- `metal_image.*` derives from shared `ImageBase` and implements mip, layer, 3D,
  and partial-region transfers for the supported formats. It packs tightly spaced
  guest rows into aligned Metal staging rows and preserves untouched padding on
  download. This is a correctness baseline: packing/writeback waits for prior
  GPU work, so it is not a demonstrated performance optimization. These image
  objects are not yet managed by `VideoCommon::TextureCache`.

- `metal_texture_cache.*` (October 5, later; **compile-checked only, not yet built or
  run on the Mac**): `TextureCacheRuntime` (derives `ImageRuntime`), `CacheImageView`,
  `CacheSampler`, `CacheFramebuffer`, `CacheImageAlloc` and `TextureCacheParams`.
  `metal_texture_cache_inst.cpp` instantiates the complete shared
  `VideoCommon::TextureCache` template against them. Implemented: staging buffers,
  same-format and same-size reinterpreting image copies (through a destination view),
  unscaled `SrcCopy` 2D blits, lazily created sampled views per shader texture type
  (incompatible types return an empty handle), swizzles (render-target sentinel maps to
  identity), cube/array views, null-descriptor placeholder textures, unswizzled
  render-target views, framebuffers keyed by render-target index with unbound gaps, and
  TSC sampler translation. Explicitly unimplemented (throw): scaled/filtered/ROP blits,
  MSAA copies, reinterpretation between differently sized formats, render-pass
  conversion, accelerated uploads. Metal has no sampler LOD bias (ignored, debug log) or
  min/max reduction (warning); border colors map to the nearest of Metal's three.
- `metal_rasterizer.{h,cpp}` (**compile-checked only; link-checked by
  `citrosis-metal-rasterizer-link-check` once built on the Mac**): `RasterizerMetal`
  implements `VideoCore::RasterizerInterface` and owns the scheduler, the shared
  `BufferCache` and `TextureCache` instances and a DMA adapter. Implemented: memory
  coherency hooks (flush/invalidate/CPU write/unmap/GPU remap, mirroring the Vulkan
  rasterizer without its pipeline/query caches), channels, buffer DMA copy/clear,
  buffer↔image DMA, Fermi 2D surface copies, inline-to-memory, full-surface color and
  depth clears through render-pass load actions (per-layer views for layered targets;
  Vulkan's integer clear conversion), semaphore/query reports (payload, with timestamp
  when requested; host counters are not implemented, so counters report their payload
  like the null backend), and `AccelerateDisplay` returning the presentable texture.
  Fences and syncpoints are **synchronous** (each waits for the GPU) — correct, slow;
  an asynchronous `VideoCommon::FenceManager` adapter is future work. Draws, indirect
  draws, DrawTexture, compute dispatch, scissored clears and color-masked clears throw.
- Runtime additions for the adapter: `SamplerDesc`/`CreateSampler(const SamplerDesc&)`,
  `Texture::Valid()` / `Texture::Dimension()`, and render passes/pipelines that accept
  empty color attachments (unbound guest render-target indices).

SPIRV-Cross is built from commit
`d8e3e2b141b8c8a167b2e3984736a6baacff316c` (vulkan-sdk-1.4.321.0) by default, so
its deployment target follows the project. The explicit
`CITROSIS_METAL_USE_SYSTEM_SPIRV_CROSS=ON` alternative uses installed packages;
those archives can have a newer minimum macOS version.

## Build and checks

`CITROSIS_BUILD_METAL` defaults to ON on Apple platforms and is unavailable
elsewhere. Both Objective-C and Objective-C++ are enabled: enabling only the
latter breaks SDL's Objective-C sources. Diagnostic executables are excluded
from the default build. `glslangValidator` is optional for the renderer core,
but needed to generate the smoke-test GLSL fixtures.

```sh
cmake --build build-macos --target \
  citrosis-metal-smoke citrosis-metal-present-smoke citrosis-metal-guest-smoke -j4

cmake --build build-macos --target \
  citrosis-metal-buffer-smoke citrosis-metal-image-smoke -j4

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build-macos/bin/citrosis-metal-smoke \
  build-macos/src/video_core/renderer_metal/fixtures

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build-macos/bin/citrosis-metal-present-smoke

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build-macos/bin/citrosis-metal-buffer-smoke
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build-macos/bin/citrosis-metal-image-smoke

cmake --build build-macos --target citrosis-metal-texture-cache-smoke \
  citrosis-metal-rasterizer-link-check -j4
build-macos/bin/citrosis-metal-rasterizer-link-check
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build-macos/bin/citrosis-metal-texture-cache-smoke

build-macos/bin/citrosis-metal-smoke --compile-spv captured-shader.spv
build-macos/bin/citrosis-metal-smoke --translate-spv input.spv output.metal

build-macos/bin/citrosis-metal-guest-smoke \
  build-macos/native-debug/data/citron/shader/0100c2500fc20000/vulkan.bin \
  build-macos/metal-debug/cache-replay.bin
```

The last diagnostic reads version-15 captured Maxwell environments using a
**disposable copy** of the cache because the existing cache loader can delete
invalid files. Vulkan structs describe the input capture format only; this
diagnostic does not invoke Vulkan. It compiles up to 32 compute environments
and creates native compute pipelines. It does not dispatch the captured game
workloads or validate their output. Generated compute MSL is written alongside
the disposable copy. Graphics captures are skipped.

Verified results, October 5:

| Check | Result | Evidence under `build-macos/metal-debug/` |
| --- | --- | --- |
| GUI, CLI and five Metal diagnostics build | Pass | `build-resources-final.log` |
| Buffer ownership, cancellation, aligned transfer, indexed/scissored/depth/MRT drawing, vertex inputs, native compute and descriptor arrays | Pass with Metal API and shader validation | `validation-final.txt` |
| Native window presentation | 90 drawable presentations, validation enabled | `present-validation.txt` |
| Captured Splatoon SPIR-V → MSL → native functions | 269 passed, 0 failed | `splatoon-shaders-final.txt` |
| Captured Maxwell → new Metal host profile → native compute pipelines | 6 passed, 0 failed, validation enabled | `maxwell-compute.txt` |
| Shared buffer adapter: GPU fill/compute writeback, uniform retention, overlapping copies, converted indexed drawing | Pass with API/shader validation | `buffer-validation.txt` |
| Native mip/layer/view/swizzle, image copy and buffer-texture alias synchronization | Pass with API/shader validation | `texture-validation.txt` |
| Guest-image padded-row, mip/array/3D roundtrips and partial updates | Pass with API/shader validation | `image-validation.txt` |

Function compilation does not prove correct rendering, resource bindings, or
graphics pipeline linking. These captures cover previously exercised scenes,
not the entire game. Existing FFmpeg/OpenSSL dependencies still warn that they
target macOS 27 while the project deployment target is 14; older macOS support
has not been validated. Intel and other GPUs have not been tested.

## Draw path (written October 5; not yet built on the Mac)

| File | Contents |
| --- | --- |
| `metal_pipeline_common.{h,cpp}` | `ArgumentPool` (recycles argument-buffer clones the GPU is done with), `StageProgram` (compiled stage + SPIR-V set-1 binding map), `ResourceBinder` (fills argument buffers from buffer/texture-cache bindings with null placeholders; texel buffers via buffer textures, copied when misaligned; storage-buffer size tables), push constants |
| `metal_pipeline.{h,cpp}` | `GraphicsPipelineKey` (= Vulkan `FixedPipelineState` with every dynamic feature off + stage hashes), `GraphicsPipeline` (port of Vulkan `ConfigureImpl`; Metal pipeline variants per framebuffer format signature; blend/write masks; vertex descriptor from Maxwell attributes; depth-stencil state from key + stencil masks), `ComputePipeline` |
| `metal_pipeline_cache.{h,cpp}` | `PipelineCache : VideoCommon::ShaderCache`; Maxwell → SPIR-V (Metal profile, fresh `Bindings` per stage) → MSL. Geometry/tessellation pipelines are logged and skipped |
| `metal_rasterizer.cpp` | Draws (direct, converted quads/fans/loops/polygons, instanced), indirect draws (count buffers read back synchronously; byte-count draws skipped), compute + indirect dispatch, viewports (negative heights for lower-left origin), scissors, cull/front face, depth bias/clamp, stencil reference, blend color, line fill; scissored/masked/stencil clears by draw; `Vulkan::StateTracker` dirty tables; shader-cache invalidation hooks |
| `renderer_metal.{h,cpp}` | `RendererMetal`: texture-cache framebuffer (or CPU-deswizzled guest memory) presented into the frontend's `CAMetalLayer` with crop and screen layout; two frames in flight |
| Frontend | `RendererBackend::Metal` (appended, so saved configs stay valid), translations, Qt uses the Vulkan native child window, status-bar toggle Vulkan ↔ Metal on macOS |

Runtime additions (`metal_runtime.{h,mm}`): `ArgumentBuffer::Busy`/`Valid`,
`Runtime::CloneArguments` (shared, mutex-guarded encoder; every member write re-targets
it), attachment-less passes, `RenderPassId`, indirect draw/dispatch, negative viewport
heights, `Buffer::Valid`, `Texture::Usage/SampleCount/operator==`, and a `Present`
overload with drawable size, destination rect and crop. `CompileSpirv` gains
`flip_vertex_y` (on for guest shaders, as MoltenVK does) and reports the per-set
`spvBufferSizeConstants` member. `CacheImageView::StorageView` and null storage/texel
placeholders were added to the texture-cache adapter.

Pipeline cache: `metal_pipeline_cache` records each new pipeline's guest environments in
`shader/<title id>/metal.bin` (same transferable format as `vulkan.bin`, separate file and
version) and rebuilds them on worker threads during the loading screen. With asynchronous
shaders on, new pipelines translate on the GPU thread and compile MSL on workers; their draws
are skipped until ready. Metal's own system cache additionally caches compiled MSL.

Fences: `VideoCommon::FenceManager` with Metal submissions (`metal_fence_manager.h`) performs the
asynchronous GPU -> guest memory downloads (buffers, images, occlusion queries) that guest CPU
code depends on (e.g. Splatoon ink state).

Texture-cache adapter rule: nothing may throw into `VideoCommon::TextureCache` (it would be
left with dangling ids). Unsupported operations log "Metal texture cache: … skipped";
unsupported guest formats get placeholders (`GuestFormatSupported`); view formats Metal cannot
reinterpret (`CanViewAs`) fall back to the image's own format.

Known gaps (logged, not silent): ROP/blending 2D blits, reinterpretation between differently sized formats, ETC2 and RGB32F contents, geometry/tessellation, transform feedback, `DrawTexture`,
logic ops, MSAA, query counters other than occlusion (ZPassPixelCount64 is implemented), screenshots, applet capture, overlay layers,
min/max sampler reduction and LOD bias, misaligned (non-4-byte) vertex/texel offsets,
8-bit indices in indirect draws, resolution scaling. Per-draw costs addressed in the October 5 performance pass (stream allocator, GPU kernels,
argument-buffer reuse, cached buffer textures/depth states, asynchronous fences); see
handoff.md. Use the "Metal perf:" log line to see where frame time goes.

To try it: build, select **Graphics → API → Metal (native, experimental)** (or set
`backend` to Metal in the per-game config), then run with `MTL_DEBUG_LAYER=1` from a
terminal. Errors on the draw path are rate-limited in the log.

## Remaining work

1. Build fixes, then run every smoke test again (runtime pipeline/pass code changed) and
   launch a title with the Metal API validation layer; fix what it reports.
2. Correctness passes on real scenes: y-flip/winding, depth range, blend, sRGB, format
   coverage, image views used as both sampled and storage, feedback loops.
3. Geometry-shader (layer passthrough) and tessellation emulation; transform feedback.
4. Performance: uniform ring buffer, cached buffer textures and depth-stencil states,
   GPU index conversion, asynchronous fences (`VideoCommon::FenceManager`), on-disk MSL
   cache with its own identity, async pipeline builds.
5. Verify Splatoon boot, plaza, lobby and gameplay at 1× docked; benchmark against
   Vulkan with the same save and scene. No FPS claim until measured.

## October 7: persistent shader bounds metadata

Cached shader argument buffers previously retained buffer-size constants allocated
from the temporary stream ring. The ring could recycle those bytes after completion
while cached arguments still referenced them. Each argument-buffer clone now owns a
persistent size-table buffer, updated only when the clone is idle. Reusing arguments
therefore also preserves their bounds metadata across frames and scene transitions.

Release build/package passed (`metal-persistent-argument-tables-package.log`), including
signing and 21 Mach-O dependency audits. Gameplay verification remains with the user;
this fixes a proven lifetime error, but the supplied scene-switch rendering corruption
has not been reproduced or verified resolved by the agent.
