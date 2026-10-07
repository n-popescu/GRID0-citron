// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/renderer_metal/metal_image.h"

namespace NativeMetal {
PixelFormat GuestPixelFormat(VideoCore::Surface::PixelFormat format) {
    using Guest = VideoCore::Surface::PixelFormat;
    switch (format) {
    case Guest::A8B8G8R8_UNORM:
        return PixelFormat::RGBA8;
    case Guest::A8B8G8R8_SNORM:
        return PixelFormat::RGBA8Snorm;
    case Guest::A8B8G8R8_UINT:
        return PixelFormat::RGBA8Uint;
    case Guest::A8B8G8R8_SINT:
        return PixelFormat::RGBA8Sint;
    case Guest::A8B8G8R8_SRGB:
        return PixelFormat::RGBA8Srgb;
    case Guest::B8G8R8A8_UNORM:
        return PixelFormat::BGRA8;
    case Guest::B8G8R8A8_SRGB:
        return PixelFormat::BGRA8Srgb;
    case Guest::R8_UNORM:
        return PixelFormat::R8;
    case Guest::R8_SNORM:
        return PixelFormat::R8Snorm;
    case Guest::R8_UINT:
        return PixelFormat::R8Uint;
    case Guest::R8_SINT:
        return PixelFormat::R8Sint;
    case Guest::R8G8_UNORM:
        return PixelFormat::RG8;
    case Guest::R8G8_SNORM:
        return PixelFormat::RG8Snorm;
    case Guest::R8G8_UINT:
        return PixelFormat::RG8Uint;
    case Guest::R8G8_SINT:
        return PixelFormat::RG8Sint;
    case Guest::R16_FLOAT:
        return PixelFormat::R16Float;
    case Guest::R16_UNORM:
        return PixelFormat::R16;
    case Guest::R16_SNORM:
        return PixelFormat::R16Snorm;
    case Guest::R16_UINT:
        return PixelFormat::R16Uint;
    case Guest::R16_SINT:
        return PixelFormat::R16Sint;
    case Guest::R16G16_FLOAT:
        return PixelFormat::RG16Float;
    case Guest::R16G16_UNORM:
        return PixelFormat::RG16;
    case Guest::R16G16_SNORM:
        return PixelFormat::RG16Snorm;
    case Guest::R16G16_UINT:
        return PixelFormat::RG16Uint;
    case Guest::R16G16_SINT:
        return PixelFormat::RG16Sint;
    case Guest::R16G16B16A16_FLOAT:
        return PixelFormat::RGBA16Float;
    case Guest::R16G16B16A16_UNORM:
        return PixelFormat::RGBA16;
    case Guest::R16G16B16A16_SNORM:
        return PixelFormat::RGBA16Snorm;
    case Guest::R16G16B16A16_UINT:
        return PixelFormat::RGBA16Uint;
    case Guest::R16G16B16A16_SINT:
        return PixelFormat::RGBA16Sint;
    case Guest::R32_FLOAT:
        return PixelFormat::R32Float;
    case Guest::R32_UINT:
        return PixelFormat::R32Uint;
    case Guest::R32_SINT:
        return PixelFormat::R32Sint;
    case Guest::R32G32_FLOAT:
        return PixelFormat::RG32Float;
    case Guest::R32G32_UINT:
        return PixelFormat::RG32Uint;
    case Guest::R32G32_SINT:
        return PixelFormat::RG32Sint;
    case Guest::R32G32B32A32_FLOAT:
        return PixelFormat::RGBA32Float;
    case Guest::R32G32B32A32_UINT:
        return PixelFormat::RGBA32Uint;
    case Guest::R32G32B32A32_SINT:
        return PixelFormat::RGBA32Sint;
    case Guest::A2B10G10R10_UNORM:
        return PixelFormat::RGB10A2;
    case Guest::A2B10G10R10_UINT:
        return PixelFormat::RGB10A2Uint;
    case Guest::B10G11R11_FLOAT:
        return PixelFormat::RG11B10Float;
    case Guest::D32_FLOAT:
        return PixelFormat::Depth32Float;
    // Packed formats: Metal names components LSB-first (DXGI style).
    case Guest::R5G6B5_UNORM:
        return PixelFormat::B5G6R5;
    case Guest::A1R5G5B5_UNORM:
        return PixelFormat::BGR5A1;
    case Guest::A4B4G4R4_UNORM:
        return PixelFormat::ABGR4; // verify channel order on hardware
    case Guest::A2R10G10B10_UNORM:
        return PixelFormat::BGR10A2;
    case Guest::E5B9G9R9_FLOAT:
        return PixelFormat::RGB9E5Float;
    case Guest::R16G16B16X16_FLOAT:
        return PixelFormat::RGBA16Float;
    case Guest::BC1_RGBA_UNORM:
        return PixelFormat::BC1;
    case Guest::BC1_RGBA_SRGB:
        return PixelFormat::BC1Srgb;
    case Guest::BC2_UNORM:
        return PixelFormat::BC2;
    case Guest::BC2_SRGB:
        return PixelFormat::BC2Srgb;
    case Guest::BC3_UNORM:
        return PixelFormat::BC3;
    case Guest::BC3_SRGB:
        return PixelFormat::BC3Srgb;
    case Guest::BC4_UNORM:
        return PixelFormat::BC4;
    case Guest::BC4_SNORM:
        return PixelFormat::BC4Snorm;
    case Guest::BC5_UNORM:
        return PixelFormat::BC5;
    case Guest::BC5_SNORM:
        return PixelFormat::BC5Snorm;
    case Guest::BC6H_UFLOAT:
        return PixelFormat::BC6HUfloat;
    case Guest::BC6H_SFLOAT:
        return PixelFormat::BC6HSfloat;
    case Guest::BC7_UNORM:
        return PixelFormat::BC7;
    case Guest::BC7_SRGB:
        return PixelFormat::BC7Srgb;
    case Guest::ASTC_2D_4X4_UNORM:
        return PixelFormat::ASTC4x4;
    case Guest::ASTC_2D_4X4_SRGB:
        return PixelFormat::ASTC4x4Srgb;
    case Guest::ASTC_2D_5X4_UNORM:
        return PixelFormat::ASTC5x4;
    case Guest::ASTC_2D_5X4_SRGB:
        return PixelFormat::ASTC5x4Srgb;
    case Guest::ASTC_2D_5X5_UNORM:
        return PixelFormat::ASTC5x5;
    case Guest::ASTC_2D_5X5_SRGB:
        return PixelFormat::ASTC5x5Srgb;
    case Guest::ASTC_2D_6X5_UNORM:
        return PixelFormat::ASTC6x5;
    case Guest::ASTC_2D_6X5_SRGB:
        return PixelFormat::ASTC6x5Srgb;
    case Guest::ASTC_2D_6X6_UNORM:
        return PixelFormat::ASTC6x6;
    case Guest::ASTC_2D_6X6_SRGB:
        return PixelFormat::ASTC6x6Srgb;
    case Guest::ASTC_2D_8X5_UNORM:
        return PixelFormat::ASTC8x5;
    case Guest::ASTC_2D_8X5_SRGB:
        return PixelFormat::ASTC8x5Srgb;
    case Guest::ASTC_2D_8X6_UNORM:
        return PixelFormat::ASTC8x6;
    case Guest::ASTC_2D_8X6_SRGB:
        return PixelFormat::ASTC8x6Srgb;
    case Guest::ASTC_2D_8X8_UNORM:
        return PixelFormat::ASTC8x8;
    case Guest::ASTC_2D_8X8_SRGB:
        return PixelFormat::ASTC8x8Srgb;
    case Guest::ASTC_2D_10X5_UNORM:
        return PixelFormat::ASTC10x5;
    case Guest::ASTC_2D_10X5_SRGB:
        return PixelFormat::ASTC10x5Srgb;
    case Guest::ASTC_2D_10X6_UNORM:
        return PixelFormat::ASTC10x6;
    case Guest::ASTC_2D_10X6_SRGB:
        return PixelFormat::ASTC10x6Srgb;
    case Guest::ASTC_2D_10X8_UNORM:
        return PixelFormat::ASTC10x8;
    case Guest::ASTC_2D_10X8_SRGB:
        return PixelFormat::ASTC10x8Srgb;
    case Guest::ASTC_2D_10X10_UNORM:
        return PixelFormat::ASTC10x10;
    case Guest::ASTC_2D_10X10_SRGB:
        return PixelFormat::ASTC10x10Srgb;
    case Guest::ASTC_2D_12X10_UNORM:
        return PixelFormat::ASTC12x10;
    case Guest::ASTC_2D_12X10_SRGB:
        return PixelFormat::ASTC12x10Srgb;
    case Guest::ASTC_2D_12X12_UNORM:
        return PixelFormat::ASTC12x12;
    case Guest::ASTC_2D_12X12_SRGB:
        return PixelFormat::ASTC12x12Srgb;
    case Guest::D16_UNORM:
        return PixelFormat::Depth16;
    case Guest::X8_D24_UNORM:
        return PixelFormat::Depth32Float; // converted on transfer
    case Guest::S8_UINT:
        return PixelFormat::Stencil8;
    case Guest::D24_UNORM_S8_UINT:
    case Guest::S8_UINT_D24_UNORM:
    case Guest::D32_FLOAT_S8_UINT:
        return PixelFormat::Depth32FloatStencil8; // converted on transfer
    // Same-sized stand-ins: channel order may differ, but memory layouts match.
    case Guest::B5G6R5_UNORM:
        return PixelFormat::B5G6R5;
    case Guest::A1B5G5R5_UNORM:
    case Guest::A5B5G5R1_UNORM:
        return PixelFormat::BGR5A1;
    case Guest::G4R4_UNORM:
        return PixelFormat::R8;
    // No Metal equivalent: placeholders whose contents are not transferred.
    case Guest::R32G32B32_FLOAT:
        return PixelFormat::RGBA32Float;
    default:
        return PixelFormat::RGBA8;
    }
}
bool GuestFormatSupported(VideoCore::Surface::PixelFormat format) {
    using Guest = VideoCore::Surface::PixelFormat;
    switch (format) {
    case Guest::R32G32B32_FLOAT:
    case Guest::ETC2_RGB_UNORM:
    case Guest::ETC2_RGBA_UNORM:
    case Guest::ETC2_RGB_PTA_UNORM:
    case Guest::ETC2_RGB_SRGB:
    case Guest::ETC2_RGBA_SRGB:
    case Guest::ETC2_RGB_PTA_SRGB:
    case Guest::Invalid:
        return false;
    default:
        return static_cast<size_t>(format) < static_cast<size_t>(Guest::MaxDepthStencilFormat);
    }
}
namespace {
size_t Product(size_t a, size_t b) {
    if (b && a > std::numeric_limits<size_t>::max() / b)
        throw std::overflow_error("Metal guest image size overflow");
    return a * b;
}
size_t Sum(size_t a, size_t b) {
    if (a > std::numeric_limits<size_t>::max() - b)
        throw std::overflow_error("Metal guest image offset overflow");
    return a + b;
}
} // namespace
namespace {
using Guest = VideoCore::Surface::PixelFormat;
enum class Conversion { None, X8D24, D24S8, S8D24, D32S8 };
Conversion GuestConversion(Guest format) {
    switch (format) {
    case Guest::X8_D24_UNORM:
        return Conversion::X8D24;
    case Guest::D24_UNORM_S8_UINT:
        return Conversion::D24S8;
    case Guest::S8_UINT_D24_UNORM:
        return Conversion::S8D24;
    case Guest::D32_FLOAT_S8_UINT:
        return Conversion::D32S8;
    default:
        return Conversion::None;
    }
}
bool IsCompressed(PixelFormat format) {
    return BlockWidth(format) > 1 || BlockHeight(format) > 1;
}
bool IsSrgb(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8Srgb:
    case PixelFormat::BGRA8Srgb:
    case PixelFormat::R8Srgb:
    case PixelFormat::RG8Srgb:
        return true;
    default:
        return false;
    }
}
} // namespace
CacheImage::CacheImage(ImageRuntime& runtime_, const VideoCommon::ImageInfo& desc, GPUVAddr gpu,
                       VAddr cpu)
    : ImageBase{desc, gpu, cpu}, runtime{&runtime_} {
    if (desc.num_samples != 1)
        LOG_WARNING(Render, "Metal: multisampled guest image ({} samples) allocated single-sampled",
                    desc.num_samples);
    if (!GuestFormatSupported(desc.format))
        LOG_WARNING(Render, "Metal: guest format {} has no Metal equivalent; contents are dropped",
                    static_cast<u32>(desc.format));
    TextureDesc native;
    native.width = desc.size.width;
    native.height = desc.size.height;
    native.depth = desc.size.depth;
    native.levels = static_cast<u32>(std::max(desc.resources.levels, 1));
    native.layers = static_cast<u32>(std::max(desc.resources.layers, 1));
    native.format = GuestPixelFormat(desc.format);
    const bool depth_stencil = IsDepthFormat(native.format) || HasStencil(native.format);
    const bool compressed = IsCompressed(native.format);
    native.usage = TextureUsage::Sample;
    if (!compressed && native.format != PixelFormat::RGB9E5Float)
        native.usage = native.usage | TextureUsage::RenderTarget;
    // Metal cannot write sRGB, depth/stencil or compressed textures from shaders.
    if (!depth_stencil && !compressed && !IsSrgb(native.format))
        native.usage = native.usage | TextureUsage::Storage;
    switch (desc.type) {
    case VideoCommon::ImageType::e1D:
        native.dimension = native.layers > 1 ? TextureDimension::D1Array : TextureDimension::D1;
        native.usage = compressed || depth_stencil ? TextureUsage::Sample
                                                   : TextureUsage::Sample | TextureUsage::Storage;
        break;
    case VideoCommon::ImageType::e2D:
    case VideoCommon::ImageType::Linear:
        native.dimension = native.layers > 1 ? TextureDimension::D2Array : TextureDimension::D2;
        break;
    case VideoCommon::ImageType::e3D:
        native.dimension = TextureDimension::D3;
        break;
    default:
        LOG_WARNING(Render, "Metal: unexpected guest image type {}; allocating a 2D image",
                    static_cast<u32>(desc.type));
        native.dimension = native.layers > 1 ? TextureDimension::D2Array : TextureDimension::D2;
        break;
    }
    try {
        texture = runtime->metal.CreateTexture(native);
    } catch (const std::exception& e) {
        // Leave the image without storage; views and copies of it become no-ops.
        LOG_ERROR(Render, "Metal: guest image {}x{}x{} format {} allocation failed: {}",
                  native.width, native.height, native.depth, static_cast<u32>(desc.format),
                  e.what());
    }
}
void CacheImage::UploadMemory(const Buffer& buffer, size_t offset,
                              std::span<const VideoCommon::BufferImageCopy> copies) {
    SafeTransfer(buffer, offset, copies, true, true, false);
}
void CacheImage::DownloadMemory(const Buffer& buffer, size_t offset,
                                std::span<const VideoCommon::BufferImageCopy> copies) {
    SafeTransfer(buffer, offset, copies, false, true, true);
}
void CacheImage::SafeTransfer(const Buffer& buffer, size_t offset,
                              std::span<const VideoCommon::BufferImageCopy> copies, bool upload,
                              bool synchronize, bool complete) {
    try {
        Transfer(buffer, offset, copies, upload, synchronize, complete);
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "Metal: guest image {} failed: {}", upload ? "upload" : "download",
                  e.what());
    }
}
void CacheImage::Transfer(const Buffer& buffer, size_t offset,
                          std::span<const VideoCommon::BufferImageCopy> copies, bool upload,
                          bool synchronize, bool complete) {
    if (!runtime)
        throw std::runtime_error("Cannot transfer a null Metal guest image");
    if (!texture.Valid() || !GuestFormatSupported(info.format))
        return;
    // Guest (buffer cache) buffers may have pending GPU writes: CPU packing waits for them
    // first. Direct GPU copies are ordered on the queue and need no wait.
    bool synchronized = !synchronize;
    bool recorded_direct = false;
    // Raw-buffer downloads (DMA) keep their completed-on-return contract; texture cache
    // staging downloads are finished by the cache itself.

    const auto ensure_cpu_access = [&] {
        if (!synchronized) {
            runtime->scheduler.Finish();
            synchronized = true;
        }
    };
    const PixelFormat native_format = texture.Format();
    const Conversion conversion = GuestConversion(info.format);
    const bool combined = IsDepthFormat(native_format) && HasStencil(native_format);
    const size_t guest_block = VideoCore::Surface::BytesPerBlock(info.format);
    const u32 block_w = BlockWidth(native_format);
    const u32 block_h = BlockHeight(native_format);
    if (conversion == Conversion::None && guest_block != BytesPerBlock(native_format))
        throw std::runtime_error("Metal guest image block size does not match its native format");
    for (const auto& copy : copies) {
        const auto& sub = copy.image_subresource;
        const auto& extent = copy.image_extent;
        if (sub.base_level < 0 || sub.base_layer < 0 || sub.num_layers <= 0 ||
            copy.image_offset.x < 0 || copy.image_offset.y < 0 || copy.image_offset.z < 0 ||
            !extent.width || !extent.height || !extent.depth)
            throw std::invalid_argument("Invalid Metal guest image transfer region");
        if (static_cast<u32>(sub.base_level) >= texture.Levels())
            throw std::out_of_range("Metal guest image transfer mip exceeds allocation");
        if (static_cast<u32>(sub.base_layer) >= texture.Layers() ||
            static_cast<u32>(sub.num_layers) > texture.Layers() - static_cast<u32>(sub.base_layer))
            throw std::out_of_range("Metal guest image transfer layers exceed allocation");
        const size_t rows = copy.buffer_image_height ? copy.buffer_image_height : extent.height;
        const size_t columns = copy.buffer_row_length ? copy.buffer_row_length : extent.width;
        if (rows < extent.height || columns < extent.width)
            throw std::invalid_argument("Metal guest transfer pitch is too small");
        // Sizes in blocks (texels for uncompressed formats).
        const size_t blocks_x = Common::DivCeil<size_t>(extent.width, block_w);
        const size_t blocks_y = Common::DivCeil<size_t>(extent.height, block_h);
        const size_t row_bytes = Product(blocks_x, guest_block);
        const size_t guest_pitch = Product(Common::DivCeil<size_t>(columns, block_w), guest_block);
        const size_t guest_image = Product(guest_pitch, Common::DivCeil<size_t>(rows, block_h));
        const size_t guest_layer = Product(guest_image, extent.depth);
        const size_t total = Product(guest_layer, static_cast<size_t>(sub.num_layers));
        const size_t begin = Sum(offset, copy.buffer_offset);
        if (begin > buffer.Size() || total > buffer.Size() - begin || total > copy.buffer_size)
            throw std::out_of_range("Metal guest image transfer exceeds buffer");
        // Layouts Metal can address directly are copied by the GPU straight between the
        // guest buffer and the image: no staging, no CPU packing, no wait.
        const bool direct = conversion == Conversion::None && guest_pitch % 4 == 0 &&
                            guest_image % 4 == 0 && guest_layer % 4 == 0 && begin % 4 == 0 &&
                            guest_pitch % guest_block == 0 && begin % guest_block == 0;
        if (direct) {
            recorded_direct = true;
            auto& commands = runtime->scheduler.Record();
            for (s32 layer = 0; layer < sub.num_layers; ++layer) {
                const size_t at = Sum(begin, Product(static_cast<size_t>(layer), guest_layer));
                const TextureRegion region{static_cast<u32>(sub.base_level),
                                           static_cast<u32>(sub.base_layer + layer),
                                           static_cast<u32>(copy.image_offset.x),
                                           static_cast<u32>(copy.image_offset.y),
                                           static_cast<u32>(copy.image_offset.z),
                                           extent.width,
                                           extent.height,
                                           extent.depth};
                if (upload)
                    commands.CopyBufferToTexture(buffer, at, guest_pitch, guest_image, texture,
                                                 region);
                else
                    commands.CopyTextureToBuffer(texture, region, buffer, at, guest_pitch,
                                                 guest_image);
            }
            continue;
        }
        ensure_cpu_access();
        std::byte* const guest_base = buffer.UnsafeContents();
        // Native staging: one plane for plain formats; depth (float) + stencil (u8) planes
        // for converted depth formats.
        const size_t depth_bytes = conversion == Conversion::None ? row_bytes : blocks_x * 4;
        const size_t depth_pitch = Common::AlignUp(depth_bytes, size_t{256});
        const size_t depth_image = Product(depth_pitch, blocks_y);
        const size_t depth_layer = Product(depth_image, extent.depth);
        const size_t stencil_pitch = Common::AlignUp(blocks_x, size_t{256});
        const size_t stencil_image = Product(stencil_pitch, blocks_y);
        const size_t stencil_layer = combined ? Product(stencil_image, extent.depth) : 0;
        for (s32 layer = 0; layer < sub.num_layers; ++layer) {
            const size_t guest_begin = Sum(begin, Product(static_cast<size_t>(layer), guest_layer));
            StreamAllocation upload_staging;
            Buffer staging;
            std::vector<std::byte> packed_download;
            std::byte* packed;
            if (upload) {
                upload_staging = runtime->scheduler.Stream(depth_layer + stencil_layer);
                packed = upload_staging.mapped.data();
            } else {
                staging = runtime->metal.CreateBuffer(depth_layer + stencil_layer);
                packed_download.resize(depth_layer + stencil_layer);
                packed = packed_download.data();
            }
            const auto guest_row = [&](u32 z, size_t y) {
                return Sum(guest_begin, Sum(Product(z, guest_image), Product(y, guest_pitch)));
            };
            const TextureRegion region{static_cast<u32>(sub.base_level),
                                       static_cast<u32>(sub.base_layer + layer),
                                       static_cast<u32>(copy.image_offset.x),
                                       static_cast<u32>(copy.image_offset.y),
                                       static_cast<u32>(copy.image_offset.z),
                                       extent.width,
                                       extent.height,
                                       extent.depth};
            if (upload) {
                std::vector<std::byte> row(row_bytes);
                for (u32 z = 0; z < extent.depth; ++z)
                    for (size_t y = 0; y < blocks_y; ++y) {
                        std::byte* depth_row = packed + z * depth_image + y * depth_pitch;
                        if (conversion == Conversion::None) {
                            std::memcpy(depth_row, guest_base + guest_row(z, y), row_bytes);
                            continue;
                        }
                        std::memcpy(row.data(), guest_base + guest_row(z, y), row_bytes);
                        std::byte* stencil_row =
                            packed + depth_layer + z * stencil_image + y * stencil_pitch;
                        for (size_t x = 0; x < blocks_x; ++x) {
                            float depth{};
                            u8 stencil{};
                            if (conversion == Conversion::D32S8) {
                                std::memcpy(&depth, row.data() + x * 8, 4);
                                stencil = static_cast<u8>(row[x * 8 + 4]);
                            } else {
                                u32 word{};
                                std::memcpy(&word, row.data() + x * 4, 4);
                                const u32 d24 = conversion == Conversion::S8D24 ? word >> 8
                                                                                : word & 0xFFFFFF;
                                stencil = static_cast<u8>(conversion == Conversion::S8D24
                                                              ? word & 0xFF
                                                              : word >> 24);
                                depth = static_cast<float>(d24) / 16777215.0f;
                            }
                            std::memcpy(depth_row + x * 4, &depth, 4);
                            if (combined)
                                stencil_row[x] = std::byte{stencil};
                        }
                    }
                auto& commands = runtime->scheduler.Record();
                const size_t base = upload_staging.offset;
                if (combined) {
                    commands.CopyBufferToTexture(upload_staging.buffer, base, depth_pitch,
                                                 depth_image, texture, region,
                                                 TransferPlane::Depth);
                    commands.CopyBufferToTexture(upload_staging.buffer, base + depth_layer,
                                                 stencil_pitch, stencil_image, texture, region,
                                                 TransferPlane::Stencil);
                } else {
                    commands.CopyBufferToTexture(upload_staging.buffer, base, depth_pitch,
                                                 depth_image, texture, region);
                }
                continue;
            }
            auto& commands = runtime->scheduler.Record();
            if (combined) {
                commands.CopyTextureToBuffer(texture, region, staging, 0, depth_pitch, depth_image,
                                             TransferPlane::Depth);
                commands.CopyTextureToBuffer(texture, region, staging, depth_layer, stencil_pitch,
                                             stencil_image, TransferPlane::Stencil);
            } else {
                commands.CopyTextureToBuffer(texture, region, staging, 0, depth_pitch, depth_image);
            }
            runtime->scheduler.Finish();
            staging.Read(0, packed_download);
            std::vector<std::byte> row(row_bytes);
            for (u32 z = 0; z < extent.depth; ++z)
                for (size_t y = 0; y < blocks_y; ++y) {
                    const std::byte* depth_row = packed + z * depth_image + y * depth_pitch;
                    if (conversion == Conversion::None) {
                        std::memcpy(guest_base + guest_row(z, y), depth_row, row_bytes);
                        continue;
                    }
                    const std::byte* stencil_row =
                        packed + depth_layer + z * stencil_image + y * stencil_pitch;
                    if (conversion == Conversion::D32S8)
                        std::memcpy(row.data(), guest_base + guest_row(z, y),
                                    row_bytes); // keep the padding bytes
                    for (size_t x = 0; x < blocks_x; ++x) {
                        float depth{};
                        std::memcpy(&depth, depth_row + x * 4, 4);
                        const u8 stencil = combined ? static_cast<u8>(stencil_row[x]) : 0;
                        if (conversion == Conversion::D32S8) {
                            std::memcpy(row.data() + x * 8, &depth, 4);
                            row[x * 8 + 4] = std::byte{stencil};
                            continue;
                        }
                        const u32 d24 = static_cast<u32>(
                            std::clamp(depth, 0.0f, 1.0f) * 16777215.0f + 0.5f);
                        u32 word = d24;
                        if (conversion == Conversion::S8D24)
                            word = (d24 << 8) | stencil;
                        else if (conversion == Conversion::D24S8)
                            word = d24 | (static_cast<u32>(stencil) << 24);
                        std::memcpy(row.data() + x * 4, &word, 4);
                    }
                    std::memcpy(guest_base + guest_row(z, y), row.data(), row_bytes);
                }
        }
    }
    if (!upload && complete && recorded_direct)
        runtime->scheduler.Finish();
}
} // namespace NativeMetal
