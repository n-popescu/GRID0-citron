// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "video_core/renderer_metal/metal_runtime.h"
#include "video_core/renderer_metal/metal_shader.h"

using namespace NativeMetal;
namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F>
void Reject(F&& fn, const char* message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Check(file.good(), "Could not open SPIR-V fixture");
    const auto size = file.tellg();
    Check(size > 0 && size % 4 == 0 && size <= 16 * 1024 * 1024, "Invalid SPIR-V fixture size");
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), size);
    Check(file.good(), "Could not read SPIR-V fixture");
    return words;
}
constexpr std::string_view shader = R"MSL(
#include <metal_stdlib>
using namespace metal;
vertex float4 near_vertex(uint i [[vertex_id]]) {
    float2 p = i == 0 ? float2(-1,-1) : i == 1 ? float2(3,-1) : float2(-1,3);
    return float4(p,.5,1);
}
vertex float4 far_vertex(uint i [[vertex_id]]) {
    float2 p = i == 0 ? float2(-1,-1) : i == 1 ? float2(3,-1) : float2(-1,3);
    return float4(p,.75,1);
}
fragment float4 red() { return float4(1,0,0,1); }
fragment float4 green() { return float4(0,1,0,1); }
struct Outputs { float4 a [[color(0)]]; float4 b [[color(1)]]; };
fragment Outputs mrt() { return {float4(1,0,0,1),float4(0,0,1,1)}; }
)MSL";
void CheckPixel(const std::vector<std::byte>& data, size_t pixel, std::array<uint8_t, 4> expected) {
    for (size_t i = 0; i < 4; ++i)
        Check(std::to_integer<uint8_t>(data.at(pixel * 4 + i)) == expected[i],
              "Metal pixel readback mismatch");
}
void RenderTests(Runtime& runtime) {
    auto target = runtime.CreateTexture(64, 64, PixelFormat::RGBA8,
                                        TextureUsage::RenderTarget | TextureUsage::Sample);
    auto depth =
        runtime.CreateTexture(64, 64, PixelFormat::Depth32Float, TextureUsage::RenderTarget);
    const std::array colors{ColorAttachment{target, true, {0, 0, 0, 1}}};
    RenderPipelineDesc desc;
    desc.colors[0] = PixelFormat::RGBA8;
    desc.depth = PixelFormat::Depth32Float;
    desc.vertex = runtime.CompileMSL(shader, "near_vertex", Stage::Vertex);
    desc.fragment = runtime.CompileMSL(shader, "red", Stage::Fragment);
    const auto near_pipeline = runtime.CreateRenderPipeline(desc);
    desc.vertex = runtime.CompileMSL(shader, "far_vertex", Stage::Vertex);
    desc.fragment = runtime.CompileMSL(shader, "green", Stage::Fragment);
    const auto far_pipeline = runtime.CreateRenderPipeline(desc);
    auto indices = runtime.CreateBuffer(16);
    const std::array<uint16_t, 3> index_data{0, 1, 2};
    indices.Write(0, std::as_bytes(std::span{index_data}));
    {
        auto commands = runtime.BeginCommands("Native Metal indexed/scissor/depth regression");
        commands.BeginRender(colors, &depth, true);
        commands.SetScissor(16, 16, 32, 32);
        commands.SetRenderPipeline(near_pipeline);
        commands.DrawIndexed(Primitive::Triangles, indices, false, 0, 3);
        commands.SetRenderPipeline(far_pipeline);
        commands.Draw(Primitive::Triangles, 0, 3);
        commands.Submit().Wait();
    }
    auto pixels = runtime.ReadTexture(target);
    CheckPixel(pixels, 32 * 64 + 32, {255, 0, 0, 255});
    CheckPixel(pixels, 0, {0, 0, 0, 255});
    auto second = runtime.CreateTexture(64, 64, PixelFormat::BGRA8, TextureUsage::RenderTarget);
    desc.vertex = runtime.CompileMSL(shader, "near_vertex", Stage::Vertex);
    desc.fragment = runtime.CompileMSL(shader, "mrt", Stage::Fragment);
    desc.depth = PixelFormat::Invalid;
    desc.color_count = 2;
    desc.colors[1] = PixelFormat::BGRA8;
    const auto pipeline = runtime.CreateRenderPipeline(desc);
    const std::array mrt_colors{ColorAttachment{target, true, {}},
                                ColorAttachment{second, true, {}}};
    auto commands = runtime.BeginCommands("Native Metal multiple render targets");
    commands.BeginRender(mrt_colors);
    commands.SetRenderPipeline(pipeline);
    commands.Draw(Primitive::Triangles, 0, 3);
    commands.Submit().Wait();
    CheckPixel(runtime.ReadTexture(target), 0, {255, 0, 0, 255});
    CheckPixel(runtime.ReadTexture(second), 0, {255, 0, 0, 255}); // blue, native BGRA byte order
    {
        auto depth_only = runtime.BeginCommands("Native Metal depth-only clear");
        depth_only.BeginRender({}, &depth, true, 0.25);
        depth_only.Submit().Wait();
    }
    const auto depth_pixels = runtime.ReadTexture(depth);
    float depth_value{};
    std::memcpy(&depth_value, depth_pixels.data(), sizeof(depth_value));
    Check(depth_value == 0.25f, "Depth-only clear/readback mismatch");
    std::cout << "PASS: indexed drawing, scissor, depth rejection and multiple render targets\n";
}
void MemoryTests(Runtime& runtime) {
    constexpr uint32_t width = 65, height = 7;
    constexpr size_t pitch = 512;
    auto source = runtime.CreateBuffer(pitch * height);
    auto dest = runtime.CreateBuffer(pitch * height);
    auto texture = runtime.CreateTexture(width, height, PixelFormat::RGBA8, TextureUsage::Sample);
    std::vector<std::byte> input(pitch * height);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            auto p = input.data() + y * pitch + x * 4;
            p[0] = std::byte(x);
            p[1] = std::byte(y);
            p[2] = std::byte(77);
            p[3] = std::byte(255);
        }
    source.Write(0, input);
    {
        auto commands = runtime.BeginCommands("Native Metal ordered upload and readback");
        Reject([&] { runtime.BeginCommands(); }, "Overlapping command recording was accepted");
        commands.CopyBufferToTexture(source, 0, pitch, texture);
        Reject([&] { source.Write(0, input); }, "CPU write raced unsubmitted GPU commands");
        commands.CopyTextureToBuffer(texture, dest, 0, pitch);
        commands.Submit(); // Buffer retains its own completion fence.
    }
    std::vector<std::byte> output(pitch * height);
    dest.Read(0, output);
    for (uint32_t y = 0; y < height; ++y)
        Check(std::equal(input.begin() + y * pitch, input.begin() + y * pitch + width * 4,
                         output.begin() + y * pitch),
              "Texture upload/readback row mismatch");
    Reject([&] { dest.Read(dest.Size() - 1, std::span<std::byte>{output.data(), 4}); },
           "Out-of-bounds buffer read was accepted");
    {
        auto abandoned = runtime.BeginCommands();
        abandoned.CopyBuffer(source, 0, dest, 0, 16);
    }
    source.Write(
        0, std::span<const std::byte>{input.data(), 16}); // cancelled recording releases ownership
    std::cout << "PASS: aligned texture transfer, ordered GPU ownership, discarded fence and "
                 "cancellation\n";
}
void SpirvTests(Runtime& runtime, const std::filesystem::path& fixtures) {
    const auto compute_source = CompileSpirv(ReadSpirv(fixtures / "storage.comp.spv"));
    std::ofstream(fixtures / "storage.generated.msl") << compute_source.msl;
    Check(compute_source.workgroup_size == std::array<uint32_t, 3>{32, 1, 1},
          "Compute local size reflection mismatch");
    auto compute =
        runtime.CompileMSL(compute_source.msl, compute_source.entry, compute_source.stage);
    auto pipeline = runtime.CreateComputePipeline(compute);
    auto buffer = runtime.CreateBuffer(64 * sizeof(uint32_t));
    auto push = runtime.CreateBuffer(16);
    const std::array<uint32_t, 4> push_values{17, 0, 0, 0};
    push.Write(0, std::as_bytes(std::span{push_values}));
    auto arguments = runtime.CreateArguments(compute, compute_source, 1);
    Check(compute_source.bindings.size() == 2, "Compute descriptor reflection mismatch");
    for (const auto& binding : compute_source.bindings)
        if (binding.kind == ResourceKind::StorageBuffer)
            arguments.SetBuffer(binding.buffer, buffer);
    {
        auto commands = runtime.BeginCommands("SPIR-V to native Metal compute");
        commands.BeginCompute(pipeline);
        commands.SetArguments(Stage::Compute, arguments);
        commands.SetBuffer(Stage::Compute, PushConstantBuffer, push);
        commands.Dispatch({2, 1, 1}, compute_source.workgroup_size);
        commands.Submit().Wait();
    }
    std::array<uint32_t, 64> values{};
    buffer.Read(0, std::as_writable_bytes(std::span{values}));
    for (uint32_t i = 0; i < values.size(); ++i)
        Check(values[i] == i * 3 + 17, "Translated Metal compute result mismatch");
    const auto vs_source = CompileSpirv(ReadSpirv(fixtures / "fullscreen.vert.spv"));
    const auto fs_source = CompileSpirv(ReadSpirv(fixtures / "sampled-array.frag.spv"));
    auto vs = runtime.CompileMSL(vs_source.msl, vs_source.entry, vs_source.stage);
    auto fs = runtime.CompileMSL(fs_source.msl, fs_source.entry, fs_source.stage);
    auto sampled_args = runtime.CreateArguments(fs, fs_source, 1);
    auto sampled = runtime.CreateTexture(2, 2, PixelFormat::RGBA8, TextureUsage::Sample);
    auto upload = runtime.CreateBuffer(512);
    std::vector<std::byte> bytes(512);
    for (size_t row = 0; row < 2; ++row)
        for (size_t x = 0; x < 2; ++x) {
            const std::array<uint8_t, 4> rgba{32, 64, 128, 255};
            for (size_t c = 0; c < 4; ++c)
                bytes[row * 256 + x * 4 + c] = std::byte(rgba[c]);
        }
    upload.Write(0, bytes);
    auto sampler = runtime.CreateSampler();
    for (const auto& binding : fs_source.bindings) {
        Check(binding.kind == ResourceKind::SampledImage && binding.count == 2,
              "Sampled array reflection mismatch");
        for (uint32_t i = 0; i < binding.count; ++i) {
            sampled_args.SetTexture(binding.texture + i, sampled);
            sampled_args.SetSampler(binding.sampler + i, sampler);
        }
    }
    auto target = runtime.CreateTexture(64, 64, PixelFormat::RGBA8, TextureUsage::RenderTarget);
    RenderPipelineDesc desc;
    desc.vertex = vs;
    desc.fragment = fs;
    desc.colors[0] = PixelFormat::RGBA8;
    auto render_pipeline = runtime.CreateRenderPipeline(desc);
    auto commands = runtime.BeginCommands("SPIR-V native Metal sampled array draw");
    commands.CopyBufferToTexture(upload, 0, 256, sampled);
    const std::array colors{ColorAttachment{target, true, {}}};
    commands.BeginRender(colors);
    commands.SetRenderPipeline(render_pipeline);
    commands.SetArguments(Stage::Fragment, sampled_args);
    commands.Draw(Primitive::Triangles, 0, 3);
    commands.Submit().Wait();
    const auto pixels = runtime.ReadTexture(target);
    CheckPixel(pixels, 0, {32, 64, 128, 255});
    CheckPixel(pixels, 32 * 64 + 32, {32, 64, 128, 255});
    const auto attr_source = CompileSpirv(ReadSpirv(fixtures / "attributes.vert.spv"));
    desc.vertex = runtime.CompileMSL(attr_source.msl, attr_source.entry, attr_source.stage);
    desc.attributes = {{0, 0, 0, VertexFormat::Float2}, {1, 0, 8, VertexFormat::Float2}};
    desc.vertex_buffers = {{0, 16, false, 1}};
    const auto attr_pipeline = runtime.CreateRenderPipeline(desc);
    const std::array<float, 12> vertex_values{-1, -1, 0, 0, 3, -1, 2, 0, -1, 3, 0, 2};
    auto vertex_buffer = runtime.CreateBuffer(sizeof(vertex_values));
    vertex_buffer.Write(0, std::as_bytes(std::span{vertex_values}));
    auto attr_commands = runtime.BeginCommands("Native Metal translated vertex attributes");
    attr_commands.BeginRender(colors);
    attr_commands.SetRenderPipeline(attr_pipeline);
    attr_commands.SetBuffer(Stage::Vertex, 0, vertex_buffer);
    attr_commands.SetArguments(Stage::Fragment, sampled_args);
    attr_commands.Draw(Primitive::Triangles, 0, 3);
    attr_commands.Submit().Wait();
    CheckPixel(runtime.ReadTexture(target), 32 * 64 + 32, {32, 64, 128, 255});
    std::cout << "PASS: SPIR-V native compute, push constants, tier-2 texture/sampler array and "
                 "graphics\n";
}
void TextureSubresourceTests(Runtime& runtime) {
    TextureDesc desc;
    desc.width = 16;
    desc.height = 8;
    desc.levels = 3;
    desc.layers = 3;
    desc.dimension = TextureDimension::D2Array;
    desc.usage = TextureUsage::Sample | TextureUsage::Storage;
    const auto texture = runtime.CreateTexture(desc);
    auto upload = runtime.CreateBuffer(256 * 4);
    std::vector<std::byte> bytes(256 * 4);
    for (size_t y = 0; y < 4; ++y)
        for (size_t x = 0; x < 8; ++x) {
            bytes[y * 256 + x * 4] = std::byte{23};
            bytes[y * 256 + x * 4 + 1] = std::byte{47};
            bytes[y * 256 + x * 4 + 2] = std::byte{89};
            bytes[y * 256 + x * 4 + 3] = std::byte{255};
        }
    upload.Write(0, bytes);
    auto commands = runtime.BeginCommands("Metal mip and array-layer upload");
    commands.CopyBufferToTexture(upload, 0, 256, 1024, texture, {1, 2, 0, 0, 0, 8, 4, 1});
    commands.Submit().Wait();
    TextureDesc view_desc = desc;
    view_desc.width = 8;
    view_desc.height = 4;
    view_desc.levels = 1;
    view_desc.layers = 1;
    view_desc.dimension = TextureDimension::D2;
    const auto view = runtime.CreateTextureView(texture, view_desc, 1, 2);
    CheckPixel(runtime.ReadTexture(view), 0, {23, 47, 89, 255});
    auto destination = runtime.CreateTexture(8, 4, PixelFormat::RGBA8, TextureUsage::Sample);
    auto copy = runtime.BeginCommands("Metal subresource-to-image copy");
    copy.CopyTexture(texture, {1, 2, 0, 0, 0, 8, 4, 1}, destination, 0, 0, 0, 0);
    copy.Submit().Wait();
    CheckPixel(runtime.ReadTexture(destination), 31, {23, 47, 89, 255});
    // Exercise swizzle through shader access; raw blit readback does not apply it.
    auto swizzled = runtime.CreateTextureView(texture, view_desc, 1, 2, {4, 3, 2, 5});
    auto results = runtime.CreateBuffer(16);
    const auto sample = runtime.CompileMSL(R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void read_view(texture2d<float,access::read> image [[texture(0)]],
                              device float4* output [[buffer(0)]]) { output[0]=image.read(uint2(0)); }
    )",
                                           "read_view", Stage::Compute);
    auto compute = runtime.BeginCommands("Metal swizzled mip view shader access");
    compute.BeginCompute(runtime.CreateComputePipeline(sample));
    compute.SetTexture(Stage::Compute, 0, swizzled);
    compute.SetBuffer(Stage::Compute, 0, results);
    compute.Dispatch({1, 1, 1}, {1, 1, 1});
    compute.Submit().Wait();
    std::array<float, 4> color{};
    results.Read(0, std::as_writable_bytes(std::span{color}));
    Check(std::abs(color[0] - 89.f / 255) < 0.0001f && std::abs(color[2] - 23.f / 255) < 0.0001f,
          "Metal swizzled view lost channels");
    auto backing = runtime.CreateBuffer(256);
    auto texel_view = runtime.CreateBufferTexture(backing, 0, 64, PixelFormat::R32Uint,
                                                  TextureUsage::Sample | TextureUsage::Storage);
    const auto writer = runtime.CompileMSL(R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void write_texel(texture_buffer<uint,access::write> image [[texture(0)]],
                                uint id [[thread_position_in_grid]]) { image.write(uint4(id+71),id); }
    )",
                                           "write_texel", Stage::Compute);
    auto texel_commands = runtime.BeginCommands("Metal texel buffer alias writeback");
    texel_commands.BeginCompute(runtime.CreateComputePipeline(writer));
    texel_commands.SetTexture(Stage::Compute, 0, texel_view);
    texel_commands.Dispatch({2, 1, 1}, {32, 1, 1});
    std::array<uint32_t, 64> values{};
    Reject([&] { backing.Read(0, std::as_writable_bytes(std::span{values})); },
           "Texel view did not retain backing-buffer ownership");
    texel_commands.Submit();
    backing.Read(0, std::as_writable_bytes(std::span{values}));
    for (size_t i = 0; i < values.size(); ++i)
        Check(values[i] == i + 71, "Texel buffer writeback mismatch");
    Reject([&] { runtime.CreateTextureView(texture, view_desc, 3, 2); },
           "Out-of-bounds mip view was accepted");
    std::cout << "PASS: mip/array transfer, texture views/swizzle, image copy and texel-buffer "
                 "alias ownership\n";
}
} // namespace
int main(int argc, char** argv) try {
    if (argc == 4 && std::string_view(argv[1]) == "--translate-spv") {
        const auto source = CompileSpirv(ReadSpirv(argv[2]));
        std::ofstream file(argv[3]);
        file << source.msl;
        Check(file.good(), "Could not write translated Metal shader");
        for (const auto& binding : source.bindings)
            std::cout << "set=" << binding.set << " binding=" << binding.binding
                      << " count=" << binding.count << " buffer=" << binding.buffer
                      << " texture=" << binding.texture << " sampler=" << binding.sampler << '\n';
        return 0;
    }
    Runtime runtime;
    std::cout << "Native Metal device: " << runtime.DeviceName() << '\n';
    if (argc > 2 && std::string_view(argv[1]) == "--compile-spv") {
        size_t success = 0, failures = 0;
        for (int i = 2; i < argc; ++i)
            try {
                const auto source = CompileSpirv(ReadSpirv(argv[i]));
                runtime.CompileMSL(source.msl, source.entry, source.stage);
                std::cout << "PASS " << argv[i] << " resources=" << source.bindings.size() << '\n';
                ++success;
            } catch (const std::exception& error) {
                std::cout << "FAIL " << argv[i] << ": " << error.what() << '\n';
                ++failures;
            }
        std::cout << "Shader compilation: " << success << " passed, " << failures << " failed\n";
        return failures ? 1 : 0;
    }
    Check(argc == 2, "Usage: citrosis-metal-smoke <fixture directory> | --compile-spv <files...>");
    MemoryTests(runtime);
    RenderTests(runtime);
    SpirvTests(runtime, argv[1]);
    TextureSubresourceTests(runtime);
    Reject([&] { runtime.CompileMSL("this is invalid Metal", "missing", Stage::Compute); },
           "Invalid shader was accepted");
    std::cout << "PASS: native Metal hardware checks completed without Vulkan/MoltenVK\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
