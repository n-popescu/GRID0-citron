// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace NativeMetal {

enum class Stage { Vertex, Fragment, Compute };
enum class PixelFormat {
    Invalid,
    RGBA8,
    BGRA8,
    RGBA16Float,
    R32Uint,
    Depth32Float,
    RGBA8Srgb,
    BGRA8Srgb,
    RGBA8Snorm,
    RGBA8Uint,
    RGBA8Sint,
    R8,
    R8Snorm,
    R8Uint,
    R8Sint,
    RG8,
    RG8Snorm,
    RG8Uint,
    RG8Sint,
    R16Float,
    R16,
    R16Snorm,
    R16Uint,
    R16Sint,
    RG16Float,
    RG16,
    RG16Snorm,
    RG16Uint,
    RG16Sint,
    RGBA16,
    RGBA16Snorm,
    RGBA16Uint,
    RGBA16Sint,
    R32Float,
    R32Sint,
    RG32Float,
    RG32Uint,
    RG32Sint,
    RGBA32Float,
    RGBA32Uint,
    RGBA32Sint,
    RGB10A2,
    RGB10A2Uint,
    RG11B10Float,
    // Additions for guest textures and render targets.
    B5G6R5,
    BGR5A1,
    ABGR4,
    RGB9E5Float,
    BGR10A2,
    R8Srgb,
    RG8Srgb,
    Depth16,
    Depth32FloatStencil8,
    Stencil8,
    BC1,
    BC1Srgb,
    BC2,
    BC2Srgb,
    BC3,
    BC3Srgb,
    BC4,
    BC4Snorm,
    BC5,
    BC5Snorm,
    BC6HUfloat,
    BC6HSfloat,
    BC7,
    BC7Srgb,
    ASTC4x4,
    ASTC4x4Srgb,
    ASTC5x4,
    ASTC5x4Srgb,
    ASTC5x5,
    ASTC5x5Srgb,
    ASTC6x5,
    ASTC6x5Srgb,
    ASTC6x6,
    ASTC6x6Srgb,
    ASTC8x5,
    ASTC8x5Srgb,
    ASTC8x6,
    ASTC8x6Srgb,
    ASTC8x8,
    ASTC8x8Srgb,
    ASTC10x5,
    ASTC10x5Srgb,
    ASTC10x6,
    ASTC10x6Srgb,
    ASTC10x8,
    ASTC10x8Srgb,
    ASTC10x10,
    ASTC10x10Srgb,
    ASTC12x10,
    ASTC12x10Srgb,
    ASTC12x12,
    ASTC12x12Srgb,
};
// Block geometry of a format (1x1 for uncompressed formats).
uint32_t BlockWidth(PixelFormat format);
uint32_t BlockHeight(PixelFormat format);
uint32_t BytesPerBlock(PixelFormat format);
bool IsDepthFormat(PixelFormat format);   // has a depth aspect
bool HasStencil(PixelFormat format);      // has a stencil aspect
// The format with the sRGB encoding removed (the format itself when it has none).
PixelFormat LinearFormat(PixelFormat format);
// Whether Metal can view a texture of format `from` as `to` (same format, same-sized
// uncompressed color formats, or an sRGB/linear pair). Depth/stencil never reinterprets.
bool CanViewAs(PixelFormat from, PixelFormat to);
// Which plane of a combined depth/stencil texture a buffer transfer addresses.
enum class TransferPlane { All, Depth, Stencil };
enum class TextureDimension { D1, D2, D3, D1Array, D2Array, Cube, CubeArray, Buffer };
enum class Primitive { Points, Lines, LineStrip, Triangles, TriangleStrip };
enum class VertexFormat {
    Float,
    Float2,
    Float3,
    Float4,
    Half2,
    Half4,
    UChar4Norm,
    Short2Norm,
    Short4Norm,
    UInt,
    UInt2,
    UInt4,
    Int,
    Int2,
    Int4
};
enum class CompareFunction { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
struct VertexAttribute {
    uint32_t location{}, buffer{}, offset{};
    VertexFormat format{};
    // Optional raw MTLVertexFormat value and its size in bytes (from metal_formats.h);
    // when non-zero it overrides `format`.
    uint32_t raw_format{}, raw_size{};
};
struct VertexBufferLayout {
    uint32_t buffer{}, stride{}; // stride 0: one constant element for every vertex
    bool per_instance{};
    uint32_t step_rate{1};
};
enum class BlendFactor {
    Zero,
    One,
    SourceColor,
    OneMinusSourceColor,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationColor,
    OneMinusDestinationColor,
    DestinationAlpha,
    OneMinusDestinationAlpha,
    SourceAlphaSaturated,
    BlendColor,
    OneMinusBlendColor,
    BlendAlpha,
    OneMinusBlendAlpha,
    Source1Color,
    OneMinusSource1Color,
    Source1Alpha,
    OneMinusSource1Alpha
};
enum class BlendOperation { Add, Subtract, ReverseSubtract, Min, Max };
struct BlendState {
    bool enable{};
    BlendFactor src_rgb{BlendFactor::One}, dst_rgb{BlendFactor::Zero};
    BlendFactor src_alpha{BlendFactor::One}, dst_alpha{BlendFactor::Zero};
    BlendOperation op_rgb{BlendOperation::Add}, op_alpha{BlendOperation::Add};
    uint8_t write_mask{0xF}; // bit 0 red .. bit 3 alpha
};
enum class InputTopology { Unspecified, Point, Line, Triangle };
enum class StencilOperation {
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap
};
struct StencilFaceDesc {
    CompareFunction compare{CompareFunction::Always};
    StencilOperation fail{StencilOperation::Keep}, depth_fail{StencilOperation::Keep},
        pass{StencilOperation::Keep};
    uint32_t read_mask{0xFF}, write_mask{0xFF};
    bool operator==(const StencilFaceDesc&) const = default;
};
struct DepthStencilDesc {
    CompareFunction depth_compare{CompareFunction::Always};
    bool depth_write{};
    bool stencil_enable{};
    StencilFaceDesc front, back;
    bool operator==(const DepthStencilDesc&) const = default;
};
enum class CullMode { None, Front, Back };
enum class Winding { Clockwise, CounterClockwise };
struct Viewport {
    double x{}, y{}, width{1}, height{1}, near_depth{0}, far_depth{1};
};
struct ScissorRect {
    uint32_t x{}, y{}, width{1}, height{1};
};
enum class SamplerFilter { Nearest, Linear };
enum class SamplerMipFilter { None, Nearest, Linear };
enum class SamplerAddress { Repeat, MirrorRepeat, ClampToEdge, ClampToBorder, MirrorClampToEdge };
// Metal samplers only offer these three border colors.
enum class BorderColor { TransparentBlack, OpaqueBlack, OpaqueWhite };
struct SamplerDesc {
    SamplerFilter min_filter{SamplerFilter::Nearest}, mag_filter{SamplerFilter::Nearest};
    SamplerMipFilter mip_filter{SamplerMipFilter::None};
    std::array<SamplerAddress, 3> address{SamplerAddress::ClampToEdge, SamplerAddress::ClampToEdge,
                                          SamplerAddress::ClampToEdge};
    float lod_min{0.0f}, lod_max{1000.0f};
    uint32_t max_anisotropy{1}; // 1..16
    bool compare{};
    CompareFunction compare_function{CompareFunction::Never};
    BorderColor border{BorderColor::TransparentBlack};
    bool normalized_coordinates{true};
};
enum class TextureUsage : uint32_t { Sample = 1, Storage = 2, RenderTarget = 4 };
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

struct ResourceState;
struct SubmissionState;
class Runtime;
class Commands;
struct ShaderSource;
class ArgumentBuffer;

// Handles own native objects. Objective-C stays out of the emulator's C++ headers.
class Buffer {
public:
    Buffer() = default;
    bool Valid() const {
        return state != nullptr;
    }
    size_t Size() const;
    /// Changes whenever CPU/GPU writes are recorded, including texture aliases.
    uint64_t ContentVersion() const;
    uint64_t AllocationId() const;
    void MarkWritten() const;
    void Write(size_t offset, std::span<const std::byte> bytes) const;
    void Read(size_t offset, std::span<std::byte> bytes) const;
    // Staging allocations only: callers must keep the handle alive and wait
    // for GPU completion before reading or writing the returned mapping.
    std::span<std::byte> MappedBytes() const;
    /// CPU mapping without waiting for GPU use. The caller guarantees no in-flight GPU work
    /// touches the bytes it accesses (stream sub-allocations, or after Scheduler::Finish).
    std::byte* UnsafeContents() const;
    bool operator==(const Buffer& other) const {
        return state == other.state;
    }
    /// Stable identity of the underlying allocation (for caches keyed by buffer).
    const void* Identity() const {
        return state.get();
    }

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
    friend class Commands;
    friend class ArgumentBuffer;
};

class Texture {
public:
    Texture() = default;
    // Empty handles stand for absent attachments (for example a gap in guest render targets).
    bool Valid() const {
        return state != nullptr;
    }
    uint32_t Width() const;
    uint32_t Height() const;
    PixelFormat Format() const;
    uint32_t Depth() const;
    uint32_t Levels() const;
    uint32_t Layers() const;
    TextureDimension Dimension() const;
    TextureUsage Usage() const;
    uint32_t SampleCount() const;
    bool operator==(const Texture& other) const {
        return state == other.state;
    }
    const void* Identity() const {
        return state.get();
    }

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
    friend class Commands;
    friend class ArgumentBuffer;
};

struct TextureDesc {
    uint32_t width{1}, height{1}, depth{1}, levels{1}, layers{1};
    TextureDimension dimension{TextureDimension::D2};
    PixelFormat format{PixelFormat::RGBA8};
    TextureUsage usage{TextureUsage::Sample};
};
struct TextureRegion {
    uint32_t level{}, slice{}, x{}, y{}, z{}, width{1}, height{1}, depth{1};
};

class Function {
public:
    Function() = default;

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
};

class DepthStencilState {
public:
    DepthStencilState() = default;
    bool Valid() const {
        return state != nullptr;
    }

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
    friend class Commands;
};

class Pipeline {
public:
    Pipeline() = default;
    bool Valid() const {
        return state != nullptr;
    }

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
    friend class Commands;
};

class Sampler {
public:
    Sampler() = default;
    const void* Identity() const {
        return state.get();
    }

private:
    std::shared_ptr<ResourceState> state;
    friend class Runtime;
    friend class Commands;
    friend class ArgumentBuffer;
};

struct ArgumentWrite {
    enum class Kind : uint8_t { Buffer, Texture, Sampler };
    Kind kind{};
    uint32_t member{};
    const Buffer* buffer{};
    const Texture* texture{};
    const Sampler* sampler{};
    size_t offset{};
    // Conservative default for callers without guest shader access metadata.
    bool is_written{true};
};

class ArgumentBuffer {
public:
    ArgumentBuffer() = default;
    bool Valid() const {
        return layout != nullptr;
    }
    /// True while a recording or an in-flight submission still references the storage.
    /// Writing members of a busy argument buffer would wait (or deadlock on the open
    /// recording), so per-draw users pick a non-busy clone from a pool instead.
    bool Busy() const;
    void SetBuffer(uint32_t member, const Buffer& buffer, size_t offset = 0);
    void SetTexture(uint32_t member, const Texture& texture);
    void SetSampler(uint32_t member, const Sampler& sampler);
    /// Writes many members under one lock and one encoder retarget (per-draw fast path).
    void Encode(std::span<const ArgumentWrite> writes);
    /// Persistent shader bounds metadata, owned by this clone rather than a recycled stream.
    const Buffer& UploadSizeTable(Runtime& runtime, std::span<const uint32_t> sizes);

private:
    Buffer storage;
    Buffer size_table;
    std::shared_ptr<ResourceState> layout;
    uint32_t slot{};
    Stage stage{};
    friend class Runtime;
    friend class Commands;
};

class Submission {
public:
    Submission() = default;
    explicit operator bool() const {
        return state != nullptr;
    }
    void Wait() const;
    bool IsComplete() const;
    uint64_t Serial() const;

private:
    std::shared_ptr<SubmissionState> state;
    friend class Commands;
};

struct ColorAttachment {
    Texture texture; // may be empty: that color index is not bound in this pass
    bool clear{};
    std::array<double, 4> clear_color{};
};
struct RenderPipelineDesc {
    Function vertex;
    Function fragment;
    std::array<PixelFormat, 8> colors{};
    uint32_t color_count{1};
    PixelFormat depth{PixelFormat::Invalid};
    uint32_t sample_count{1};
    bool premultiplied_blend{};
    std::vector<VertexAttribute> attributes;
    std::vector<VertexBufferLayout> vertex_buffers;
    // When set, `blend` describes every color attachment (premultiplied_blend is ignored).
    bool use_blend_states{};
    std::array<BlendState, 8> blend{};
    bool alpha_to_coverage{};
    bool rasterization_enabled{true};
    InputTopology input_topology{InputTopology::Unspecified};
};

// One encoder at a time; switching between render, compute and blit ends the
// previous encoder. Submissions on the same queue preserve guest command order.
class Commands {
public:
    Commands(Commands&&) noexcept;
    Commands& operator=(Commands&&) noexcept;
    ~Commands();
    void BeginRender(std::span<const ColorAttachment> colors, const Texture* depth = nullptr,
                     bool clear_depth = false, double depth_value = 1.0,
                     bool clear_stencil = false, uint32_t stencil_value = 0);
    /// True while the render encoder opened by the last BeginRender is still active.
    bool RenderPassActive() const;
    /// Unique identifier of the render pass begun last (0 before any). Lets callers tell
    /// whether the active pass is still the one they began.
    uint64_t RenderPassId() const;
    /// Render pass without attachments (guest draws with every render target disabled).
    void BeginRenderWithoutAttachments(uint32_t width, uint32_t height);
    /// Visibility (occlusion) result buffer attached to render passes begun after this call
    /// on this recording. Offsets passed to SetVisibilityResult are into this buffer.
    void SetVisibilityBuffer(const Buffer& buffer);
    /// Counts samples passing depth/stencil for following draws at `offset` (8-aligned), or
    /// stops counting. Needs an active pass begun with a visibility buffer.
    void SetVisibilityResult(bool counting, size_t offset);
    void SetDepthStencilState(const DepthStencilState& state);
    void SetCullMode(CullMode mode);
    void SetFrontFacing(Winding winding);
    void SetDepthBias(float bias, float slope_scale, float clamp);
    void SetDepthClamp(bool clamp);
    void SetStencilReference(uint32_t front, uint32_t back);
    void SetBlendColor(float red, float green, float blue, float alpha);
    void SetFillLines(bool lines);
    void SetViewports(std::span<const Viewport> viewports);
    void SetScissors(std::span<const ScissorRect> scissors);
    /// Small inline constant data (setVertexBytes and friends), up to 4 KB.
    void SetBytes(Stage stage, uint32_t slot, std::span<const std::byte> bytes);
    void DrawInstanced(Primitive primitive, uint32_t first, uint32_t count, uint32_t instances,
                       uint32_t base_instance);
    void SetRenderPipeline(const Pipeline& pipeline);
    void SetViewport(double x, double y, double width, double height, double near_depth = 0.0,
                     double far_depth = 1.0);
    void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    void SetBuffer(Stage stage, uint32_t slot, const Buffer& buffer, size_t offset = 0);
    void SetTexture(Stage stage, uint32_t slot, const Texture& texture);
    void SetSampler(Stage stage, uint32_t slot, const Sampler& sampler);
    void SetArguments(Stage stage, const ArgumentBuffer& arguments);
    void Draw(Primitive primitive, uint32_t first, uint32_t count, uint32_t instances = 1);
    void DrawIndexed(Primitive primitive, const Buffer& indices, bool index32, size_t offset,
                     uint32_t count, uint32_t instances = 1, int32_t base_vertex = 0,
                     uint32_t base_instance = 0);
    /// Indirect draws. Argument layouts match Vulkan's VkDraw(Indexed)IndirectCommand.
    void DrawIndirect(Primitive primitive, const Buffer& arguments, size_t offset);
    void DrawIndexedIndirect(Primitive primitive, const Buffer& indices, bool index32,
                             size_t index_offset, const Buffer& arguments, size_t offset);
    void BeginCompute(const Pipeline& pipeline);
    void Dispatch(std::array<uint32_t, 3> groups, std::array<uint32_t, 3> local_size);
    void DispatchIndirect(const Buffer& arguments, size_t offset,
                          std::array<uint32_t, 3> local_size);
    void CopyBuffer(const Buffer& src, size_t src_offset, const Buffer& dst, size_t dst_offset,
                    size_t size);
    void CopyTextureToBuffer(const Texture& src, const Buffer& dst, size_t offset,
                             size_t bytes_per_row);
    void CopyBufferToTexture(const Buffer& src, size_t offset, size_t bytes_per_row,
                             const Texture& dst);
    void CopyBufferToTexture(const Buffer& src, size_t offset, size_t bytes_per_row,
                             size_t bytes_per_image, const Texture& dst,
                             const TextureRegion& region);
    void CopyTextureToBuffer(const Texture& src, const TextureRegion& region, const Buffer& dst,
                             size_t offset, size_t bytes_per_row, size_t bytes_per_image);
    void CopyTexture(const Texture& src, const TextureRegion& region, const Texture& dst,
                     uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t z = 0);
    // Plane-selecting variants for combined depth/stencil textures.
    void CopyBufferToTexture(const Buffer& src, size_t offset, size_t bytes_per_row,
                             size_t bytes_per_image, const Texture& dst,
                             const TextureRegion& region, TransferPlane plane);
    void CopyTextureToBuffer(const Texture& src, const TextureRegion& region, const Buffer& dst,
                             size_t offset, size_t bytes_per_row, size_t bytes_per_image,
                             TransferPlane plane);
    Submission Submit();

private:
    struct Impl;
    explicit Commands(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl;
    friend class Runtime;
};

/// Process-wide Metal counters for the performance log (reset on read).
struct RuntimeStats {
    uint64_t render_passes{};
    uint64_t command_buffers{};
    uint64_t gpu_nanoseconds{};
    uint64_t buffers_created{};
    uint64_t textures_created{};
    uint64_t blit_encoders{};
    uint64_t compute_encoders{};
    uint64_t bindings_skipped{};
};
RuntimeStats TakeRuntimeStats();

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    std::string DeviceName() const;
    Buffer CreateBuffer(size_t size);
    Texture CreateTexture(uint32_t width, uint32_t height, PixelFormat format, TextureUsage usage);
    Texture CreateTexture(const TextureDesc& desc);
    Texture CreateTextureView(const Texture& source, const TextureDesc& view, uint32_t level,
                              uint32_t slice, std::array<uint8_t, 4> swizzle = {2, 3, 4, 5});
    Texture CreateBufferTexture(const Buffer&, size_t offset, uint32_t elements, PixelFormat format,
                                TextureUsage usage);
    Function CompileMSL(std::string_view source, std::string_view entry, Stage stage);
    Pipeline CreateRenderPipeline(const RenderPipelineDesc& desc);
    Pipeline CreateComputePipeline(const Function& function);
    DepthStencilState CreateDepthStencilState(const DepthStencilDesc& desc);
    /// Minimum offset alignment of buffer-backed textures of this format.
    size_t TextureBufferAlignment(PixelFormat format) const;
    Sampler CreateSampler(bool linear = false, bool repeat = false);
    Sampler CreateSampler(const SamplerDesc& desc);
    ArgumentBuffer CreateArguments(const Function& function, const ShaderSource& source,
                                   uint32_t descriptor_set);
    /// New argument buffer with the prototype's layout and fresh, unbound storage.
    ArgumentBuffer CloneArguments(const ArgumentBuffer& prototype);
    Commands BeginCommands(std::string_view label = {});
    std::vector<std::byte> ReadTexture(const Texture& texture);
    // Borrow the frontend's CAMetalLayer. A nil drawable is a recoverable
    // minimized/occluded-window condition, represented by an empty submission.
    Submission Present(void* metal_layer, const Texture& texture);
    /// Presentation with an explicit drawable size, destination rectangle (in drawable
    /// pixels) and normalized source rectangle (u1 < u0 or v1 < v0 flips that axis).
    struct PresentDesc {
        uint32_t drawable_width{}, drawable_height{}; // 0 keeps the layer's drawable size
        double dst_x{}, dst_y{}, dst_width{}, dst_height{}; // dst_width 0: fit, letterboxed
        float u0{0}, v0{0}, u1{1}, v1{1};
        bool linear{true};
    };
    Submission Present(void* metal_layer, const Texture& texture, const PresentDesc& desc);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace NativeMetal
