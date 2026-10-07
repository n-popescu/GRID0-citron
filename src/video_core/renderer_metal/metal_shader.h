// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_metal/metal_runtime.h"

namespace NativeMetal {
constexpr uint32_t InvalidBinding = ~uint32_t{0};
constexpr uint32_t ArgumentBufferBase = 26;
constexpr uint32_t PushConstantBuffer = 28;
// Guest vertex shaders: float4 per-draw fixups (x: clip-space Z offset, see metal_shader.cpp).
constexpr uint32_t VertexFixupBuffer = 29;
constexpr uint32_t BufferSizeBuffer = 30;
enum class ResourceKind {
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    SeparateImage,
    SeparateSampler,
    StorageImage,
    PushConstant,
    // SPIRV-Cross' per-set table of storage-buffer sizes (uint array indexed by member ID).
    BufferSizes
};
struct ResourceBinding {
    ResourceKind kind{};
    uint32_t set{}, binding{}, count{1};
    // Argument-buffer member IDs, except for the discrete push-constant buffer.
    uint32_t buffer{InvalidBinding}, texture{InvalidBinding}, sampler{InvalidBinding};
};
struct ShaderSource {
    Stage stage{};
    std::string entry;
    std::string msl;
    std::vector<ResourceBinding> bindings;
    std::array<bool, 2> argument_sets{};
    bool needs_buffer_sizes{};
    // Member ID of the buffer-size table inside each argument set, or InvalidBinding.
    std::array<uint32_t, 2> buffer_size_member{InvalidBinding, InvalidBinding};
    std::array<uint32_t, 3> workgroup_size{};
};
// `flip_vertex_y` negates the vertex position's Y so Vulkan-convention clip space (+Y down)
// renders upright in Metal (+Y up), as MoltenVK does by default. Guest shaders need it.
ShaderSource CompileSpirv(std::span<const uint32_t> words, std::string_view entry = "main",
                          bool flip_vertex_y = false);
Shader::HostTranslateInfo HostTranslateInfo();
// The existing Maxwell frontend remains shared. Only the host emission/profile
// differs; no Vulkan device, shader module or MoltenVK translation is involved.
ShaderSource EmitGuestShader(Shader::IR::Program& program, const Shader::RuntimeInfo& info,
                             Shader::Backend::Bindings& bindings);
struct GuestShader {
    ShaderSource source;
    Shader::Info info;
};
GuestShader CompileGuestEnvironment(Shader::Environment& env, const Shader::RuntimeInfo& info,
                                    Shader::Backend::Bindings& bindings,
                                    Shader::Environment* vertex_a = nullptr);
} // namespace NativeMetal
