// SPDX-License-Identifier: GPL-3.0-or-later
// Link check for the Metal rasterizer: referencing its constructor pulls the rasterizer,
// its vtable and every buffer/texture-cache member it calls out of the static libraries,
// so missing explicit instantiations or definitions fail at link time. Nothing runs.
#include <cstdio>
#include <memory>
#include "video_core/renderer_metal/metal_rasterizer.h"

int main() {
    using Factory = std::unique_ptr<VideoCore::RasterizerInterface> (*)(
        Tegra::GPU&, Tegra::MaxwellDeviceMemoryManager&, NativeMetal::Runtime&);
    volatile Factory factory = [](Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& memory,
                                  NativeMetal::Runtime& metal)
        -> std::unique_ptr<VideoCore::RasterizerInterface> {
        return std::make_unique<NativeMetal::RasterizerMetal>(gpu, memory, metal);
    };
    std::printf("PASS: Metal rasterizer links (%s)\n", factory ? "factory resolved" : "?");
    return 0;
}
