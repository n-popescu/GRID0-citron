// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_metal/metal_image.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main() try {
    using namespace NativeMetal;
    Runtime metal;
    Scheduler scheduler{metal};
    ImageRuntime runtime{metal, scheduler};
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    info.size = {14, 10, 1};
    info.resources = {2, 2};
    info.layer_stride = 1024;
    CacheImage image{runtime, info, 0x10000, 0x10000};
    constexpr size_t offset = 13, pitch = 9 * 4, rows = 6, layer_size = pitch * rows;
    auto source = metal.CreateBuffer(offset + layer_size * 2);
    std::vector<std::byte> input(source.Size(), std::byte{99});
    for (size_t layer = 0; layer < 2; ++layer)
        for (size_t y = 0; y < 5; ++y)
            for (size_t x = 0; x < 7; ++x) {
                const size_t pos = offset + layer * layer_size + y * pitch + x * 4;
                input[pos] = std::byte(40 + layer);
                input[pos + 1] = std::byte(y);
                input[pos + 2] = std::byte(x);
                input[pos + 3] = std::byte{255};
            }
    source.Write(0, input);
    const std::array copies{
        VideoCommon::BufferImageCopy{0, layer_size * 2, 9, 6, {1, 0, 2}, {0, 0, 0}, {7, 5, 1}}};
    image.UploadMemory(source, offset, copies);
    auto destination = metal.CreateBuffer(source.Size());
    std::vector<std::byte> sentinel(destination.Size(), std::byte{77});
    destination.Write(0, sentinel);
    image.DownloadMemory(destination, offset, copies);
    std::vector<std::byte> output(destination.Size());
    destination.Read(0, output);
    auto expected = sentinel;
    for (size_t layer = 0; layer < 2; ++layer)
        for (size_t y = 0; y < 5; ++y) {
            const size_t pos = offset + layer * layer_size + y * pitch;
            std::copy_n(input.begin() + pos, 7 * 4, expected.begin() + pos);
        }
    Check(output == expected, "Guest image roundtrip changed pixels or untouched padding");
    // Partial-region update must preserve the rest of the existing mip.
    auto patch = metal.CreateBuffer(16);
    const std::array<std::byte, 16> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
                                        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
                                        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
                                        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
    patch.Write(0, red);
    const std::array update{
        VideoCommon::BufferImageCopy{0, 16, 2, 2, {1, 1, 1}, {3, 2, 0}, {2, 2, 1}}};
    image.UploadMemory(patch, 0, update);
    image.DownloadMemory(destination, offset, copies);
    destination.Read(0, output);
    for (size_t y = 0; y < 2; ++y)
        for (size_t x = 0; x < 2; ++x) {
            const size_t pos = offset + layer_size + (2 + y) * pitch + (3 + x) * 4;
            std::copy_n(red.begin(), 4, expected.begin() + pos);
        }
    Check(output == expected, "Guest partial image upload overwrote neighboring pixels");
    VideoCommon::ImageInfo volume = info;
    volume.type = VideoCommon::ImageType::e3D;
    volume.size = {3, 2, 2};
    volume.resources = {1, 1};
    CacheImage image3d{runtime, volume, 0x20000, 0x20000};
    auto voxels = metal.CreateBuffer(48);
    std::vector<std::byte> voxel_data(48);
    for (size_t i = 0; i < 48; ++i)
        voxel_data[i] = std::byte(i);
    voxels.Write(0, voxel_data);
    const std::array volume_copy{
        VideoCommon::BufferImageCopy{0, 48, 3, 2, {0, 0, 1}, {0, 0, 0}, {3, 2, 2}}};
    image3d.UploadMemory(voxels, 0, volume_copy);
    auto returned = metal.CreateBuffer(48);
    image3d.DownloadMemory(returned, 0, volume_copy);
    std::vector<std::byte> returned_data(48);
    returned.Read(0, returned_data);
    Check(returned_data == voxel_data, "Guest 3D image transfer lost depth slices");
    bool rejected = false;
    try {
        image.UploadMemory(patch, 0, copies);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Check(rejected, "Guest image accepted an undersized buffer");
    std::cout << "PASS: native guest-image mip/array and 3D roundtrips, guest row padding, "
                 "partial updates, and transfer bounds\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
