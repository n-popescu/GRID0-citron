// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/metal_runtime.h"
#include "video_core/renderer_metal/metal_shader.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NativeMetal {
namespace {
struct DeviceState;
std::string Message(NSError* error) {
    return error ? std::string(error.localizedDescription.UTF8String) : "no native error detail";
}
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void CheckRange(size_t offset, size_t size, size_t capacity) {
    if (offset > capacity || size > capacity - offset)
        throw std::out_of_range("Metal buffer range exceeds its allocation");
}
struct FormatInfo {
    MTLPixelFormat format;
    uint32_t block_width, block_height, bytes;
};
FormatInfo Describe(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8:
        return {MTLPixelFormatRGBA8Unorm, 1, 1, 4};
    case PixelFormat::BGRA8:
        return {MTLPixelFormatBGRA8Unorm, 1, 1, 4};
    case PixelFormat::RGBA16Float:
        return {MTLPixelFormatRGBA16Float, 1, 1, 8};
    case PixelFormat::R32Uint:
        return {MTLPixelFormatR32Uint, 1, 1, 4};
    case PixelFormat::Depth32Float:
        return {MTLPixelFormatDepth32Float, 1, 1, 4};
    case PixelFormat::RGBA8Srgb:
        return {MTLPixelFormatRGBA8Unorm_sRGB, 1, 1, 4};
    case PixelFormat::BGRA8Srgb:
        return {MTLPixelFormatBGRA8Unorm_sRGB, 1, 1, 4};
    case PixelFormat::RGBA8Snorm:
        return {MTLPixelFormatRGBA8Snorm, 1, 1, 4};
    case PixelFormat::RGBA8Uint:
        return {MTLPixelFormatRGBA8Uint, 1, 1, 4};
    case PixelFormat::RGBA8Sint:
        return {MTLPixelFormatRGBA8Sint, 1, 1, 4};
    case PixelFormat::R8:
        return {MTLPixelFormatR8Unorm, 1, 1, 1};
    case PixelFormat::R8Snorm:
        return {MTLPixelFormatR8Snorm, 1, 1, 1};
    case PixelFormat::R8Uint:
        return {MTLPixelFormatR8Uint, 1, 1, 1};
    case PixelFormat::R8Sint:
        return {MTLPixelFormatR8Sint, 1, 1, 1};
    case PixelFormat::RG8:
        return {MTLPixelFormatRG8Unorm, 1, 1, 2};
    case PixelFormat::RG8Snorm:
        return {MTLPixelFormatRG8Snorm, 1, 1, 2};
    case PixelFormat::RG8Uint:
        return {MTLPixelFormatRG8Uint, 1, 1, 2};
    case PixelFormat::RG8Sint:
        return {MTLPixelFormatRG8Sint, 1, 1, 2};
    case PixelFormat::R16Float:
        return {MTLPixelFormatR16Float, 1, 1, 2};
    case PixelFormat::R16:
        return {MTLPixelFormatR16Unorm, 1, 1, 2};
    case PixelFormat::R16Snorm:
        return {MTLPixelFormatR16Snorm, 1, 1, 2};
    case PixelFormat::R16Uint:
        return {MTLPixelFormatR16Uint, 1, 1, 2};
    case PixelFormat::R16Sint:
        return {MTLPixelFormatR16Sint, 1, 1, 2};
    case PixelFormat::RG16Float:
        return {MTLPixelFormatRG16Float, 1, 1, 4};
    case PixelFormat::RG16:
        return {MTLPixelFormatRG16Unorm, 1, 1, 4};
    case PixelFormat::RG16Snorm:
        return {MTLPixelFormatRG16Snorm, 1, 1, 4};
    case PixelFormat::RG16Uint:
        return {MTLPixelFormatRG16Uint, 1, 1, 4};
    case PixelFormat::RG16Sint:
        return {MTLPixelFormatRG16Sint, 1, 1, 4};
    case PixelFormat::RGBA16:
        return {MTLPixelFormatRGBA16Unorm, 1, 1, 8};
    case PixelFormat::RGBA16Snorm:
        return {MTLPixelFormatRGBA16Snorm, 1, 1, 8};
    case PixelFormat::RGBA16Uint:
        return {MTLPixelFormatRGBA16Uint, 1, 1, 8};
    case PixelFormat::RGBA16Sint:
        return {MTLPixelFormatRGBA16Sint, 1, 1, 8};
    case PixelFormat::R32Float:
        return {MTLPixelFormatR32Float, 1, 1, 4};
    case PixelFormat::R32Sint:
        return {MTLPixelFormatR32Sint, 1, 1, 4};
    case PixelFormat::RG32Float:
        return {MTLPixelFormatRG32Float, 1, 1, 8};
    case PixelFormat::RG32Uint:
        return {MTLPixelFormatRG32Uint, 1, 1, 8};
    case PixelFormat::RG32Sint:
        return {MTLPixelFormatRG32Sint, 1, 1, 8};
    case PixelFormat::RGBA32Float:
        return {MTLPixelFormatRGBA32Float, 1, 1, 16};
    case PixelFormat::RGBA32Uint:
        return {MTLPixelFormatRGBA32Uint, 1, 1, 16};
    case PixelFormat::RGBA32Sint:
        return {MTLPixelFormatRGBA32Sint, 1, 1, 16};
    case PixelFormat::RGB10A2:
        return {MTLPixelFormatRGB10A2Unorm, 1, 1, 4};
    case PixelFormat::RGB10A2Uint:
        return {MTLPixelFormatRGB10A2Uint, 1, 1, 4};
    case PixelFormat::RG11B10Float:
        return {MTLPixelFormatRG11B10Float, 1, 1, 4};
    case PixelFormat::B5G6R5:
        return {MTLPixelFormatB5G6R5Unorm, 1, 1, 2};
    case PixelFormat::BGR5A1:
        return {MTLPixelFormatBGR5A1Unorm, 1, 1, 2};
    case PixelFormat::ABGR4:
        return {MTLPixelFormatABGR4Unorm, 1, 1, 2};
    case PixelFormat::RGB9E5Float:
        return {MTLPixelFormatRGB9E5Float, 1, 1, 4};
    case PixelFormat::BGR10A2:
        return {MTLPixelFormatBGR10A2Unorm, 1, 1, 4};
    case PixelFormat::R8Srgb:
        return {MTLPixelFormatR8Unorm_sRGB, 1, 1, 1};
    case PixelFormat::RG8Srgb:
        return {MTLPixelFormatRG8Unorm_sRGB, 1, 1, 2};
    case PixelFormat::Depth16:
        return {MTLPixelFormatDepth16Unorm, 1, 1, 2};
    case PixelFormat::Depth32FloatStencil8:
        return {MTLPixelFormatDepth32Float_Stencil8, 1, 1, 8};
    case PixelFormat::Stencil8:
        return {MTLPixelFormatStencil8, 1, 1, 1};
    case PixelFormat::BC1:
        return {MTLPixelFormatBC1_RGBA, 4, 4, 8};
    case PixelFormat::BC1Srgb:
        return {MTLPixelFormatBC1_RGBA_sRGB, 4, 4, 8};
    case PixelFormat::BC2:
        return {MTLPixelFormatBC2_RGBA, 4, 4, 16};
    case PixelFormat::BC2Srgb:
        return {MTLPixelFormatBC2_RGBA_sRGB, 4, 4, 16};
    case PixelFormat::BC3:
        return {MTLPixelFormatBC3_RGBA, 4, 4, 16};
    case PixelFormat::BC3Srgb:
        return {MTLPixelFormatBC3_RGBA_sRGB, 4, 4, 16};
    case PixelFormat::BC4:
        return {MTLPixelFormatBC4_RUnorm, 4, 4, 8};
    case PixelFormat::BC4Snorm:
        return {MTLPixelFormatBC4_RSnorm, 4, 4, 8};
    case PixelFormat::BC5:
        return {MTLPixelFormatBC5_RGUnorm, 4, 4, 16};
    case PixelFormat::BC5Snorm:
        return {MTLPixelFormatBC5_RGSnorm, 4, 4, 16};
    case PixelFormat::BC6HUfloat:
        return {MTLPixelFormatBC6H_RGBUfloat, 4, 4, 16};
    case PixelFormat::BC6HSfloat:
        return {MTLPixelFormatBC6H_RGBFloat, 4, 4, 16};
    case PixelFormat::BC7:
        return {MTLPixelFormatBC7_RGBAUnorm, 4, 4, 16};
    case PixelFormat::BC7Srgb:
        return {MTLPixelFormatBC7_RGBAUnorm_sRGB, 4, 4, 16};
    case PixelFormat::ASTC4x4:
        return {MTLPixelFormatASTC_4x4_LDR, 4, 4, 16};
    case PixelFormat::ASTC4x4Srgb:
        return {MTLPixelFormatASTC_4x4_sRGB, 4, 4, 16};
    case PixelFormat::ASTC5x4:
        return {MTLPixelFormatASTC_5x4_LDR, 5, 4, 16};
    case PixelFormat::ASTC5x4Srgb:
        return {MTLPixelFormatASTC_5x4_sRGB, 5, 4, 16};
    case PixelFormat::ASTC5x5:
        return {MTLPixelFormatASTC_5x5_LDR, 5, 5, 16};
    case PixelFormat::ASTC5x5Srgb:
        return {MTLPixelFormatASTC_5x5_sRGB, 5, 5, 16};
    case PixelFormat::ASTC6x5:
        return {MTLPixelFormatASTC_6x5_LDR, 6, 5, 16};
    case PixelFormat::ASTC6x5Srgb:
        return {MTLPixelFormatASTC_6x5_sRGB, 6, 5, 16};
    case PixelFormat::ASTC6x6:
        return {MTLPixelFormatASTC_6x6_LDR, 6, 6, 16};
    case PixelFormat::ASTC6x6Srgb:
        return {MTLPixelFormatASTC_6x6_sRGB, 6, 6, 16};
    case PixelFormat::ASTC8x5:
        return {MTLPixelFormatASTC_8x5_LDR, 8, 5, 16};
    case PixelFormat::ASTC8x5Srgb:
        return {MTLPixelFormatASTC_8x5_sRGB, 8, 5, 16};
    case PixelFormat::ASTC8x6:
        return {MTLPixelFormatASTC_8x6_LDR, 8, 6, 16};
    case PixelFormat::ASTC8x6Srgb:
        return {MTLPixelFormatASTC_8x6_sRGB, 8, 6, 16};
    case PixelFormat::ASTC8x8:
        return {MTLPixelFormatASTC_8x8_LDR, 8, 8, 16};
    case PixelFormat::ASTC8x8Srgb:
        return {MTLPixelFormatASTC_8x8_sRGB, 8, 8, 16};
    case PixelFormat::ASTC10x5:
        return {MTLPixelFormatASTC_10x5_LDR, 10, 5, 16};
    case PixelFormat::ASTC10x5Srgb:
        return {MTLPixelFormatASTC_10x5_sRGB, 10, 5, 16};
    case PixelFormat::ASTC10x6:
        return {MTLPixelFormatASTC_10x6_LDR, 10, 6, 16};
    case PixelFormat::ASTC10x6Srgb:
        return {MTLPixelFormatASTC_10x6_sRGB, 10, 6, 16};
    case PixelFormat::ASTC10x8:
        return {MTLPixelFormatASTC_10x8_LDR, 10, 8, 16};
    case PixelFormat::ASTC10x8Srgb:
        return {MTLPixelFormatASTC_10x8_sRGB, 10, 8, 16};
    case PixelFormat::ASTC10x10:
        return {MTLPixelFormatASTC_10x10_LDR, 10, 10, 16};
    case PixelFormat::ASTC10x10Srgb:
        return {MTLPixelFormatASTC_10x10_sRGB, 10, 10, 16};
    case PixelFormat::ASTC12x10:
        return {MTLPixelFormatASTC_12x10_LDR, 12, 10, 16};
    case PixelFormat::ASTC12x10Srgb:
        return {MTLPixelFormatASTC_12x10_sRGB, 12, 10, 16};
    case PixelFormat::ASTC12x12:
        return {MTLPixelFormatASTC_12x12_LDR, 12, 12, 16};
    case PixelFormat::ASTC12x12Srgb:
        return {MTLPixelFormatASTC_12x12_sRGB, 12, 12, 16};
    case PixelFormat::Invalid:
        break;
    }
    throw std::invalid_argument("Unsupported Metal pixel format");
}
MTLPixelFormat Format(PixelFormat format) {
    return Describe(format).format;
}
// Bytes per block (per pixel for uncompressed formats).
size_t PixelBytes(PixelFormat format) {
    return Describe(format).bytes;
}
MTLPrimitiveType Topology(Primitive primitive) {
    switch (primitive) {
    case Primitive::Points:
        return MTLPrimitiveTypePoint;
    case Primitive::Lines:
        return MTLPrimitiveTypeLine;
    case Primitive::LineStrip:
        return MTLPrimitiveTypeLineStrip;
    case Primitive::Triangles:
        return MTLPrimitiveTypeTriangle;
    case Primitive::TriangleStrip:
        return MTLPrimitiveTypeTriangleStrip;
    }
    throw std::invalid_argument("Unsupported Metal primitive topology");
}
std::pair<MTLVertexFormat, uint32_t> VertexType(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float:
        return {MTLVertexFormatFloat, 4};
    case VertexFormat::Float2:
        return {MTLVertexFormatFloat2, 8};
    case VertexFormat::Float3:
        return {MTLVertexFormatFloat3, 12};
    case VertexFormat::Float4:
        return {MTLVertexFormatFloat4, 16};
    case VertexFormat::Half2:
        return {MTLVertexFormatHalf2, 4};
    case VertexFormat::Half4:
        return {MTLVertexFormatHalf4, 8};
    case VertexFormat::UChar4Norm:
        return {MTLVertexFormatUChar4Normalized, 4};
    case VertexFormat::Short2Norm:
        return {MTLVertexFormatShort2Normalized, 4};
    case VertexFormat::Short4Norm:
        return {MTLVertexFormatShort4Normalized, 8};
    case VertexFormat::UInt:
        return {MTLVertexFormatUInt, 4};
    case VertexFormat::UInt2:
        return {MTLVertexFormatUInt2, 8};
    case VertexFormat::UInt4:
        return {MTLVertexFormatUInt4, 16};
    case VertexFormat::Int:
        return {MTLVertexFormatInt, 4};
    case VertexFormat::Int2:
        return {MTLVertexFormatInt2, 8};
    case VertexFormat::Int4:
        return {MTLVertexFormatInt4, 16};
    }
    throw std::invalid_argument("Unsupported Metal vertex format");
}
} // namespace

namespace {
std::atomic<uint64_t> g_render_passes{};
std::atomic<uint64_t> g_command_buffers{};
std::atomic<uint64_t> g_gpu_nanoseconds{};
std::atomic<uint64_t> g_buffers_created{};
std::atomic<uint64_t> g_textures_created{};
std::atomic<uint64_t> g_blit_encoders{};
std::atomic<uint64_t> g_compute_encoders{};
std::atomic<uint64_t> g_bindings_skipped{};
} // namespace
RuntimeStats TakeRuntimeStats() {
    RuntimeStats stats;
    stats.render_passes = g_render_passes.exchange(0, std::memory_order_relaxed);
    stats.command_buffers = g_command_buffers.exchange(0, std::memory_order_relaxed);
    stats.gpu_nanoseconds = g_gpu_nanoseconds.exchange(0, std::memory_order_relaxed);
    stats.buffers_created = g_buffers_created.exchange(0, std::memory_order_relaxed);
    stats.textures_created = g_textures_created.exchange(0, std::memory_order_relaxed);
    stats.blit_encoders = g_blit_encoders.exchange(0, std::memory_order_relaxed);
    stats.compute_encoders = g_compute_encoders.exchange(0, std::memory_order_relaxed);
    stats.bindings_skipped = g_bindings_skipped.exchange(0, std::memory_order_relaxed);
    return stats;
}

struct SubmissionState {
    id<MTLCommandBuffer> object;
    std::shared_ptr<DeviceState> owner;
    std::atomic<bool> submitted{};
    std::atomic<bool> cancelled{};
    uint64_t serial{};
    mutable std::mutex mutex;
    std::vector<id> indirect_objects;
    std::unordered_set<const void*> indirect_set; // dedups indirect_objects
    // Set by the completion handler, which also drops the command buffer: resources keep
    // their last submission alive, and a retained MTLCommandBuffer would keep every
    // (per-draw) object it referenced alive with it.
    std::atomic<bool> completed{};
    bool failed{};
    std::string error;
    void Complete(id<MTLCommandBuffer> buffer) {
        std::scoped_lock lock{mutex};
        failed = buffer.status != MTLCommandBufferStatusCompleted;
        const double gpu_seconds = buffer.GPUEndTime - buffer.GPUStartTime;
        if (gpu_seconds > 0)
            g_gpu_nanoseconds.fetch_add(static_cast<uint64_t>(gpu_seconds * 1e9),
                                        std::memory_order_relaxed);
        if (failed)
            error = Message(buffer.error);
        indirect_objects.clear();
        indirect_set.clear();
        object = nil;
        completed = true;
    }
    void Wait() const {
        id<MTLCommandBuffer> local;
        {
            std::scoped_lock lock{mutex};
            if (cancelled.load())
                return;
            Require(submitted.load(), "Submit Metal commands before accessing GPU-owned memory");
            if (completed.load()) {
                if (failed)
                    throw std::runtime_error("Metal command buffer failed: " + error);
                return;
            }
            local = object;
        }
        @autoreleasepool {
            [local waitUntilCompleted];
            if (local.status != MTLCommandBufferStatusCompleted)
                throw std::runtime_error("Metal command buffer failed: " + Message(local.error));
        }
    }
    bool Done() const {
        if (completed.load())
            return true;
        std::scoped_lock lock{mutex};
        if (completed.load())
            return true;
        if (!submitted.load() || object == nil)
            return false;
        const auto status = object.status;
        return status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
    }
};
struct ResourceState {
    id object;
    id auxiliary;
    std::weak_ptr<DeviceState> owner;
    std::mutex mutex;
    // Strong fence ownership is intentional: dropping a Submission must not
    // permit CPU writes to race GPU reads of a still-live buffer.
    std::shared_ptr<SubmissionState> last_use;
    uint64_t allocation_id{};
    std::atomic<uint64_t> content_version{1};
    size_t bytes{};
    uint32_t width{}, height{}, depth_size{1}, levels{1}, layers{1};
    TextureDimension dimension{TextureDimension::D2};
    std::shared_ptr<ResourceState> backing;
    PixelFormat format{};
    TextureUsage usage{};
    Stage stage{};
    bool compute{};
    std::array<PixelFormat, 8> colors{};
    uint32_t color_count{}, sample_count{1};
    PixelFormat depth{};
    std::string source;
    struct Member {
        ResourceKind kind;
    };
    std::unordered_map<uint32_t, Member> members;
    std::unordered_map<uint32_t, std::shared_ptr<ResourceState>> argument_resources;
    std::unordered_map<uint32_t, bool> argument_writes;
    // Argument encoders are stateful and shared between clones of one layout.
    std::shared_ptr<std::mutex> encoder_mutex;
};
namespace {
struct DeviceState {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    std::mutex mutex;
    std::weak_ptr<SubmissionState> recording;
    uint64_t next_serial{1};
    std::unordered_map<std::string, std::shared_ptr<ResourceState>> functions;
    std::unordered_map<std::string, std::shared_ptr<ResourceState>> pipelines;
};
std::shared_ptr<ResourceState> MakeResource(const std::shared_ptr<DeviceState>& owner, id object) {
    Require(object != nil, "Metal resource allocation failed");
    auto resource = std::make_shared<ResourceState>();
    static std::atomic<uint64_t> next_allocation{1};
    resource->allocation_id = next_allocation.fetch_add(1, std::memory_order_relaxed);
    resource->owner = owner;
    resource->object = object;
    return resource;
}
void MarkModified(const std::shared_ptr<ResourceState>& resource) {
    resource->content_version.fetch_add(1, std::memory_order_relaxed);
    if (resource->backing)
        MarkModified(resource->backing);
}
void CheckResource(const std::shared_ptr<DeviceState>& owner,
                   const std::shared_ptr<ResourceState>& resource) {
    Require(resource != nullptr && resource->owner.lock() == owner,
            "Metal resource is missing or belongs to a different device");
}
} // namespace

struct Runtime::Impl {
    std::shared_ptr<DeviceState> device;
    // Presentation objects, created once instead of every frame.
    std::shared_ptr<ResourceState> present_pipeline;
    std::array<std::shared_ptr<ResourceState>, 2> present_samplers;
};
struct Commands::Impl {
    std::shared_ptr<SubmissionState> submission;
    id<MTLRenderCommandEncoder> render;
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    std::shared_ptr<ResourceState> pipeline;
    std::array<PixelFormat, 8> colors{};
    uint32_t color_count{}, width{}, height{};
    PixelFormat depth{};
    uint64_t pass_id{};
    // Resources already declared with useResource on the open encoder (by object, usage
    // and stage): repeating the call every draw is a large share of the encoding cost.
    std::unordered_set<uint64_t> encoder_uses;
    struct BufferSlot {
        std::shared_ptr<ResourceState> resource;
        size_t offset{};
        std::vector<std::byte> bytes;
    };
    std::array<std::array<BufferSlot, 31>, 3> buffer_slots;
    id<MTLDepthStencilState> depth_state;
    std::shared_ptr<ResourceState> visibility;
    void AttachVisibility(MTLRenderPassDescriptor* descriptor) {
        if (!visibility)
            return;
        Track(visibility);
        descriptor.visibilityResultBuffer = (id<MTLBuffer>)visibility->object;
    }
    void NewPass() {
        static std::atomic<uint64_t> next_pass{1};
        pass_id = next_pass.fetch_add(1, std::memory_order_relaxed);
        g_render_passes.fetch_add(1, std::memory_order_relaxed);
    }
    void Check() const {
        Require(!submission->submitted && !submission->cancelled,
                "Metal command buffer is no longer recording");
    }
    void End() {
        encoder_uses.clear();
        depth_state = nil;
        for (auto& stage : buffer_slots) {
            for (auto& slot : stage) {
                slot.resource.reset();
                slot.bytes.clear();
            }
        }
        if (render) {
            [render endEncoding];
            render = nil;
        }
        if (compute) {
            [compute endEncoding];
            compute = nil;
        }
        if (blit) {
            [blit endEncoding];
            blit = nil;
        }
        pipeline.reset();
    }
    void Track(const std::shared_ptr<ResourceState>& resource) {
        Check();
        CheckResource(submission->owner, resource);
        if (resource->backing)
            Track(resource->backing);
        std::scoped_lock lock{resource->mutex};
        const auto& prior = resource->last_use;
        Require(!prior || prior == submission || prior->submitted || prior->cancelled,
                "A resource is already owned by unsubmitted Metal commands");
        resource->last_use = submission;
    }
    void Blit() {
        Check();
        if (blit)
            return;
        End();
        @autoreleasepool {
            blit = [submission->object blitCommandEncoder];
        }
        g_blit_encoders.fetch_add(1, std::memory_order_relaxed);
        Require(blit != nil, "Metal blit encoder creation failed");
    }
};

size_t Buffer::Size() const {
    Require(state != nullptr, "Missing Metal buffer");
    return state->bytes;
}
uint64_t Buffer::AllocationId() const {
    Require(state != nullptr, "Missing Metal buffer");
    return state->allocation_id;
}
uint64_t Buffer::ContentVersion() const {
    Require(state != nullptr, "Missing Metal buffer");
    return state->content_version.load(std::memory_order_relaxed);
}
void Buffer::MarkWritten() const {
    Require(state != nullptr, "Missing Metal buffer");
    MarkModified(state);
}
void Buffer::Write(size_t offset, std::span<const std::byte> bytes) const {
    CheckRange(offset, bytes.size(), Size());
    std::scoped_lock lock{state->mutex};
    if (state->last_use)
        state->last_use->Wait();
    MarkModified(state);
    auto buffer = (id<MTLBuffer>)state->object;
    if (!bytes.empty())
        std::memcpy(static_cast<std::byte*>(buffer.contents) + offset, bytes.data(), bytes.size());
}
void Buffer::Read(size_t offset, std::span<std::byte> bytes) const {
    CheckRange(offset, bytes.size(), Size());
    std::scoped_lock lock{state->mutex};
    if (state->last_use)
        state->last_use->Wait();
    auto buffer = (id<MTLBuffer>)state->object;
    if (!bytes.empty())
        std::memcpy(bytes.data(), static_cast<const std::byte*>(buffer.contents) + offset,
                    bytes.size());
}
std::span<std::byte> Buffer::MappedBytes() const {
    Require(state != nullptr, "Missing Metal staging buffer");
    std::scoped_lock lock{state->mutex};
    if (state->last_use)
        state->last_use->Wait();
    MarkModified(state);
    return {static_cast<std::byte*>(((id<MTLBuffer>)state->object).contents), state->bytes};
}
std::byte* Buffer::UnsafeContents() const {
    Require(state != nullptr, "Missing Metal buffer");
    MarkModified(state);
    return static_cast<std::byte*>(((id<MTLBuffer>)state->object).contents);
}
uint32_t Texture::Width() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->width;
}
uint32_t Texture::Height() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->height;
}
PixelFormat Texture::Format() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->format;
}
void Submission::Wait() const {
    Require(state != nullptr, "Missing Metal submission");
    state->Wait();
}
bool Submission::IsComplete() const {
    Require(state != nullptr, "Missing Metal submission");
    if (state->cancelled.load())
        return true;
    if (!state->Done())
        return false;
    state->Wait(); // reports GPU faults
    return true;
}
uint64_t Submission::Serial() const {
    Require(state != nullptr, "Missing Metal submission");
    return state->serial;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {
    @autoreleasepool {
        auto device = std::make_shared<DeviceState>();
        device->device = MTLCreateSystemDefaultDevice();
        Require(device->device != nil, "No native Metal device is available");
        Require([device->device supportsFamily:MTLGPUFamilyMetal3],
                "The native renderer core requires Metal 3 support");
        device->queue = [device->device newCommandQueue];
        Require(device->queue != nil, "Metal command queue creation failed");
        device->queue.label = @"Citrosis native Metal queue";
        impl->device = std::move(device);
    }
}
Runtime::~Runtime() = default;
std::string Runtime::DeviceName() const {
    return impl->device->device.name.UTF8String;
}
Buffer Runtime::CreateBuffer(size_t size) {
    @autoreleasepool {
        g_buffers_created.fetch_add(1, std::memory_order_relaxed);
        Require(size > 0 && size <= impl->device->device.maxBufferLength,
                "Invalid Metal buffer size");
        Buffer result;
        result.state = MakeResource(
            impl->device, [impl->device->device newBufferWithLength:size
                                                            options:MTLResourceStorageModeShared]);
        result.state->bytes = size;
        return result;
    }
}
namespace {
MTLTextureType TextureType(TextureDimension dimension) {
    switch (dimension) {
    case TextureDimension::D1:
        return MTLTextureType1D;
    case TextureDimension::D2:
        return MTLTextureType2D;
    case TextureDimension::D3:
        return MTLTextureType3D;
    case TextureDimension::D1Array:
        return MTLTextureType1DArray;
    case TextureDimension::D2Array:
        return MTLTextureType2DArray;
    case TextureDimension::Cube:
        return MTLTextureTypeCube;
    case TextureDimension::CubeArray:
        return MTLTextureTypeCubeArray;
    case TextureDimension::Buffer:
        return MTLTextureTypeTextureBuffer;
    }
    throw std::invalid_argument("Invalid Metal texture dimension");
}
void TextureMetadata(ResourceState& state, const TextureDesc& desc) {
    state.width = desc.width;
    state.height = desc.height;
    state.depth_size = desc.depth;
    state.levels = desc.levels;
    state.layers = desc.layers;
    state.dimension = desc.dimension;
    state.format = desc.format;
    state.usage = desc.usage;
}
MTLTextureUsage TextureFlags(TextureUsage usage) {
    const auto flags = static_cast<uint32_t>(usage);
    Require(flags != 0 && (flags & ~7u) == 0, "Invalid Metal texture usage flags");
    MTLTextureUsage result = MTLTextureUsagePixelFormatView;
    if (flags & 1)
        result |= MTLTextureUsageShaderRead;
    if (flags & 2)
        result |= MTLTextureUsageShaderWrite;
    if (flags & 4)
        result |= MTLTextureUsageRenderTarget;
    return result;
}
void ValidateRegion(const Texture& texture, const TextureRegion& region) {
    Require(region.level < texture.Levels() && region.slice < texture.Layers(),
            "Metal texture subresource exceeds allocation");
    const auto axis = [](uint32_t first, uint32_t count, uint32_t maximum) {
        return count > 0 && first < maximum && count <= maximum - first;
    };
    Require(axis(region.x, region.width, std::max(1u, texture.Width() >> region.level)) &&
                axis(region.y, region.height, std::max(1u, texture.Height() >> region.level)) &&
                axis(region.z, region.depth, std::max(1u, texture.Depth() >> region.level)),
            "Metal texture region exceeds subresource");
    Require(texture.Dimension() == TextureDimension::D3 || (region.z == 0 && region.depth == 1),
            "Metal non-3D texture region must have depth 1");
}
void ValidateTransfer(const Texture& texture, const TextureRegion& region, const Buffer& buffer,
                      size_t offset, size_t pitch, size_t image_pitch,
                      TransferPlane plane = TransferPlane::All) {
    ValidateRegion(texture, region);
    const auto info = Describe(texture.Format());
    const bool combined = IsDepthFormat(texture.Format()) && HasStencil(texture.Format());
    size_t bytes = info.bytes;
    switch (plane) {
    case TransferPlane::All:
        Require(!combined, "Combined depth/stencil transfers must select a plane");
        break;
    case TransferPlane::Depth:
        Require(IsDepthFormat(texture.Format()), "Depth plane transfer of a non-depth texture");
        bytes = texture.Format() == PixelFormat::Depth16 ? 2 : 4;
        break;
    case TransferPlane::Stencil:
        Require(HasStencil(texture.Format()), "Stencil plane transfer of a non-stencil texture");
        bytes = 1;
        break;
    }
    Require(region.x % info.block_width == 0 && region.y % info.block_height == 0,
            "Metal compressed texture transfer origin is not block aligned");
    // Compressed extents must be whole blocks unless they reach the subresource edge.
    const uint32_t level_width = std::max(1u, texture.Width() >> region.level);
    const uint32_t level_height = std::max(1u, texture.Height() >> region.level);
    Require((region.width % info.block_width == 0 || region.x + region.width == level_width) &&
                (region.height % info.block_height == 0 ||
                 region.y + region.height == level_height),
            "Metal compressed texture transfer extent is not block aligned");
    const size_t blocks_x = (region.width + info.block_width - 1) / info.block_width;
    const size_t blocks_y = (region.height + info.block_height - 1) / info.block_height;
    const size_t width = blocks_x * bytes;
    // Metal needs rows, images and offsets in whole pixels/blocks; 4-byte multiples keep
    // every format (and the depth/stencil plane copies) on the safe side.
    Require(pitch >= width && pitch % bytes == 0 && pitch % 4 == 0 && offset % bytes == 0 &&
                offset % 4 == 0,
            "Metal texture transfer needs aligned pitch and offset");
    Require(pitch <= std::numeric_limits<size_t>::max() / blocks_y, "Metal row pitch overflows");
    Require(image_pitch >= pitch * blocks_y && image_pitch % 4 == 0 &&
                image_pitch <= std::numeric_limits<size_t>::max() / region.depth,
            "Metal image pitch overflows");
    CheckRange(offset, image_pitch * region.depth, buffer.Size());
}
MTLBlitOption PlaneOption(TransferPlane plane) {
    switch (plane) {
    case TransferPlane::Depth:
        return MTLBlitOptionDepthFromDepthStencil;
    case TransferPlane::Stencil:
        return MTLBlitOptionStencilFromDepthStencil;
    default:
        return MTLBlitOptionNone;
    }
}
} // namespace
uint32_t Texture::Depth() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->depth_size;
}
uint32_t Texture::Levels() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->levels;
}
uint32_t Texture::Layers() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->layers;
}
TextureDimension Texture::Dimension() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->dimension;
}
TextureUsage Texture::Usage() const {
    Require(state != nullptr, "Missing Metal texture");
    return state->usage;
}
uint32_t Texture::SampleCount() const {
    Require(state != nullptr, "Missing Metal texture");
    return std::max<uint32_t>(1, static_cast<uint32_t>([(id<MTLTexture>)state->object sampleCount]));
}
uint32_t BlockWidth(PixelFormat format) {
    return Describe(format).block_width;
}
uint32_t BlockHeight(PixelFormat format) {
    return Describe(format).block_height;
}
uint32_t BytesPerBlock(PixelFormat format) {
    return Describe(format).bytes;
}
bool IsDepthFormat(PixelFormat format) {
    return format == PixelFormat::Depth32Float || format == PixelFormat::Depth16 ||
           format == PixelFormat::Depth32FloatStencil8;
}
bool HasStencil(PixelFormat format) {
    return format == PixelFormat::Depth32FloatStencil8 || format == PixelFormat::Stencil8;
}
PixelFormat LinearFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8Srgb:
        return PixelFormat::RGBA8;
    case PixelFormat::BGRA8Srgb:
        return PixelFormat::BGRA8;
    case PixelFormat::R8Srgb:
        return PixelFormat::R8;
    case PixelFormat::RG8Srgb:
        return PixelFormat::RG8;
    case PixelFormat::BC1Srgb:
        return PixelFormat::BC1;
    case PixelFormat::BC2Srgb:
        return PixelFormat::BC2;
    case PixelFormat::BC3Srgb:
        return PixelFormat::BC3;
    case PixelFormat::BC7Srgb:
        return PixelFormat::BC7;
    case PixelFormat::ASTC4x4Srgb:
        return PixelFormat::ASTC4x4;
    case PixelFormat::ASTC5x4Srgb:
        return PixelFormat::ASTC5x4;
    case PixelFormat::ASTC5x5Srgb:
        return PixelFormat::ASTC5x5;
    case PixelFormat::ASTC6x5Srgb:
        return PixelFormat::ASTC6x5;
    case PixelFormat::ASTC6x6Srgb:
        return PixelFormat::ASTC6x6;
    case PixelFormat::ASTC8x5Srgb:
        return PixelFormat::ASTC8x5;
    case PixelFormat::ASTC8x6Srgb:
        return PixelFormat::ASTC8x6;
    case PixelFormat::ASTC8x8Srgb:
        return PixelFormat::ASTC8x8;
    case PixelFormat::ASTC10x5Srgb:
        return PixelFormat::ASTC10x5;
    case PixelFormat::ASTC10x6Srgb:
        return PixelFormat::ASTC10x6;
    case PixelFormat::ASTC10x8Srgb:
        return PixelFormat::ASTC10x8;
    case PixelFormat::ASTC10x10Srgb:
        return PixelFormat::ASTC10x10;
    case PixelFormat::ASTC12x10Srgb:
        return PixelFormat::ASTC12x10;
    case PixelFormat::ASTC12x12Srgb:
        return PixelFormat::ASTC12x12;
    default:
        return format;
    }
}
bool CanViewAs(PixelFormat from, PixelFormat to) {
    if (from == to)
        return true;
    if (IsDepthFormat(from) || HasStencil(from) || IsDepthFormat(to) || HasStencil(to))
        return false;
    if (LinearFormat(from) == LinearFormat(to))
        return true;
    const auto a = Describe(from);
    const auto b = Describe(to);
    return a.block_width == 1 && a.block_height == 1 && b.block_width == 1 &&
           b.block_height == 1 && a.bytes == b.bytes;
}
Texture Runtime::CreateTexture(uint32_t width, uint32_t height, PixelFormat format,
                               TextureUsage usage) {
    TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = usage;
    return CreateTexture(desc);
}
Texture Runtime::CreateTexture(const TextureDesc& desc) {
    @autoreleasepool {
        g_textures_created.fetch_add(1, std::memory_order_relaxed);
        Require(desc.width > 0 && desc.height > 0 && desc.depth > 0 && desc.width <= 16384 &&
                    desc.height <= 16384 && desc.depth <= 2048,
                "Invalid Metal texture dimensions");
        Require(desc.levels > 0 &&
                    desc.levels <= static_cast<uint32_t>(std::bit_width(
                                       std::max({desc.width, desc.height, desc.depth}))) &&
                    desc.layers > 0 && desc.layers <= 2048,
                "Invalid Metal mip or layer count");
        Require(desc.dimension != TextureDimension::Buffer,
                "Buffer textures require a backing buffer");
        const bool cube = desc.dimension == TextureDimension::Cube ||
                          desc.dimension == TextureDimension::CubeArray;
        if (cube)
            Require(desc.width == desc.height && desc.layers % 6 == 0 &&
                        (desc.dimension != TextureDimension::Cube || desc.layers == 6),
                    "Invalid Metal cube texture");
        if (desc.dimension == TextureDimension::D1 || desc.dimension == TextureDimension::D1Array)
            Require(desc.height == 1 && desc.depth == 1 && desc.levels == 1,
                    "Invalid Metal 1D texture");
        if (desc.dimension == TextureDimension::D3)
            Require(desc.layers == 1 && desc.width <= 2048 && desc.height <= 2048,
                    "Invalid Metal 3D texture dimensions or array slices");
        else
            Require(desc.depth == 1, "Non-3D Metal texture has depth slices");
        if (desc.dimension == TextureDimension::D1 || desc.dimension == TextureDimension::D2)
            Require(desc.layers == 1, "Non-array Metal texture has array slices");
        auto descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = TextureType(desc.dimension);
        descriptor.pixelFormat = Format(desc.format);
        descriptor.width = desc.width;
        descriptor.height = desc.height;
        descriptor.depth = desc.depth;
        descriptor.mipmapLevelCount = desc.levels;
        descriptor.arrayLength = cube ? desc.layers / 6 : desc.layers;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = TextureFlags(desc.usage);
        Texture result;
        result.state =
            MakeResource(impl->device, [impl->device->device newTextureWithDescriptor:descriptor]);
        TextureMetadata(*result.state, desc);
        return result;
    }
}
Texture Runtime::CreateTextureView(const Texture& source, const TextureDesc& view, uint32_t level,
                                   uint32_t slice, std::array<uint8_t, 4> swizzle) {
    @autoreleasepool {
        CheckResource(impl->device, source.state);
        Require(level < source.Levels() && view.levels > 0 &&
                    view.levels <= source.Levels() - level && slice < source.Layers() &&
                    view.layers > 0 && view.layers <= source.Layers() - slice,
                "Metal texture view range exceeds allocation");
        Require(view.width == std::max(1u, source.Width() >> level) &&
                    view.height == std::max(1u, source.Height() >> level) &&
                    view.depth == std::max(1u, source.Depth() >> level),
                "Metal texture view dimensions mismatch");
        Require((static_cast<uint32_t>(view.usage) & ~static_cast<uint32_t>(source.state->usage)) ==
                    0,
                "Metal texture view requests missing usage");
        Require(source.state->dimension != TextureDimension::Buffer &&
                    view.dimension != TextureDimension::Buffer,
                "Metal buffer-texture views use the buffer texture API");
        if (source.state->dimension == TextureDimension::D3 ||
            view.dimension == TextureDimension::D3)
            Require(source.state->dimension == view.dimension && slice == 0 && view.layers == 1,
                    "Metal 3D slice view emulation is not implemented");
        const auto channel = [](uint8_t c) {
            switch (c) {
            case 0:
                return MTLTextureSwizzleZero;
            case 1:
                return MTLTextureSwizzleOne;
            case 2:
                return MTLTextureSwizzleRed;
            case 3:
                return MTLTextureSwizzleGreen;
            case 4:
                return MTLTextureSwizzleBlue;
            case 5:
                return MTLTextureSwizzleAlpha;
            }
            throw std::invalid_argument("Invalid Metal texture swizzle");
        };
        // Metal aborts (under the API validation layer) on incompatible reinterpretation;
        // reject it with an exception the caller can handle instead.
        Require(CanViewAs(source.state->format, view.format),
                "Incompatible Metal texture view format reinterpretation");
        const auto channels = MTLTextureSwizzleChannelsMake(
            channel(swizzle[0]), channel(swizzle[1]), channel(swizzle[2]), channel(swizzle[3]));
        auto object = [(id<MTLTexture>)source.state->object
            newTextureViewWithPixelFormat:Format(view.format)
                              textureType:TextureType(view.dimension)
                                   levels:NSMakeRange(level, view.levels)
                                   slices:NSMakeRange(slice, view.layers)
                                  swizzle:channels];
        Texture result;
        result.state = MakeResource(impl->device, object);
        TextureMetadata(*result.state, view);
        result.state->backing = source.state;
        return result;
    }
}
Texture Runtime::CreateBufferTexture(const Buffer& buffer, size_t offset, uint32_t elements,
                                     PixelFormat format, TextureUsage usage) {
    @autoreleasepool {
        g_textures_created.fetch_add(1, std::memory_order_relaxed);
        CheckResource(impl->device, buffer.state);
        const auto alignment =
            [impl->device->device minimumTextureBufferAlignmentForPixelFormat:Format(format)];
        Require(elements > 0 && offset % alignment == 0,
                "Metal buffer texture has invalid extent or alignment");
        CheckRange(offset, static_cast<size_t>(elements) * PixelBytes(format), buffer.Size());
        auto descriptor = [MTLTextureDescriptor new];
        descriptor.textureType = MTLTextureTypeTextureBuffer;
        descriptor.pixelFormat = Format(format);
        descriptor.width = elements;
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = TextureFlags(usage);
        auto object = [(id<MTLBuffer>)buffer.state->object
            newTextureWithDescriptor:descriptor
                              offset:offset
                         bytesPerRow:static_cast<size_t>(elements) * PixelBytes(format)];
        Texture result;
        result.state = MakeResource(impl->device, object);
        TextureDesc desc;
        desc.width = elements;
        desc.dimension = TextureDimension::Buffer;
        desc.format = format;
        desc.usage = usage;
        TextureMetadata(*result.state, desc);
        result.state->backing = buffer.state;
        return result;
    }
}
Function Runtime::CompileMSL(std::string_view source, std::string_view entry, Stage stage) {
    @autoreleasepool {
        const std::string key = std::to_string(static_cast<int>(stage)) + ":" + std::string(entry) +
                                ":" + std::string(source);
        std::unique_lock lock{impl->device->mutex};
        if (auto found = impl->device->functions.find(key);
            found != impl->device->functions.end()) {
            Function result;
            result.state = found->second;
            return result;
        }
        lock.unlock(); // build without blocking other threads; insert under the lock
        auto options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        if (@available(macOS 11.0, *))
            options.preserveInvariance = YES;
        if (@available(macOS 15.0, *)) {
            // Fast-math reassociation and contraction, but NaN/Inf semantics preserved
            // (guest shaders rely on IEEE min/max/NaN behaviour; full fast math breaks them).
            options.mathMode = MTLMathModeRelaxed;
        }
        NSError* error = nil;
        auto text = [[NSString alloc] initWithBytes:source.data()
                                             length:source.size()
                                           encoding:NSUTF8StringEncoding];
        auto library = [impl->device->device newLibraryWithSource:text
                                                          options:options
                                                            error:&error];
        if (!library)
            throw std::runtime_error("Metal shader compilation failed: " + Message(error));
        auto name = [[NSString alloc] initWithBytes:entry.data()
                                             length:entry.size()
                                           encoding:NSUTF8StringEncoding];
        auto function = [library newFunctionWithName:name];
        Require(function != nil, "Metal shader entry point was not found");
        const auto expected = stage == Stage::Vertex     ? MTLFunctionTypeVertex
                              : stage == Stage::Fragment ? MTLFunctionTypeFragment
                                                         : MTLFunctionTypeKernel;
        Require(function.functionType == expected, "Metal shader entry point has the wrong stage");
        Function result;
        result.state = MakeResource(impl->device, function);
        result.state->auxiliary = library;
        result.state->stage = stage;
        result.state->source = source;
        lock.lock();
        impl->device->functions.emplace(key, result.state);
        return result;
    }
}
namespace {
MTLBlendFactor Factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return MTLBlendFactorZero;
    case BlendFactor::One:
        return MTLBlendFactorOne;
    case BlendFactor::SourceColor:
        return MTLBlendFactorSourceColor;
    case BlendFactor::OneMinusSourceColor:
        return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::SourceAlpha:
        return MTLBlendFactorSourceAlpha;
    case BlendFactor::OneMinusSourceAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DestinationColor:
        return MTLBlendFactorDestinationColor;
    case BlendFactor::OneMinusDestinationColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::DestinationAlpha:
        return MTLBlendFactorDestinationAlpha;
    case BlendFactor::OneMinusDestinationAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::SourceAlphaSaturated:
        return MTLBlendFactorSourceAlphaSaturated;
    case BlendFactor::BlendColor:
        return MTLBlendFactorBlendColor;
    case BlendFactor::OneMinusBlendColor:
        return MTLBlendFactorOneMinusBlendColor;
    case BlendFactor::BlendAlpha:
        return MTLBlendFactorBlendAlpha;
    case BlendFactor::OneMinusBlendAlpha:
        return MTLBlendFactorOneMinusBlendAlpha;
    case BlendFactor::Source1Color:
        return MTLBlendFactorSource1Color;
    case BlendFactor::OneMinusSource1Color:
        return MTLBlendFactorOneMinusSource1Color;
    case BlendFactor::Source1Alpha:
        return MTLBlendFactorSource1Alpha;
    case BlendFactor::OneMinusSource1Alpha:
        return MTLBlendFactorOneMinusSource1Alpha;
    }
    throw std::invalid_argument("Invalid Metal blend factor");
}
MTLBlendOperation Operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Add:
        return MTLBlendOperationAdd;
    case BlendOperation::Subtract:
        return MTLBlendOperationSubtract;
    case BlendOperation::ReverseSubtract:
        return MTLBlendOperationReverseSubtract;
    case BlendOperation::Min:
        return MTLBlendOperationMin;
    case BlendOperation::Max:
        return MTLBlendOperationMax;
    }
    throw std::invalid_argument("Invalid Metal blend operation");
}
MTLStencilOperation Stencil(StencilOperation operation) {
    switch (operation) {
    case StencilOperation::Keep:
        return MTLStencilOperationKeep;
    case StencilOperation::Zero:
        return MTLStencilOperationZero;
    case StencilOperation::Replace:
        return MTLStencilOperationReplace;
    case StencilOperation::IncrementClamp:
        return MTLStencilOperationIncrementClamp;
    case StencilOperation::DecrementClamp:
        return MTLStencilOperationDecrementClamp;
    case StencilOperation::Invert:
        return MTLStencilOperationInvert;
    case StencilOperation::IncrementWrap:
        return MTLStencilOperationIncrementWrap;
    case StencilOperation::DecrementWrap:
        return MTLStencilOperationDecrementWrap;
    }
    throw std::invalid_argument("Invalid Metal stencil operation");
}
MTLCompareFunction CompareOp(CompareFunction function) {
    switch (function) {
    case CompareFunction::Never:
        return MTLCompareFunctionNever;
    case CompareFunction::Less:
        return MTLCompareFunctionLess;
    case CompareFunction::Equal:
        return MTLCompareFunctionEqual;
    case CompareFunction::LessEqual:
        return MTLCompareFunctionLessEqual;
    case CompareFunction::Greater:
        return MTLCompareFunctionGreater;
    case CompareFunction::NotEqual:
        return MTLCompareFunctionNotEqual;
    case CompareFunction::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case CompareFunction::Always:
        return MTLCompareFunctionAlways;
    }
    throw std::invalid_argument("Invalid Metal compare function");
}
} // namespace
DepthStencilState Runtime::CreateDepthStencilState(const DepthStencilDesc& desc) {
    @autoreleasepool {
        const auto face_key = [](const StencilFaceDesc& face) {
            std::ostringstream key;
            key << static_cast<int>(face.compare) << ',' << static_cast<int>(face.fail) << ','
                << static_cast<int>(face.depth_fail) << ',' << static_cast<int>(face.pass) << ','
                << face.read_mask << ',' << face.write_mask;
            return key.str();
        };
        std::ostringstream key;
        key << "ds:" << static_cast<int>(desc.depth_compare) << ':' << desc.depth_write << ':'
            << desc.stencil_enable << ':' << face_key(desc.front) << ':' << face_key(desc.back);
        std::scoped_lock lock{impl->device->mutex};
        if (auto found = impl->device->pipelines.find(key.str());
            found != impl->device->pipelines.end()) {
            DepthStencilState result;
            result.state = found->second;
            return result;
        }
        auto descriptor = [MTLDepthStencilDescriptor new];
        descriptor.depthCompareFunction = CompareOp(desc.depth_compare);
        descriptor.depthWriteEnabled = desc.depth_write;
        if (desc.stencil_enable) {
            const auto face = [](const StencilFaceDesc& in) {
                auto out = [MTLStencilDescriptor new];
                out.stencilCompareFunction = CompareOp(in.compare);
                out.stencilFailureOperation = Stencil(in.fail);
                out.depthFailureOperation = Stencil(in.depth_fail);
                out.depthStencilPassOperation = Stencil(in.pass);
                out.readMask = in.read_mask & 0xFF;
                out.writeMask = in.write_mask & 0xFF;
                return out;
            };
            descriptor.frontFaceStencil = face(desc.front);
            descriptor.backFaceStencil = face(desc.back);
        }
        DepthStencilState result;
        result.state = MakeResource(impl->device,
                                    [impl->device->device newDepthStencilStateWithDescriptor:descriptor]);
        impl->device->pipelines.emplace(key.str(), result.state);
        return result;
    }
}
size_t Runtime::TextureBufferAlignment(PixelFormat format) const {
    return [impl->device->device minimumTextureBufferAlignmentForPixelFormat:Format(format)];
}
Pipeline Runtime::CreateRenderPipeline(const RenderPipelineDesc& desc) {
    @autoreleasepool {
        CheckResource(impl->device, desc.vertex.state);
        if (desc.fragment.state)
            CheckResource(impl->device, desc.fragment.state);
        Require(desc.vertex.state->stage == Stage::Vertex &&
                    (!desc.fragment.state || desc.fragment.state->stage == Stage::Fragment),
                "Invalid Metal render shader stages");
        Require(desc.color_count <= 8 && desc.sample_count == 1,
                "Unsupported Metal attachment count or sample count");
        std::ostringstream key;
        key << desc.vertex.state.get() << ':' << desc.fragment.state.get() << ':'
            << desc.color_count << ':' << static_cast<int>(desc.depth) << ':' << desc.sample_count
            << ':' << desc.premultiplied_blend << ':' << desc.use_blend_states << ':'
            << desc.alpha_to_coverage << ':' << desc.rasterization_enabled << ':'
            << static_cast<int>(desc.input_topology);
        if (desc.use_blend_states) {
            for (const auto& blend : desc.blend)
                key << ":bl:" << blend.enable << ',' << static_cast<int>(blend.src_rgb) << ','
                    << static_cast<int>(blend.dst_rgb) << ',' << static_cast<int>(blend.src_alpha)
                    << ',' << static_cast<int>(blend.dst_alpha) << ','
                    << static_cast<int>(blend.op_rgb) << ',' << static_cast<int>(blend.op_alpha)
                    << ',' << static_cast<int>(blend.write_mask);
        }
        for (const auto format : desc.colors)
            key << ':' << static_cast<int>(format);
        for (const auto& attr : desc.attributes)
            key << ":a:" << attr.location << ':' << attr.buffer << ':' << attr.offset << ':'
                << static_cast<int>(attr.format) << ':' << attr.raw_format << ':' << attr.raw_size;
        for (const auto& buffer : desc.vertex_buffers)
            key << ":b:" << buffer.buffer << ':' << buffer.stride << ':' << buffer.per_instance
                << ':' << buffer.step_rate;
        std::unique_lock lock{impl->device->mutex};
        if (auto found = impl->device->pipelines.find(key.str());
            found != impl->device->pipelines.end()) {
            Pipeline result;
            result.state = found->second;
            return result;
        }
        lock.unlock(); // build without blocking other threads; insert under the lock
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = (id<MTLFunction>)desc.vertex.state->object;
        descriptor.fragmentFunction =
            desc.fragment.state ? (id<MTLFunction>)desc.fragment.state->object : nil;
        descriptor.rasterSampleCount = desc.sample_count;
        descriptor.alphaToCoverageEnabled = desc.alpha_to_coverage;
        descriptor.rasterizationEnabled = desc.rasterization_enabled;
        switch (desc.input_topology) {
        case InputTopology::Unspecified:
            descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
            break;
        case InputTopology::Point:
            descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
            break;
        case InputTopology::Line:
            descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine;
            break;
        case InputTopology::Triangle:
            descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
            break;
        }
        if (!desc.attributes.empty()) {
            auto vertices = [MTLVertexDescriptor vertexDescriptor];
            std::array<uint32_t, 26> strides{};
            std::array<bool, 31> locations{};
            std::array<bool, 26> layouts{};
            for (const auto& buffer : desc.vertex_buffers) {
                Require(buffer.buffer < strides.size() && buffer.stride <= 2048 &&
                            buffer.stride % 4 == 0 && (buffer.stride == 0 || buffer.step_rate > 0),
                        "Invalid Metal vertex-buffer layout (strides must be multiples of 4)");
                Require(!layouts[buffer.buffer], "Duplicate Metal vertex-buffer layout");
                layouts[buffer.buffer] = true;
                strides[buffer.buffer] = buffer.stride;
                auto layout = vertices.layouts[buffer.buffer];
                if (buffer.stride == 0) {
                    // Every vertex reads the same element.
                    layout.stride = 0;
                    layout.stepFunction = MTLVertexStepFunctionConstant;
                    layout.stepRate = 0;
                } else {
                    layout.stride = buffer.stride;
                    layout.stepFunction = buffer.per_instance ? MTLVertexStepFunctionPerInstance
                                                              : MTLVertexStepFunctionPerVertex;
                    layout.stepRate = buffer.step_rate;
                }
            }
            for (const auto& attr : desc.attributes) {
                Require(attr.location < locations.size() && !locations[attr.location] &&
                            attr.buffer < strides.size() && layouts[attr.buffer],
                        "Invalid or duplicate Metal vertex attribute");
                const auto [format, bytes] =
                    attr.raw_format != 0
                        ? std::pair{static_cast<MTLVertexFormat>(attr.raw_format), attr.raw_size}
                        : VertexType(attr.format);
                Require(strides[attr.buffer] == 0 ||
                            (attr.offset <= strides[attr.buffer] &&
                             bytes <= strides[attr.buffer] - attr.offset),
                        "Metal vertex attribute exceeds its buffer stride");
                locations[attr.location] = true;
                vertices.attributes[attr.location].format = format;
                vertices.attributes[attr.location].offset = attr.offset;
                vertices.attributes[attr.location].bufferIndex = attr.buffer;
            }
            descriptor.vertexDescriptor = vertices;
        }
        for (uint32_t i = 0; i < desc.color_count; ++i) {
            if (desc.colors[i] == PixelFormat::Invalid)
                continue; // unbound color index
            Require(!IsDepthFormat(desc.colors[i]) && !HasStencil(desc.colors[i]),
                    "Depth format cannot be a Metal color attachment");
            auto attachment = descriptor.colorAttachments[i];
            attachment.pixelFormat = Format(desc.colors[i]);
            if (desc.use_blend_states) {
                const auto& blend = desc.blend[i];
                attachment.blendingEnabled = blend.enable;
                attachment.sourceRGBBlendFactor = Factor(blend.src_rgb);
                attachment.destinationRGBBlendFactor = Factor(blend.dst_rgb);
                attachment.sourceAlphaBlendFactor = Factor(blend.src_alpha);
                attachment.destinationAlphaBlendFactor = Factor(blend.dst_alpha);
                attachment.rgbBlendOperation = Operation(blend.op_rgb);
                attachment.alphaBlendOperation = Operation(blend.op_alpha);
                MTLColorWriteMask mask = MTLColorWriteMaskNone;
                if (blend.write_mask & 1)
                    mask |= MTLColorWriteMaskRed;
                if (blend.write_mask & 2)
                    mask |= MTLColorWriteMaskGreen;
                if (blend.write_mask & 4)
                    mask |= MTLColorWriteMaskBlue;
                if (blend.write_mask & 8)
                    mask |= MTLColorWriteMaskAlpha;
                attachment.writeMask = mask;
            } else {
                attachment.blendingEnabled = desc.premultiplied_blend;
                attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
                attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
                attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            }
        }
        if (IsDepthFormat(desc.depth))
            descriptor.depthAttachmentPixelFormat = Format(desc.depth);
        if (HasStencil(desc.depth))
            descriptor.stencilAttachmentPixelFormat = Format(desc.depth);
        NSError* error = nil;
        auto pipeline = [impl->device->device newRenderPipelineStateWithDescriptor:descriptor
                                                                             error:&error];
        if (!pipeline)
            throw std::runtime_error("Metal render pipeline creation failed: " + Message(error));
        Pipeline result;
        result.state = MakeResource(impl->device, pipeline);
        result.state->colors = desc.colors;
        result.state->color_count = desc.color_count;
        result.state->depth = desc.depth;
        result.state->sample_count = desc.sample_count;
        auto depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = IsDepthFormat(desc.depth) ? MTLCompareFunctionLessEqual
                                                               : MTLCompareFunctionAlways;
        depth.depthWriteEnabled = IsDepthFormat(desc.depth);
        result.state->auxiliary = [impl->device->device newDepthStencilStateWithDescriptor:depth];
        lock.lock();
        impl->device->pipelines.emplace(key.str(), result.state);
        return result;
    }
}
Pipeline Runtime::CreateComputePipeline(const Function& function) {
    @autoreleasepool {
        CheckResource(impl->device, function.state);
        Require(function.state->stage == Stage::Compute, "Invalid Metal compute shader stage");
        const auto key =
            "compute:" + std::to_string(reinterpret_cast<uintptr_t>(function.state.get()));
        std::unique_lock lock{impl->device->mutex};
        if (auto found = impl->device->pipelines.find(key);
            found != impl->device->pipelines.end()) {
            Pipeline result;
            result.state = found->second;
            return result;
        }
        lock.unlock(); // build without blocking other threads; insert under the lock
        NSError* error = nil;
        auto pipeline = [impl->device->device
            newComputePipelineStateWithFunction:(id<MTLFunction>)function.state->object
                                          error:&error];
        if (!pipeline)
            throw std::runtime_error("Metal compute pipeline creation failed: " + Message(error));
        Pipeline result;
        result.state = MakeResource(impl->device, pipeline);
        result.state->compute = true;
        lock.lock();
        impl->device->pipelines.emplace(key, result.state);
        return result;
    }
}
Sampler Runtime::CreateSampler(bool linear, bool repeat) {
    @autoreleasepool {
        auto descriptor = [MTLSamplerDescriptor new];
        descriptor.supportArgumentBuffers = YES;
        descriptor.minFilter = descriptor.magFilter =
            linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode =
            repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        Sampler result;
        result.state = MakeResource(
            impl->device, [impl->device->device newSamplerStateWithDescriptor:descriptor]);
        return result;
    }
}
namespace {
MTLSamplerMinMagFilter MinMag(SamplerFilter filter) {
    return filter == SamplerFilter::Linear ? MTLSamplerMinMagFilterLinear
                                           : MTLSamplerMinMagFilterNearest;
}
MTLSamplerMipFilter Mip(SamplerMipFilter filter) {
    switch (filter) {
    case SamplerMipFilter::None:
        return MTLSamplerMipFilterNotMipmapped;
    case SamplerMipFilter::Nearest:
        return MTLSamplerMipFilterNearest;
    case SamplerMipFilter::Linear:
        return MTLSamplerMipFilterLinear;
    }
    throw std::invalid_argument("Invalid Metal mip filter");
}
MTLSamplerAddressMode Address(SamplerAddress mode) {
    switch (mode) {
    case SamplerAddress::Repeat:
        return MTLSamplerAddressModeRepeat;
    case SamplerAddress::MirrorRepeat:
        return MTLSamplerAddressModeMirrorRepeat;
    case SamplerAddress::ClampToEdge:
        return MTLSamplerAddressModeClampToEdge;
    case SamplerAddress::ClampToBorder:
        return MTLSamplerAddressModeClampToBorderColor;
    case SamplerAddress::MirrorClampToEdge:
        return MTLSamplerAddressModeMirrorClampToEdge;
    }
    throw std::invalid_argument("Invalid Metal sampler address mode");
}
MTLCompareFunction Compare(CompareFunction function) {
    switch (function) {
    case CompareFunction::Never:
        return MTLCompareFunctionNever;
    case CompareFunction::Less:
        return MTLCompareFunctionLess;
    case CompareFunction::Equal:
        return MTLCompareFunctionEqual;
    case CompareFunction::LessEqual:
        return MTLCompareFunctionLessEqual;
    case CompareFunction::Greater:
        return MTLCompareFunctionGreater;
    case CompareFunction::NotEqual:
        return MTLCompareFunctionNotEqual;
    case CompareFunction::GreaterEqual:
        return MTLCompareFunctionGreaterEqual;
    case CompareFunction::Always:
        return MTLCompareFunctionAlways;
    }
    throw std::invalid_argument("Invalid Metal compare function");
}
MTLSamplerBorderColor Border(BorderColor color) {
    switch (color) {
    case BorderColor::TransparentBlack:
        return MTLSamplerBorderColorTransparentBlack;
    case BorderColor::OpaqueBlack:
        return MTLSamplerBorderColorOpaqueBlack;
    case BorderColor::OpaqueWhite:
        return MTLSamplerBorderColorOpaqueWhite;
    }
    throw std::invalid_argument("Invalid Metal border color");
}
} // namespace
Sampler Runtime::CreateSampler(const SamplerDesc& desc) {
    @autoreleasepool {
        Require(desc.max_anisotropy >= 1 && desc.max_anisotropy <= 16 &&
                    std::isfinite(desc.lod_min) && std::isfinite(desc.lod_max) &&
                    desc.lod_min >= 0.0f && desc.lod_min <= desc.lod_max,
                "Invalid Metal sampler description");
        auto descriptor = [MTLSamplerDescriptor new];
        descriptor.supportArgumentBuffers = YES;
        descriptor.minFilter = MinMag(desc.min_filter);
        descriptor.magFilter = MinMag(desc.mag_filter);
        descriptor.mipFilter = Mip(desc.mip_filter);
        descriptor.sAddressMode = Address(desc.address[0]);
        descriptor.tAddressMode = Address(desc.address[1]);
        descriptor.rAddressMode = Address(desc.address[2]);
        descriptor.lodMinClamp = desc.lod_min;
        descriptor.lodMaxClamp = desc.lod_max;
        descriptor.maxAnisotropy = desc.max_anisotropy;
        descriptor.borderColor = Border(desc.border);
        descriptor.normalizedCoordinates = desc.normalized_coordinates;
        if (desc.compare)
            descriptor.compareFunction = Compare(desc.compare_function);
        Sampler result;
        result.state = MakeResource(
            impl->device, [impl->device->device newSamplerStateWithDescriptor:descriptor]);
        return result;
    }
}
Commands Runtime::BeginCommands(std::string_view label) {
    @autoreleasepool {
        std::scoped_lock lock{impl->device->mutex};
        const auto active = impl->device->recording.lock();
        Require(!active || active->submitted || active->cancelled,
                "Finish the previous Metal recording before beginning another");
        auto commands = std::make_unique<Commands::Impl>();
        commands->submission = std::make_shared<SubmissionState>();
        commands->submission->owner = impl->device;
        commands->submission->object = [impl->device->queue commandBuffer];
        g_command_buffers.fetch_add(1, std::memory_order_relaxed);
        Require(commands->submission->object != nil, "Metal command buffer allocation failed");
        {
            std::weak_ptr<SubmissionState> weak = commands->submission;
            [commands->submission->object addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
              if (const auto state = weak.lock())
                  state->Complete(buffer);
            }];
        }
        commands->submission->serial = impl->device->next_serial++;
        commands->submission->object.label = [[NSString alloc] initWithBytes:label.data()
                                                                      length:label.size()
                                                                    encoding:NSUTF8StringEncoding];
        impl->device->recording = commands->submission;
        return Commands{std::move(commands)};
    }
}
Commands::Commands(std::unique_ptr<Impl> impl_) : impl{std::move(impl_)} {}
Commands::Commands(Commands&&) noexcept = default;
Commands& Commands::operator=(Commands&& other) noexcept {
    if (this != &other) {
        if (impl && !impl->submission->submitted) {
            impl->End();
            impl->submission->cancelled = true;
        }
        impl = std::move(other.impl);
    }
    return *this;
}
Commands::~Commands() {
    if (impl && !impl->submission->submitted) {
        impl->End();
        impl->submission->cancelled = true;
    }
}
void Commands::BeginRender(std::span<const ColorAttachment> colors, const Texture* depth,
                           bool clear_depth, double depth_value, bool clear_stencil,
                           uint32_t stencil_value) {
    @autoreleasepool {
        impl->Check();
        impl->End();
        Require((!colors.empty() || depth != nullptr) && colors.size() <= 8,
                "Invalid Metal render attachment count");
        auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        impl->color_count = static_cast<uint32_t>(colors.size());
        // Guest render targets can leave gaps; size the pass from the first bound attachment.
        const auto first = std::find_if(colors.begin(), colors.end(),
                                        [](const ColorAttachment& c) { return c.texture.Valid(); });
        Require(first != colors.end() || depth != nullptr, "Metal render pass has no attachments");
        // Guest attachments may differ in size (e.g. a padded depth buffer); the render area
        // is their intersection, as in Vulkan's framebuffer extent.
        impl->width = first != colors.end() ? first->texture.Width() : depth->Width();
        impl->height = first != colors.end() ? first->texture.Height() : depth->Height();
        for (const auto& color : colors) {
            if (!color.texture.Valid())
                continue;
            impl->width = std::min(impl->width, color.texture.Width());
            impl->height = std::min(impl->height, color.texture.Height());
        }
        if (depth) {
            impl->width = std::min(impl->width, depth->Width());
            impl->height = std::min(impl->height, depth->Height());
        }
        impl->depth = PixelFormat::Invalid;
        impl->colors.fill(PixelFormat::Invalid);
        for (size_t i = 0; i < colors.size(); ++i) {
            const auto& color = colors[i];
            if (!color.texture.Valid())
                continue;
            Require(!IsDepthFormat(color.texture.Format()) && !HasStencil(color.texture.Format()),
                    "Depth texture cannot be a Metal color attachment");
            Require((static_cast<uint32_t>(color.texture.state->usage) & 4) != 0,
                    "Texture is not a Metal render target");
            impl->Track(color.texture.state);
            MarkModified(color.texture.state);
            impl->colors[i] = color.texture.Format();
            auto attachment = descriptor.colorAttachments[i];
            attachment.texture = (id<MTLTexture>)color.texture.state->object;
            attachment.loadAction = color.clear ? MTLLoadActionClear : MTLLoadActionLoad;
            attachment.storeAction = MTLStoreActionStore;
            attachment.clearColor = MTLClearColorMake(color.clear_color[0], color.clear_color[1],
                                                      color.clear_color[2], color.clear_color[3]);
        }
        if (depth) {
            Require(IsDepthFormat(depth->Format()) || HasStencil(depth->Format()),
                    "Invalid Metal depth attachment format");
            Require((static_cast<uint32_t>(depth->state->usage) & 4) != 0,
                    "Depth texture is not a Metal render target");
            impl->Track(depth->state);
            MarkModified(depth->state);
            impl->depth = depth->Format();
            auto texture = (id<MTLTexture>)depth->state->object;
            if (IsDepthFormat(depth->Format())) {
                descriptor.depthAttachment.texture = texture;
                descriptor.depthAttachment.loadAction =
                    clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
                descriptor.depthAttachment.storeAction = MTLStoreActionStore;
                descriptor.depthAttachment.clearDepth = depth_value;
            }
            if (HasStencil(depth->Format())) {
                descriptor.stencilAttachment.texture = texture;
                descriptor.stencilAttachment.loadAction =
                    clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
                descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
                descriptor.stencilAttachment.clearStencil = stencil_value;
            }
        }
        descriptor.renderTargetWidth = impl->width;
        descriptor.renderTargetHeight = impl->height;
        impl->AttachVisibility(descriptor);
        impl->render = [impl->submission->object renderCommandEncoderWithDescriptor:descriptor];
        Require(impl->render != nil, "Metal render encoder creation failed");
        impl->NewPass();
        SetViewport(0, 0, impl->width, impl->height);
        SetScissor(0, 0, impl->width, impl->height);
    }
}
void Commands::SetRenderPipeline(const Pipeline& pipeline) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    CheckResource(impl->submission->owner, pipeline.state);
    Require(!pipeline.state->compute && pipeline.state->color_count == impl->color_count &&
                pipeline.state->depth == impl->depth,
            "Metal pipeline attachment layout does not match the render pass");
    for (uint32_t i = 0; i < impl->color_count; ++i)
        Require(pipeline.state->colors[i] == impl->colors[i],
                "Metal pipeline color format does not match the render pass");
    if (impl->pipeline != pipeline.state)
        [impl->render setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline.state->object];
    const auto depth = (id<MTLDepthStencilState>)pipeline.state->auxiliary;
    if (impl->depth_state != depth) {
        [impl->render setDepthStencilState:depth];
        impl->depth_state = depth;
    }
    impl->pipeline = pipeline.state;
}
void Commands::SetViewport(double x, double y, double width, double height, double near_depth,
                           double far_depth) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    // Reversed depth ranges (near > far) are valid.
    Require(std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) &&
                width > 0 && height > 0 && std::isfinite(near_depth) && std::isfinite(far_depth) &&
                near_depth >= 0 && near_depth <= 1 && far_depth >= 0 && far_depth <= 1,
            "Invalid Metal viewport");
    [impl->render setViewport:MTLViewport{x, y, width, height, near_depth, far_depth}];
}
void Commands::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    x = std::min(x, impl->width);
    y = std::min(y, impl->height);
    [impl->render setScissorRect:MTLScissorRect{x, y, std::min(width, impl->width - x),
                                                std::min(height, impl->height - y)}];
}
bool Commands::RenderPassActive() const {
    return impl && !impl->submission->submitted && impl->render != nil;
}
uint64_t Commands::RenderPassId() const {
    return impl ? impl->pass_id : 0;
}
void Commands::BeginRenderWithoutAttachments(uint32_t width, uint32_t height) {
    @autoreleasepool {
        impl->Check();
        impl->End();
        Require(width > 0 && height > 0 && width <= 16384 && height <= 16384,
                "Invalid Metal attachment-less render area");
        auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        descriptor.renderTargetWidth = width;
        descriptor.renderTargetHeight = height;
        descriptor.defaultRasterSampleCount = 1;
        impl->color_count = 0;
        impl->width = width;
        impl->height = height;
        impl->depth = PixelFormat::Invalid;
        impl->colors.fill(PixelFormat::Invalid);
        impl->AttachVisibility(descriptor);
        impl->render = [impl->submission->object renderCommandEncoderWithDescriptor:descriptor];
        Require(impl->render != nil, "Metal render encoder creation failed");
        impl->NewPass();
        SetViewport(0, 0, width, height);
        SetScissor(0, 0, width, height);
    }
}
void Commands::SetVisibilityBuffer(const Buffer& buffer) {
    impl->Check();
    Require(buffer.state != nullptr, "Missing Metal visibility buffer");
    CheckResource(impl->submission->owner, buffer.state);
    impl->visibility = buffer.state;
}
void Commands::SetVisibilityResult(bool counting, size_t offset) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    Require(impl->visibility && offset % 8 == 0 && offset + 8 <= impl->visibility->bytes,
            "Invalid Metal visibility result offset");
    [impl->render setVisibilityResultMode:counting ? MTLVisibilityResultModeCounting
                                                   : MTLVisibilityResultModeDisabled
                                   offset:offset];
}
void Commands::SetDepthStencilState(const DepthStencilState& state) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    CheckResource(impl->submission->owner, state.state);
    const auto depth = (id<MTLDepthStencilState>)state.state->object;
    if (impl->depth_state != depth) {
        [impl->render setDepthStencilState:depth];
        impl->depth_state = depth;
    }
}
void Commands::SetCullMode(CullMode mode) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setCullMode:mode == CullMode::None    ? MTLCullModeNone
                              : mode == CullMode::Front ? MTLCullModeFront
                                                        : MTLCullModeBack];
}
void Commands::SetFrontFacing(Winding winding) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setFrontFacingWinding:winding == Winding::Clockwise
                                            ? MTLWindingClockwise
                                            : MTLWindingCounterClockwise];
}
void Commands::SetDepthBias(float bias, float slope_scale, float clamp) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setDepthBias:bias slopeScale:slope_scale clamp:clamp];
}
void Commands::SetDepthClamp(bool clamp) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setDepthClipMode:clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
}
void Commands::SetStencilReference(uint32_t front, uint32_t back) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setStencilFrontReferenceValue:front & 0xFF backReferenceValue:back & 0xFF];
}
void Commands::SetBlendColor(float red, float green, float blue, float alpha) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setBlendColorRed:red green:green blue:blue alpha:alpha];
}
void Commands::SetFillLines(bool lines) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    [impl->render setTriangleFillMode:lines ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
}
void Commands::SetViewports(std::span<const Viewport> viewports) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    Require(!viewports.empty() && viewports.size() <= 16, "Invalid Metal viewport count");
    std::array<MTLViewport, 16> native{};
    for (size_t i = 0; i < viewports.size(); ++i) {
        const auto& v = viewports[i];
        Require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.width) &&
                    // Negative heights flip Y (Vulkan VK_KHR_maintenance1 semantics);
                    // Metal accepts them, as MoltenVK relies on.
                    std::isfinite(v.height) && v.width > 0 && v.height != 0 &&
                    v.near_depth >= 0 && v.near_depth <= 1 && v.far_depth >= 0 &&
                    v.far_depth <= 1,
                "Invalid Metal viewport");
        native[i] = MTLViewport{v.x, v.y, v.width, v.height, v.near_depth, v.far_depth};
    }
    [impl->render setViewports:native.data() count:viewports.size()];
}
void Commands::SetScissors(std::span<const ScissorRect> scissors) {
    impl->Check();
    Require(impl->render != nil, "No active Metal render pass");
    Require(!scissors.empty() && scissors.size() <= 16, "Invalid Metal scissor count");
    std::array<MTLScissorRect, 16> native{};
    for (size_t i = 0; i < scissors.size(); ++i) {
        const auto& r = scissors[i];
        // Clamp to the render area (guest framebuffers can exceed the smallest attachment).
        const uint32_t x = std::min(r.x, impl->width);
        const uint32_t y = std::min(r.y, impl->height);
        native[i] = MTLScissorRect{x, y, std::min(r.width, impl->width - x),
                                   std::min(r.height, impl->height - y)};
    }
    [impl->render setScissorRects:native.data() count:scissors.size()];
}
void Commands::SetBytes(Stage stage, uint32_t slot, std::span<const std::byte> bytes) {
    impl->Check();
    Require(slot < 31 && !bytes.empty() && bytes.size() <= 4096, "Invalid Metal inline bytes");
    Require(stage == Stage::Compute ? impl->compute != nil : impl->render != nil,
            "No active Metal encoder for the inline-bytes stage");
    auto& cached = impl->buffer_slots[static_cast<size_t>(stage)][slot];
    if (!cached.resource && cached.bytes.size() == bytes.size() &&
        std::equal(bytes.begin(), bytes.end(), cached.bytes.begin())) {
        g_bindings_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    cached.resource.reset();
    cached.bytes.assign(bytes.begin(), bytes.end());
    switch (stage) {
    case Stage::Vertex:
        [impl->render setVertexBytes:bytes.data() length:bytes.size() atIndex:slot];
        break;
    case Stage::Fragment:
        [impl->render setFragmentBytes:bytes.data() length:bytes.size() atIndex:slot];
        break;
    case Stage::Compute:
        [impl->compute setBytes:bytes.data() length:bytes.size() atIndex:slot];
        break;
    }
}
void Commands::DrawInstanced(Primitive primitive, uint32_t first, uint32_t count,
                             uint32_t instances, uint32_t base_instance) {
    impl->Check();
    Require(impl->render != nil && impl->pipeline != nullptr, "Metal draw needs a render pipeline");
    [impl->render drawPrimitives:Topology(primitive)
                     vertexStart:first
                     vertexCount:count
                   instanceCount:instances
                    baseInstance:base_instance];
}
void Commands::CopyTextureToBuffer(const Texture& src, const TextureRegion& region,
                                   const Buffer& dst, size_t offset, size_t pitch,
                                   size_t image_pitch, TransferPlane plane) {
    ValidateTransfer(src, region, dst, offset, pitch, image_pitch, plane);
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromTexture:(id<MTLTexture>)src.state->object
                     sourceSlice:region.slice
                     sourceLevel:region.level
                    sourceOrigin:MTLOriginMake(region.x, region.y, region.z)
                      sourceSize:MTLSizeMake(region.width, region.height, region.depth)
                        toBuffer:(id<MTLBuffer>)dst.state->object
               destinationOffset:offset
          destinationBytesPerRow:pitch
        destinationBytesPerImage:image_pitch
                         options:PlaneOption(plane)];
}
void Commands::CopyBufferToTexture(const Buffer& src, size_t offset, size_t pitch,
                                   size_t image_pitch, const Texture& dst,
                                   const TextureRegion& region, TransferPlane plane) {
    ValidateTransfer(dst, region, src, offset, pitch, image_pitch, plane);
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromBuffer:(id<MTLBuffer>)src.state->object
                  sourceOffset:offset
             sourceBytesPerRow:pitch
           sourceBytesPerImage:image_pitch
                    sourceSize:MTLSizeMake(region.width, region.height, region.depth)
                     toTexture:(id<MTLTexture>)dst.state->object
              destinationSlice:region.slice
              destinationLevel:region.level
             destinationOrigin:MTLOriginMake(region.x, region.y, region.z)
                       options:PlaneOption(plane)];
}
void Commands::SetBuffer(Stage stage, uint32_t slot, const Buffer& buffer, size_t offset) {
    impl->Check();
    Require(slot < 31 && offset <= buffer.Size() && offset % 4 == 0,
            "Invalid Metal buffer binding");
    Require(stage == Stage::Compute ? impl->compute != nil : impl->render != nil,
            "No active Metal encoder for the buffer stage");
    auto& cached = impl->buffer_slots[static_cast<size_t>(stage)][slot];
    if (cached.resource == buffer.state && cached.offset == offset) {
        g_bindings_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    impl->Track(buffer.state);
    cached.resource = buffer.state;
    cached.offset = offset;
    cached.bytes.clear();
    auto object = (id<MTLBuffer>)buffer.state->object;
    switch (stage) {
    case Stage::Vertex:
        [impl->render setVertexBuffer:object offset:offset atIndex:slot];
        break;
    case Stage::Fragment:
        [impl->render setFragmentBuffer:object offset:offset atIndex:slot];
        break;
    case Stage::Compute:
        [impl->compute setBuffer:object offset:offset atIndex:slot];
        break;
    }
}
void Commands::SetTexture(Stage stage, uint32_t slot, const Texture& texture) {
    impl->Check();
    Require(slot < 128, "Metal texture slot exceeds the limit");
    Require(stage == Stage::Compute ? impl->compute != nil : impl->render != nil,
            "No active Metal encoder for the texture stage");
    impl->Track(texture.state);
    auto object = (id<MTLTexture>)texture.state->object;
    switch (stage) {
    case Stage::Vertex:
        [impl->render setVertexTexture:object atIndex:slot];
        break;
    case Stage::Fragment:
        [impl->render setFragmentTexture:object atIndex:slot];
        break;
    case Stage::Compute:
        [impl->compute setTexture:object atIndex:slot];
        break;
    }
}
void Commands::SetSampler(Stage stage, uint32_t slot, const Sampler& sampler) {
    impl->Check();
    Require(slot < 16, "Metal sampler slot exceeds the limit");
    Require(stage == Stage::Compute ? impl->compute != nil : impl->render != nil,
            "No active Metal encoder for the sampler stage");
    CheckResource(impl->submission->owner, sampler.state);
    auto object = (id<MTLSamplerState>)sampler.state->object;
    switch (stage) {
    case Stage::Vertex:
        [impl->render setVertexSamplerState:object atIndex:slot];
        break;
    case Stage::Fragment:
        [impl->render setFragmentSamplerState:object atIndex:slot];
        break;
    case Stage::Compute:
        [impl->compute setSamplerState:object atIndex:slot];
        break;
    }
}
void Commands::Draw(Primitive primitive, uint32_t first, uint32_t count, uint32_t instances) {
    impl->Check();
    Require(impl->render != nil && impl->pipeline != nullptr, "Metal draw needs a render pipeline");
    [impl->render drawPrimitives:Topology(primitive)
                     vertexStart:first
                     vertexCount:count
                   instanceCount:instances];
}
void Commands::DrawIndexed(Primitive primitive, const Buffer& indices, bool index32, size_t offset,
                           uint32_t count, uint32_t instances, int32_t base_vertex,
                           uint32_t base_instance) {
    impl->Check();
    Require(impl->render != nil && impl->pipeline != nullptr,
            "Metal indexed draw needs a render pipeline");
    const size_t stride = index32 ? 4 : 2;
    Require(offset % stride == 0, "Misaligned Metal index buffer");
    CheckRange(offset, static_cast<size_t>(count) * stride, indices.Size());
    impl->Track(indices.state);
    [impl->render drawIndexedPrimitives:Topology(primitive)
                             indexCount:count
                              indexType:index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:(id<MTLBuffer>)indices.state->object
                      indexBufferOffset:offset
                          instanceCount:instances
                             baseVertex:base_vertex
                           baseInstance:base_instance];
}
void Commands::DrawIndirect(Primitive primitive, const Buffer& arguments, size_t offset) {
    impl->Check();
    Require(impl->render != nil && impl->pipeline != nullptr,
            "Metal indirect draw needs a render pipeline");
    Require(offset % 4 == 0, "Misaligned Metal indirect draw arguments");
    CheckRange(offset, 16, arguments.Size());
    impl->Track(arguments.state);
    [impl->render drawPrimitives:Topology(primitive)
                  indirectBuffer:(id<MTLBuffer>)arguments.state->object
            indirectBufferOffset:offset];
}
void Commands::DrawIndexedIndirect(Primitive primitive, const Buffer& indices, bool index32,
                                   size_t index_offset, const Buffer& arguments, size_t offset) {
    impl->Check();
    Require(impl->render != nil && impl->pipeline != nullptr,
            "Metal indirect draw needs a render pipeline");
    Require(offset % 4 == 0 && index_offset % (index32 ? 4 : 2) == 0,
            "Misaligned Metal indirect indexed draw");
    CheckRange(offset, 20, arguments.Size());
    impl->Track(indices.state);
    impl->Track(arguments.state);
    [impl->render drawIndexedPrimitives:Topology(primitive)
                              indexType:index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:(id<MTLBuffer>)indices.state->object
                      indexBufferOffset:index_offset
                         indirectBuffer:(id<MTLBuffer>)arguments.state->object
                   indirectBufferOffset:offset];
}
void Commands::DispatchIndirect(const Buffer& arguments, size_t offset,
                                std::array<uint32_t, 3> local_size) {
    impl->Check();
    Require(impl->compute != nil && impl->pipeline != nullptr, "No active Metal compute pipeline");
    Require(offset % 4 == 0, "Misaligned Metal indirect dispatch arguments");
    CheckRange(offset, 12, arguments.Size());
    impl->Track(arguments.state);
    [impl->compute
        dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)arguments.state->object
                          indirectBufferOffset:offset
                         threadsPerThreadgroup:MTLSizeMake(local_size[0], local_size[1],
                                                           local_size[2])];
}
void Commands::BeginCompute(const Pipeline& pipeline) {
    @autoreleasepool {
        impl->Check();
        CheckResource(impl->submission->owner, pipeline.state);
        Require(pipeline.state->compute, "Metal compute dispatch needs a compute pipeline");
        if (!impl->compute) {
            impl->End();
            impl->compute = [impl->submission->object computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
            g_compute_encoders.fetch_add(1, std::memory_order_relaxed);
            Require(impl->compute != nil, "Metal compute encoder creation failed");
        }
        impl->encoder_uses.clear();
        if (impl->pipeline != pipeline.state)
            [impl->compute setComputePipelineState:(id<MTLComputePipelineState>)pipeline.state->object];
        impl->pipeline = pipeline.state;
    }
}
void Commands::Dispatch(std::array<uint32_t, 3> groups, std::array<uint32_t, 3> local_size) {
    impl->Check();
    Require(impl->compute != nil && impl->pipeline != nullptr, "No active Metal compute pipeline");
    auto pipeline = (id<MTLComputePipelineState>)impl->pipeline->object;
    uint64_t threads = 1;
    for (size_t i = 0; i < 3; ++i) {
        Require(groups[i] > 0 && local_size[i] > 0 &&
                    local_size[i] <= pipeline.maxTotalThreadsPerThreadgroup,
                "Invalid Metal dispatch dimensions");
        threads *= local_size[i];
    }
    Require(threads <= pipeline.maxTotalThreadsPerThreadgroup,
            "Metal workgroup exceeds the device limit");
    [impl->compute dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2])
                  threadsPerThreadgroup:MTLSizeMake(local_size[0], local_size[1], local_size[2])];
}
void Commands::CopyBuffer(const Buffer& src, size_t src_offset, const Buffer& dst,
                          size_t dst_offset, size_t size) {
    CheckRange(src_offset, size, src.Size());
    CheckRange(dst_offset, size, dst.Size());
    if (src.state == dst.state && size > 0)
        Require(src_offset >= dst_offset + size || dst_offset >= src_offset + size,
                "Overlapping Metal buffer copies are unsupported");
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromBuffer:(id<MTLBuffer>)src.state->object
                  sourceOffset:src_offset
                      toBuffer:(id<MTLBuffer>)dst.state->object
             destinationOffset:dst_offset
                          size:size];
}
void Commands::CopyTextureToBuffer(const Texture& src, const Buffer& dst, size_t offset,
                                   size_t pitch) {
    CopyTextureToBuffer(src, {0, 0, 0, 0, 0, src.Width(), src.Height(), 1}, dst, offset, pitch,
                        pitch * src.Height());
}
void Commands::CopyBufferToTexture(const Buffer& src, size_t offset, size_t pitch,
                                   const Texture& dst) {
    CopyBufferToTexture(src, offset, pitch, pitch * dst.Height(), dst,
                        {0, 0, 0, 0, 0, dst.Width(), dst.Height(), 1});
}
void Commands::CopyTextureToBuffer(const Texture& src, const TextureRegion& region,
                                   const Buffer& dst, size_t offset, size_t pitch,
                                   size_t image_pitch) {
    ValidateTransfer(src, region, dst, offset, pitch, image_pitch);
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromTexture:(id<MTLTexture>)src.state->object
                     sourceSlice:region.slice
                     sourceLevel:region.level
                    sourceOrigin:MTLOriginMake(region.x, region.y, region.z)
                      sourceSize:MTLSizeMake(region.width, region.height, region.depth)
                        toBuffer:(id<MTLBuffer>)dst.state->object
               destinationOffset:offset
          destinationBytesPerRow:pitch
        destinationBytesPerImage:image_pitch];
}
void Commands::CopyBufferToTexture(const Buffer& src, size_t offset, size_t pitch,
                                   size_t image_pitch, const Texture& dst,
                                   const TextureRegion& region) {
    ValidateTransfer(dst, region, src, offset, pitch, image_pitch);
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromBuffer:(id<MTLBuffer>)src.state->object
                  sourceOffset:offset
             sourceBytesPerRow:pitch
           sourceBytesPerImage:image_pitch
                    sourceSize:MTLSizeMake(region.width, region.height, region.depth)
                     toTexture:(id<MTLTexture>)dst.state->object
              destinationSlice:region.slice
              destinationLevel:region.level
             destinationOrigin:MTLOriginMake(region.x, region.y, region.z)];
}
void Commands::CopyTexture(const Texture& src, const TextureRegion& region, const Texture& dst,
                           uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t z) {
    ValidateRegion(src, region);
    ValidateRegion(dst, {level, slice, x, y, z, region.width, region.height, region.depth});
    Require(src.Format() == dst.Format(), "Metal image copy format mismatch");
    // Overlapping image copies need a temporary image in the cache runtime.
    Require(src.state != dst.state, "Metal self image copy requires temporary storage");
    impl->Blit();
    impl->Track(src.state);
    impl->Track(dst.state);
    MarkModified(dst.state);
    [impl->blit copyFromTexture:(id<MTLTexture>)src.state->object
                    sourceSlice:region.slice
                    sourceLevel:region.level
                   sourceOrigin:MTLOriginMake(region.x, region.y, region.z)
                     sourceSize:MTLSizeMake(region.width, region.height, region.depth)
                      toTexture:(id<MTLTexture>)dst.state->object
               destinationSlice:slice
               destinationLevel:level
              destinationOrigin:MTLOriginMake(x, y, z)];
}
Submission Commands::Submit() {
    impl->Check();
    impl->End();
    std::scoped_lock lock{impl->submission->mutex};
    [impl->submission->object commit];
    impl->submission->submitted = true;
    Submission result;
    result.state = impl->submission;
    return result;
}
std::vector<std::byte> Runtime::ReadTexture(const Texture& texture) {
    const auto row_bytes = static_cast<size_t>(texture.Width()) * PixelBytes(texture.Format());
    const auto pitch = (row_bytes + 255) & ~size_t{255};
    auto buffer = CreateBuffer(pitch * texture.Height());
    auto commands = BeginCommands("Citrosis Metal readback");
    commands.CopyTextureToBuffer(texture, buffer, 0, pitch);
    commands.Submit().Wait();
    std::vector<std::byte> padded(pitch * texture.Height());
    buffer.Read(0, padded);
    std::vector<std::byte> result(row_bytes * texture.Height());
    for (uint32_t row = 0; row < texture.Height(); ++row)
        std::memcpy(result.data() + row * row_bytes, padded.data() + row * pitch, row_bytes);
    return result;
}
Submission Runtime::Present(void* metal_layer, const Texture& texture) {
    return Present(metal_layer, texture, PresentDesc{});
}
Submission Runtime::Present(void* metal_layer, const Texture& texture, const PresentDesc& present) {
    @autoreleasepool {
        Require(metal_layer != nullptr,
                "No CAMetalLayer is available for native Metal presentation");
        CheckResource(impl->device, texture.state);
        // Accept a CAMetalLayer, or a view backed by one (SDL_Metal_CreateView).
        id object = (__bridge id)metal_layer;
        if (![object isKindOfClass:[CAMetalLayer class]] && [object respondsToSelector:@selector(layer)])
            object = [object valueForKey:@"layer"];
        Require([object isKindOfClass:[CAMetalLayer class]],
                "The presentation surface is not backed by a CAMetalLayer");
        auto layer = (CAMetalLayer*)object;
        layer.device = impl->device->device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (present.drawable_width > 0 && present.drawable_height > 0) {
            const CGSize size = CGSizeMake(present.drawable_width, present.drawable_height);
            if (!CGSizeEqualToSize(layer.drawableSize, size))
                layer.drawableSize = size;
        }
        auto drawable = [layer nextDrawable];
        if (!drawable)
            return {};
        Texture target;
        target.state = MakeResource(impl->device, drawable.texture);
        target.state->width = static_cast<uint32_t>(drawable.texture.width);
        target.state->height = static_cast<uint32_t>(drawable.texture.height);
        target.state->format = PixelFormat::BGRA8;
        target.state->usage = TextureUsage::RenderTarget;
        static constexpr std::string_view shader = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; float2 uv; };
struct Crop { float4 rect; };
vertex V present_vertex(uint id [[vertex_id]]) {
    float2 p = id == 0 ? float2(-1,-1) : id == 1 ? float2(3,-1) : float2(-1,3);
    return {float4(p,0,1), float2(p.x*.5+.5, .5-p.y*.5)};
}
fragment float4 present_fragment(V in [[stage_in]], texture2d<float> image [[texture(0)]],
                                 sampler s [[sampler(0)]], constant Crop& crop [[buffer(0)]]) {
    const float2 uv = mix(crop.rect.xy, crop.rect.zw, in.uv);
    return float4(image.sample(s, uv).rgb, 1.0);
})MSL";
        Pipeline pipeline;
        if (!impl->present_pipeline) {
            RenderPipelineDesc desc;
            desc.vertex = CompileMSL(shader, "present_vertex", Stage::Vertex);
            desc.fragment = CompileMSL(shader, "present_fragment", Stage::Fragment);
            desc.colors[0] = PixelFormat::BGRA8;
            impl->present_pipeline = CreateRenderPipeline(desc).state;
        }
        pipeline.state = impl->present_pipeline;
        auto& sampler_state = impl->present_samplers[present.linear ? 1 : 0];
        if (!sampler_state)
            sampler_state = CreateSampler(present.linear).state;
        Sampler present_sampler;
        present_sampler.state = sampler_state;
        const std::array colors{ColorAttachment{target, true, {0, 0, 0, 1}}};
        auto commands = BeginCommands("Citrosis native Metal presentation");
        commands.BeginRender(colors);
        commands.SetRenderPipeline(pipeline);
        double x = present.dst_x, y = present.dst_y;
        double width = present.dst_width, height = present.dst_height;
        if (width <= 0 || height <= 0) {
            const double scale = std::min(static_cast<double>(target.Width()) / texture.Width(),
                                          static_cast<double>(target.Height()) / texture.Height());
            width = texture.Width() * scale;
            height = texture.Height() * scale;
            x = (target.Width() - width) / 2;
            y = (target.Height() - height) / 2;
        }
        commands.SetViewport(x, y, std::max(width, 1.0), std::max(height, 1.0));
        const std::array<float, 4> crop{present.u0, present.v0, present.u1, present.v1};
        commands.SetBytes(Stage::Fragment, 0, std::as_bytes(std::span{crop}));
        commands.SetTexture(Stage::Fragment, 0, texture);
        commands.SetSampler(Stage::Fragment, 0, present_sampler);
        commands.Draw(Primitive::Triangles, 0, 3);
        commands.impl->End();
        [commands.impl->submission->object presentDrawable:drawable];
        return commands.Submit();
    }
}
} // namespace NativeMetal

namespace NativeMetal {
ArgumentBuffer Runtime::CreateArguments(const Function& function, const ShaderSource& source,
                                        uint32_t descriptor_set) {
    @autoreleasepool {
        CheckResource(impl->device, function.state);
        Require(impl->device->device.argumentBuffersSupport == MTLArgumentBuffersTier2,
                "The native Metal shader path requires tier-2 argument buffers");
        Require(descriptor_set < source.argument_sets.size() &&
                    source.argument_sets[descriptor_set],
                "This shader has no argument buffer for the requested descriptor set");
        Require(source.msl == function.state->source && source.stage == function.state->stage,
                "Metal argument metadata does not match the compiled function");
        ArgumentBuffer result;
        result.slot = ArgumentBufferBase + descriptor_set;
        result.stage = source.stage;
        auto encoder =
            [(id<MTLFunction>)function.state->object newArgumentEncoderWithBufferIndex:result.slot];
        Require(encoder != nil && encoder.encodedLength > 0,
                "Metal argument encoder creation failed");
        result.layout = MakeResource(impl->device, encoder);
        result.layout->encoder_mutex = std::make_shared<std::mutex>();
        result.storage = CreateBuffer(encoder.encodedLength);
        std::vector<std::byte> zeros(encoder.encodedLength);
        result.storage.Write(0, zeros);
        [encoder setArgumentBuffer:(id<MTLBuffer>)result.storage.state->object offset:0];
        for (const auto& binding : source.bindings) {
            if (binding.set != descriptor_set || binding.kind == ResourceKind::PushConstant)
                continue;
            const auto add = [&](uint32_t first, ResourceKind kind) {
                if (first == InvalidBinding)
                    return;
                for (uint32_t index = 0; index < binding.count; ++index) {
                    if (!result.layout->members.emplace(first + index, ResourceState::Member{kind})
                             .second)
                        throw std::runtime_error("Overlapping Metal argument member IDs");
                }
            };
            add(binding.buffer, binding.kind);
            add(binding.texture, binding.kind == ResourceKind::StorageImage
                                     ? ResourceKind::StorageImage
                                     : ResourceKind::SeparateImage);
            add(binding.sampler, ResourceKind::SeparateSampler);
        }
        return result;
    }
}
void ArgumentBuffer::SetBuffer(uint32_t member, const Buffer& buffer, size_t offset) {
    Require(layout != nullptr, "Missing Metal argument buffer");
    CheckResource(layout->owner.lock(), buffer.state);
    Require(offset < buffer.Size() && offset % 4 == 0, "Invalid Metal argument buffer offset");
    std::scoped_lock lock{storage.state->mutex, layout->mutex, *layout->encoder_mutex};
    if (storage.state->last_use)
        storage.state->last_use->Wait();
    [(id<MTLArgumentEncoder>)layout->object setArgumentBuffer:(id<MTLBuffer>)storage.state->object
                                                       offset:0];
    const auto found = layout->members.find(member);
    Require(found != layout->members.end() && (found->second.kind == ResourceKind::UniformBuffer ||
                                               found->second.kind == ResourceKind::StorageBuffer ||
                                               found->second.kind == ResourceKind::BufferSizes),
            "Metal argument member is not a buffer");
    [(id<MTLArgumentEncoder>)layout->object setBuffer:(id<MTLBuffer>)buffer.state->object
                                               offset:offset
                                              atIndex:member];
    layout->argument_resources[member] = buffer.state;
    layout->argument_writes.erase(member);
}
void ArgumentBuffer::SetTexture(uint32_t member, const Texture& texture) {
    Require(layout != nullptr, "Missing Metal argument buffer");
    CheckResource(layout->owner.lock(), texture.state);
    std::scoped_lock lock{storage.state->mutex, layout->mutex, *layout->encoder_mutex};
    if (storage.state->last_use)
        storage.state->last_use->Wait();
    [(id<MTLArgumentEncoder>)layout->object setArgumentBuffer:(id<MTLBuffer>)storage.state->object
                                                       offset:0];
    const auto found = layout->members.find(member);
    Require(found != layout->members.end() && (found->second.kind == ResourceKind::SeparateImage ||
                                               found->second.kind == ResourceKind::StorageImage),
            "Metal argument member is not a texture");
    const auto required_usage = found->second.kind == ResourceKind::StorageImage ? 2u : 1u;
    Require((static_cast<uint32_t>(texture.state->usage) & required_usage) != 0,
            "Metal argument texture has incompatible usage");
    [(id<MTLArgumentEncoder>)layout->object setTexture:(id<MTLTexture>)texture.state->object
                                               atIndex:member];
    layout->argument_resources[member] = texture.state;
    layout->argument_writes.erase(member);
}
void ArgumentBuffer::SetSampler(uint32_t member, const Sampler& sampler) {
    Require(layout != nullptr, "Missing Metal argument buffer");
    CheckResource(layout->owner.lock(), sampler.state);
    std::scoped_lock lock{storage.state->mutex, layout->mutex, *layout->encoder_mutex};
    if (storage.state->last_use)
        storage.state->last_use->Wait();
    [(id<MTLArgumentEncoder>)layout->object setArgumentBuffer:(id<MTLBuffer>)storage.state->object
                                                       offset:0];
    const auto found = layout->members.find(member);
    Require(found != layout->members.end() && found->second.kind == ResourceKind::SeparateSampler,
            "Metal argument member is not a sampler");
    [(id<MTLArgumentEncoder>)layout->object
        setSamplerState:(id<MTLSamplerState>)sampler.state->object
                atIndex:member];
    layout->argument_resources[member] = sampler.state;
}
void ArgumentBuffer::Encode(std::span<const ArgumentWrite> writes) {
    Require(layout != nullptr, "Missing Metal argument buffer");
    std::scoped_lock lock{storage.state->mutex, layout->mutex, *layout->encoder_mutex};
    if (storage.state->last_use)
        storage.state->last_use->Wait();
    auto encoder = (id<MTLArgumentEncoder>)layout->object;
    [encoder setArgumentBuffer:(id<MTLBuffer>)storage.state->object offset:0];
    for (const ArgumentWrite& write : writes) {
        const auto found = layout->members.find(write.member);
        Require(found != layout->members.end(), "Unknown Metal argument member");
        const ResourceKind kind = found->second.kind;
        switch (write.kind) {
        case ArgumentWrite::Kind::Buffer: {
            Require(write.buffer && write.buffer->state &&
                        (kind == ResourceKind::UniformBuffer ||
                         kind == ResourceKind::StorageBuffer || kind == ResourceKind::BufferSizes) &&
                        write.offset < write.buffer->state->bytes && write.offset % 4 == 0,
                    "Invalid Metal argument buffer member write");
            [encoder setBuffer:(id<MTLBuffer>)write.buffer->state->object
                        offset:write.offset
                       atIndex:write.member];
            layout->argument_resources[write.member] = write.buffer->state;
            layout->argument_writes[write.member] = write.is_written;
            break;
        }
        case ArgumentWrite::Kind::Texture: {
            Require(write.texture && write.texture->state &&
                        (kind == ResourceKind::SeparateImage || kind == ResourceKind::StorageImage),
                    "Invalid Metal argument texture member write");
            const auto required_usage = kind == ResourceKind::StorageImage ? 2u : 1u;
            Require((static_cast<uint32_t>(write.texture->state->usage) & required_usage) != 0,
                    "Metal argument texture has incompatible usage");
            [encoder setTexture:(id<MTLTexture>)write.texture->state->object atIndex:write.member];
            layout->argument_resources[write.member] = write.texture->state;
            layout->argument_writes[write.member] = write.is_written;
            break;
        }
        case ArgumentWrite::Kind::Sampler:
            Require(write.sampler && write.sampler->state && kind == ResourceKind::SeparateSampler,
                    "Invalid Metal argument sampler member write");
            [encoder setSamplerState:(id<MTLSamplerState>)write.sampler->state->object
                             atIndex:write.member];
            layout->argument_resources[write.member] = write.sampler->state;
            break;
        }
    }
}
const Buffer& ArgumentBuffer::UploadSizeTable(Runtime& runtime, std::span<const uint32_t> sizes) {
    Require(!Busy(), "Cannot overwrite a GPU-owned Metal argument size table");
    const size_t bytes = std::max<size_t>(sizes.size_bytes(), 16);
    if (!size_table.Valid() || size_table.Size() < bytes)
        size_table = runtime.CreateBuffer(bytes);
    size_table.Write(0, std::as_bytes(sizes));
    return size_table;
}
bool ArgumentBuffer::Busy() const {
    if (!storage.state)
        return false;
    std::scoped_lock lock{storage.state->mutex};
    const auto& use = storage.state->last_use;
    if (!use || use->cancelled.load())
        return false;
    if (!use->submitted.load())
        return true; // owned by the open recording
    return !use->Done();
}
ArgumentBuffer Runtime::CloneArguments(const ArgumentBuffer& prototype) {
    @autoreleasepool {
        Require(prototype.layout != nullptr, "Missing Metal argument buffer prototype");
        CheckResource(impl->device, prototype.layout);
        ArgumentBuffer result;
        result.slot = prototype.slot;
        result.stage = prototype.stage;
        result.layout = MakeResource(impl->device, prototype.layout->object);
        result.layout->encoder_mutex = prototype.layout->encoder_mutex;
        {
            std::scoped_lock lock{prototype.layout->mutex};
            result.layout->members = prototype.layout->members;
        }
        result.storage = CreateBuffer(prototype.storage.Size());
        std::vector<std::byte> zeros(prototype.storage.Size());
        result.storage.Write(0, zeros);
        return result;
    }
}
void Commands::SetArguments(Stage stage, const ArgumentBuffer& arguments) {
    Require(arguments.layout != nullptr && arguments.stage == stage,
            "Metal argument buffer has the wrong shader stage");
    CheckResource(impl->submission->owner, arguments.layout);
    SetBuffer(stage, arguments.slot, arguments.storage);
    std::scoped_lock lock{arguments.layout->mutex};
    Require(arguments.layout->argument_resources.size() == arguments.layout->members.size(),
            "Bind every Metal argument member before dispatching the shader");
    for (const auto& [member, resource] : arguments.layout->argument_resources) {
        if (impl->submission->indirect_set.insert(resource.get()).second)
            impl->submission->indirect_objects.push_back(resource->object);
        if (resource->bytes == 0 && resource->width == 0)
            continue; // sampler
        const auto kind = arguments.layout->members.at(member).kind;
        const auto access = arguments.layout->argument_writes.find(member);
        const bool writes = (kind == ResourceKind::StorageBuffer || kind == ResourceKind::StorageImage) &&
                            (access == arguments.layout->argument_writes.end() || access->second);
        // Do this for every draw/dispatch, even when residency was already declared.
        if (writes)
            MarkModified(resource);
        const auto usage = writes ? MTLResourceUsageRead | MTLResourceUsageWrite
                                  : MTLResourceUsageRead;
        const uint64_t use_key = (reinterpret_cast<uintptr_t>(resource.get()) << 4) |
                                 (writes ? 8u : 0u) | static_cast<uint64_t>(stage);
        if (!impl->encoder_uses.insert(use_key).second)
            continue; // already declared on this encoder
        impl->Track(resource);
        if (stage == Stage::Compute)
            [impl->compute useResource:(id<MTLResource>)resource->object usage:usage];
        else
            [impl->render
                useResource:(id<MTLResource>)resource->object
                      usage:usage
                     stages:stage == Stage::Vertex ? MTLRenderStageVertex : MTLRenderStageFragment];
    }
}
} // namespace NativeMetal
