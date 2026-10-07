// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "video_core/renderer_metal/metal_buffer_cache.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
int main() try {
    using namespace NativeMetal;
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    Runtime metal;
    Scheduler scheduler{metal};
    BufferCacheRuntime cache{metal, scheduler};
    CacheBuffer data{cache, 0x10000, 256};
    cache.ClearBuffer(data, 0, 256, 0x12345678);
    auto download = cache.DownloadStagingBuffer(256);
    const std::array copies{VideoCommon::BufferCopy{0, 0, 256}};
    cache.CopyBuffer(download.buffer, data, copies, true);
    bool rejected = false;
    try {
        download.buffer.MappedBytes();
    } catch (const std::exception&) {
        rejected = true;
    }
    Check(rejected, "Mapped staging read accepted unsubmitted GPU writes");
    cache.Finish();
    std::array<u32, 64> values{};
    std::memcpy(values.data(), download.mapped_span.data(), 256);
    for (auto value : values)
        Check(value == 0x12345678, "Guest GPU fill/readback lost data");
    // Real GPU compute using a retained mapped uniform and guest storage buffer.
    const auto uniforms = cache.BindMappedUniformBuffer(0, 0, 16);
    const std::array<u32, 4> parameters{11, 0, 0, 0};
    std::memcpy(uniforms.data(), parameters.data(), 16);
    cache.BindComputeUniformBuffer(0, cache.GraphicsBindings(0).uniforms[0].buffer, 0, 16);
    cache.BindComputeStorageBuffer(0, data, 0, 256, true);
    const auto function = metal.CompileMSL(R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void update(device uint* data [[buffer(0)]], constant uint* parameters [[buffer(1)]],
                           uint id [[thread_position_in_grid]]) { data[id] = id*7+parameters[0]; }
    )",
                                           "update", Stage::Compute);
    const auto pipeline = metal.CreateComputePipeline(function);
    auto& commands = scheduler.Record();
    commands.BeginCompute(pipeline);
    const auto& storage = cache.ComputeBindings().storage[0];
    const auto& uniform = cache.ComputeBindings().uniforms[0];
    commands.SetBuffer(Stage::Compute, 0, storage.buffer, storage.offset);
    commands.SetBuffer(Stage::Compute, 1, uniform.buffer, uniform.offset);
    commands.Dispatch({2, 1, 1}, {32, 1, 1});
    cache.CopyBuffer(download.buffer, data, copies, true);
    scheduler.Flush(); // Buffer retains completion fence, even if token is ignored.
    download.buffer.Read(0, std::as_writable_bytes(std::span{values}));
    for (size_t i = 0; i < values.size(); ++i)
        Check(values[i] == i * 7 + 11, "Guest compute writeback mismatch");
    const std::array overlap{VideoCommon::BufferCopy{0, 4, 60}};
    cache.CopyBuffer(data, data, overlap, true);
    cache.Finish();
    std::array<u32, 16> moved{};
    data.Handle().Read(0, std::as_writable_bytes(std::span{moved}));
    for (size_t i = 1; i < moved.size(); ++i)
        Check(moved[i] == values[i - 1], "Overlapping GPU copy corrupted data");
    const std::array<u8, 6> byte_indices{99, 99, 0, 1, 2, 3};
    auto index_buffer = metal.CreateBuffer(16);
    index_buffer.Write(0, std::as_bytes(std::span{byte_indices}));
    cache.BindIndexBuffer(Maxwell::PrimitiveTopology::Quads, Maxwell::IndexFormat::UnsignedByte, 2,
                          4, index_buffer, 0, 6);
    const auto converted = cache.Index();
    Check(converted.count == 6 && converted.index32, "Guest quad index conversion failed");
    const auto vertex = metal.CompileMSL(R"(
        #include <metal_stdlib>
        using namespace metal;
        vertex float4 guest_quad_vertex(uint id [[vertex_id]]) {
            constexpr float2 positions[] = {float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,1)};
            return float4(positions[id],0,1);
        }
    )",
                                         "guest_quad_vertex", Stage::Vertex);
    const auto fragment = metal.CompileMSL(R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 color() { return float4(0,1,0,1); }
    )",
                                           "color", Stage::Fragment);
    RenderPipelineDesc desc;
    desc.vertex = vertex;
    desc.fragment = fragment;
    desc.colors[0] = PixelFormat::RGBA8;
    const auto graphics = metal.CreateRenderPipeline(desc);
    const auto target = metal.CreateTexture(32, 32, PixelFormat::RGBA8, TextureUsage::RenderTarget);
    const std::array colors{ColorAttachment{target, true, {0, 0, 0, 1}}};
    auto& draw = scheduler.Record();
    draw.BeginRender(colors);
    draw.SetRenderPipeline(graphics);
    draw.DrawIndexed(converted.primitive, converted.buffer, converted.index32, converted.offset,
                     converted.count);
    scheduler.Finish();
    const auto pixels = metal.ReadTexture(target);
    for (size_t i = 0; i < pixels.size(); i += 4)
        Check(pixels[i] == std::byte{0} && pixels[i + 1] == std::byte{255} &&
                  pixels[i + 2] == std::byte{0},
              "Guest converted quad did not fill render target");
    std::cout << "PASS: Metal guest buffer upload/fill, compute writeback, retained uniforms, "
                 "overlapping copy, uint8/quad conversion and native indexed rendering\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
