// SPDX-License-Identifier: GPL-2.0-or-later
// Replay captured Maxwell compute environments through the Metal host profile.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include "video_core/renderer_metal/metal_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

int main(int argc, char** argv) try {
    if (argc != 3)
        throw std::runtime_error(
            "Usage: citrosis-metal-guest-smoke <captured cache> <disposable copy>");
    const std::filesystem::path input = argv[1], copy = argv[2];
    if (std::filesystem::absolute(input).lexically_normal() ==
        std::filesystem::absolute(copy).lexically_normal())
        throw std::runtime_error("Replay copy must differ from the original cache");
    if (std::filesystem::exists(copy) && std::filesystem::equivalent(input, copy))
        throw std::runtime_error("Replay copy aliases the original cache");
    std::filesystem::copy_file(input, copy, std::filesystem::copy_options::overwrite_existing);
    NativeMetal::Runtime runtime;
    std::stop_source stop;
    size_t passed = 0, failed = 0;
    VideoCommon::LoadPipelines(
        stop.get_token(), copy, 15,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            const char* phase = "Maxwell translation";
            try {
                Shader::Backend::Bindings bindings;
                const auto shader = NativeMetal::CompileGuestEnvironment(env, {}, bindings);
                phase = "MSL compilation";
                std::ofstream msl(copy.parent_path() /
                                  ("compute-" + std::to_string(key.unique_hash) + ".metal"));
                msl << shader.source.msl;
                const auto fn =
                    runtime.CompileMSL(shader.source.msl, shader.source.entry, shader.source.stage);
                phase = "compute pipeline creation";
                runtime.CreateComputePipeline(fn);
                ++passed;
                std::cout << "PASS Maxwell compute " << std::hex << key.unique_hash << std::dec
                          << '\n';
            } catch (const std::exception& error) {
                ++failed;
                std::cout << "FAIL Maxwell compute " << std::hex << key.unique_hash << std::dec
                          << " during " << phase << ": " << error.what() << '\n';
            }
            if (passed + failed >= 32)
                stop.request_stop();
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment>) {
            // Vulkan keys are only input capture metadata here; no Vulkan API
            // is invoked. Raster/attribute state replay is a separate test.
            file.seekg(sizeof(Vulkan::GraphicsPipelineCacheKey), std::ios::cur);
        });
    std::cout << "Maxwell-to-Metal compute: " << passed << " passed, " << failed << " failed\n";
    return passed > 0 && failed == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
