// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <spirv_msl.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_metal/metal_shader.h"

namespace NativeMetal {
namespace {
// Guest vertex shaders get two host fixups:
//  * [[position, invariant]]: depth pre-passes and later passes with EQUAL/LEQUAL depth tests
//    must compute bit-identical positions (with preserveInvariance in the compile options),
//    or surfaces speckle where the test randomly fails.
//  * A per-draw clip-space Z offset (buffer VertexFixupBuffer, .x * w): guest D24 depth
//    buffers are emulated with Depth32Float, whose depth-bias unit depends on the depth value;
//    the constant part of the guest bias is applied here in D24 UNORM units instead (as
//    VK_EXT_depth_bias_control's FORCE_UNORM does), avoiding shadow acne.
void PatchGuestVertexShader(std::string& msl, const std::string& entry) {
    if (const auto at = msl.find("[[position]]"); at != std::string::npos)
        msl.replace(at, 12, "[[position, invariant]]");
    const std::string signature = " " + entry + "(";
    const auto name = msl.find(signature);
    if (name == std::string::npos || msl.rfind("vertex ", name) == std::string::npos)
        return;
    size_t depth = 0;
    size_t close = std::string::npos;
    for (size_t i = name + signature.size() - 1; i < msl.size(); ++i) {
        if (msl[i] == '(')
            ++depth;
        else if (msl[i] == ')' && --depth == 0) {
            close = i;
            break;
        }
    }
    if (close == std::string::npos)
        return;
    const auto body = msl.find('{', close);
    if (body == std::string::npos)
        return;
    // Only patch functions that return the position through `out`.
    const std::string ret = "return out;";
    if (msl.find("out.gl_Position", body) == std::string::npos ||
        msl.find(ret, body) == std::string::npos)
        return;
    const bool empty = msl.find_first_not_of(" \n\t", name + signature.size()) == close;
    const std::string param = std::string(empty ? "" : ", ") +
                              "constant float4& citrosisVertexFixup [[buffer(" +
                              std::to_string(VertexFixupBuffer) + ")]]";
    msl.insert(close, param);
    const std::string fix = "out.gl_Position.z += citrosisVertexFixup.x * out.gl_Position.w;\n    ";
    for (size_t at = msl.find(ret, body); at != std::string::npos;
         at = msl.find(ret, at + fix.size() + ret.size()))
        msl.insert(at, fix);
}
} // namespace

ShaderSource CompileSpirv(std::span<const uint32_t> words, std::string_view entry,
                          bool flip_vertex_y) {
    if (words.size() < 5 || words.front() != 0x07230203)
        throw std::invalid_argument("Invalid SPIR-V header for Metal translation");
    spirv_cross::CompilerMSL compiler(words.data(), words.size());
    const auto entries = compiler.get_entry_points_and_stages();
    const auto selected = std::find_if(entries.begin(), entries.end(),
                                       [&](const auto& item) { return item.name == entry; });
    if (selected == entries.end())
        throw std::runtime_error("SPIR-V entry point was not found");
    const auto model = selected->execution_model;
    ShaderSource result;
    switch (model) {
    case spv::ExecutionModelVertex:
        result.stage = Stage::Vertex;
        break;
    case spv::ExecutionModelFragment:
        result.stage = Stage::Fragment;
        break;
    case spv::ExecutionModelGLCompute:
        result.stage = Stage::Compute;
        break;
    default:
        throw std::runtime_error("Metal guest geometry/tessellation emulation is not implemented");
    }
    compiler.set_entry_point(std::string(entry), model);
    compiler.set_enabled_interface_variables(compiler.get_active_interface_variables());
    auto options = compiler.get_msl_options();
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.set_msl_version(3, 0);
    options.argument_buffers = true;
    options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
    options.buffer_size_buffer_index = BufferSizeBuffer;
    options.texture_buffer_native = true;
    options.fixed_subgroup_size = 32;
    compiler.set_msl_options(options);
    if (flip_vertex_y && model == spv::ExecutionModelVertex) {
        auto common = compiler.get_common_options();
        common.vertex.flip_vert_y = true;
        compiler.set_common_options(common);
    }
    const auto resources = compiler.get_shader_resources();
    struct Item {
        spirv_cross::Resource resource;
        ResourceKind kind;
    };
    std::vector<Item> items;
    const auto append = [&](const auto& list, ResourceKind kind) {
        for (const auto& resource : list)
            items.push_back({resource, kind});
    };
    append(resources.uniform_buffers, ResourceKind::UniformBuffer);
    append(resources.storage_buffers, ResourceKind::StorageBuffer);
    append(resources.sampled_images, ResourceKind::SampledImage);
    append(resources.separate_images, ResourceKind::SeparateImage);
    append(resources.separate_samplers, ResourceKind::SeparateSampler);
    append(resources.storage_images, ResourceKind::StorageImage);
    append(resources.push_constant_buffers, ResourceKind::PushConstant);
    if (!resources.subpass_inputs.empty() || !resources.atomic_counters.empty())
        throw std::runtime_error(
            "Metal input-attachment/atomic-counter lowering is not implemented");
    const auto location = [&](const auto& item) {
        return std::tuple{compiler.get_decoration(item.resource.id, spv::DecorationDescriptorSet),
                          compiler.get_decoration(item.resource.id, spv::DecorationBinding)};
    };
    std::stable_sort(items.begin(), items.end(),
                     [&](const auto& a, const auto& b) { return location(a) < location(b); });
    std::array<uint32_t, 2> next_id{};
    std::map<std::pair<uint32_t, uint32_t>, ResourceBinding> descriptor_bindings;
    for (const auto& item : items) {
        ResourceBinding binding;
        binding.kind = item.kind;
        binding.set = compiler.get_decoration(item.resource.id, spv::DecorationDescriptorSet);
        binding.binding = compiler.get_decoration(item.resource.id, spv::DecorationBinding);
        const auto& type = compiler.get_type(item.resource.type_id);
        for (size_t i = 0; i < type.array.size(); ++i) {
            if (!type.array_size_literal[i] || type.array[i] == 0 ||
                type.array[i] > 65536 / binding.count)
                throw std::runtime_error("Metal needs a bounded descriptor array");
            binding.count *= type.array[i];
        }
        if (item.kind != ResourceKind::PushConstant) {
            const auto found = descriptor_bindings.find({binding.set, binding.binding});
            if (found != descriptor_bindings.end()) {
                if (found->second.kind != binding.kind || found->second.count != binding.count)
                    throw std::runtime_error("Incompatible Metal descriptor aliases");
                // SPIRV-Cross maps by set/binding, not SPIR-V variable ID. Typed
                // aliases must share one member ID and one runtime descriptor.
                continue;
            }
        }
        spirv_cross::MSLResourceBinding mapping;
        mapping.stage = model;
        mapping.desc_set = binding.set;
        mapping.binding = binding.binding;
        mapping.count = binding.count;
        if (item.kind == ResourceKind::PushConstant) {
            mapping.desc_set = spirv_cross::kPushConstDescSet;
            mapping.binding = spirv_cross::kPushConstBinding;
            mapping.msl_buffer = binding.buffer = PushConstantBuffer;
        } else {
            if (binding.set >= next_id.size())
                throw std::runtime_error("Metal shader descriptor set exceeds the guest layout");
            auto& next = next_id[binding.set];
            result.argument_sets[binding.set] = true;
            switch (item.kind) {
            case ResourceKind::UniformBuffer:
            case ResourceKind::StorageBuffer:
                mapping.msl_buffer = binding.buffer = next;
                next += binding.count;
                break;
            case ResourceKind::SeparateSampler:
                mapping.msl_sampler = binding.sampler = next;
                next += binding.count;
                break;
            case ResourceKind::SampledImage:
                mapping.msl_texture = binding.texture = next;
                next += binding.count;
                mapping.msl_sampler = binding.sampler = next;
                next += binding.count;
                break;
            case ResourceKind::SeparateImage:
            case ResourceKind::StorageImage:
                mapping.msl_texture = binding.texture = next;
                next += binding.count;
                break;
            default:
                break;
            }
        }
        compiler.add_msl_resource_binding(mapping);
        if (item.kind != ResourceKind::PushConstant)
            descriptor_bindings.emplace(std::pair{binding.set, binding.binding}, binding);
        result.bindings.push_back(binding);
    }
    // Storage buffers whose length the shader queries get their sizes from a table
    // SPIRV-Cross adds to the set's argument buffer. Reserve a member ID after the set's
    // resources; whether the table is really emitted is only known after compilation.
    std::array<uint32_t, 2> size_member{InvalidBinding, InvalidBinding};
    for (uint32_t set = 0; set < result.argument_sets.size(); ++set) {
        if (!result.argument_sets[set])
            continue;
        spirv_cross::MSLResourceBinding mapping;
        mapping.stage = model;
        mapping.desc_set = set;
        mapping.binding = spirv_cross::kBufferSizeBufferBinding;
        mapping.count = 1;
        mapping.msl_buffer = size_member[set] = next_id[set];
        compiler.add_msl_resource_binding(mapping);
    }
    for (uint32_t set = 0; set < result.argument_sets.size(); ++set) {
        if (!result.argument_sets[set])
            continue;
        spirv_cross::MSLResourceBinding mapping;
        mapping.stage = model;
        mapping.desc_set = set;
        mapping.binding = spirv_cross::kArgumentBufferBinding;
        mapping.msl_buffer = ArgumentBufferBase + set;
        compiler.add_msl_resource_binding(mapping);
    }
    result.msl = compiler.compile();
    result.entry = compiler.get_cleansed_entry_point_name(std::string(entry), model);
    if (flip_vertex_y && model == spv::ExecutionModelVertex)
        PatchGuestVertexShader(result.msl, result.entry);
    result.needs_buffer_sizes = compiler.needs_buffer_size_buffer();
    for (uint32_t set = 0; set < result.argument_sets.size(); ++set) {
        if (size_member[set] == InvalidBinding)
            continue;
        // Find the set's argument struct and check whether it carries the size table.
        const std::string header = "struct spvDescriptorSetBuffer" + std::to_string(set);
        const auto begin = result.msl.find(header);
        if (begin == std::string::npos)
            continue;
        const auto end = result.msl.find("};", begin);
        const auto table = result.msl.find("spvBufferSizeConstants", begin);
        if (table == std::string::npos || table > end)
            continue;
        result.buffer_size_member[set] = size_member[set];
        ResourceBinding binding;
        binding.kind = ResourceKind::BufferSizes;
        binding.set = set;
        binding.binding = spirv_cross::kBufferSizeBufferBinding;
        binding.buffer = size_member[set];
        result.bindings.push_back(binding);
    }
    if (result.stage == Stage::Compute) {
        for (uint32_t axis = 0; axis < 3; ++axis)
            result.workgroup_size[axis] =
                compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
    }
    return result;
}
Shader::HostTranslateInfo HostTranslateInfo() {
    return {.support_float64 = false,
            .support_float16 = true,
            .support_int64 = true,
            .needs_demote_reorder = true,
            .support_snorm_render_buffer = true,
            .support_viewport_index_layer = true,
            .min_ssbo_alignment = 16,
            .support_geometry_shader_passthrough = false,
            .support_conditional_barrier = false};
}
ShaderSource EmitGuestShader(Shader::IR::Program& program, const Shader::RuntimeInfo& info,
                             Shader::Backend::Bindings& bindings) {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    profile.has_split_descriptor_sets = true;
    profile.support_descriptor_aliasing = true;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    profile.support_vertex_instance_id = true;
    profile.inline_precise_fp32 = true;
    profile.support_vote = true;
    profile.support_viewport_index_layer_non_geometry = true;
    profile.support_derivative_control = true;
    profile.min_ssbo_alignment = 16;
    profile.max_user_clip_distances = 8;
    return CompileSpirv(Shader::Backend::SPIRV::EmitSPIRV(profile, info, program, bindings), "main",
                        true);
}
GuestShader CompileGuestEnvironment(Shader::Environment& env, const Shader::RuntimeInfo& info,
                                    Shader::Backend::Bindings& bindings,
                                    Shader::Environment* vertex_a) {
    const auto stage = env.ShaderStage();
    if (stage != Shader::Stage::VertexB && stage != Shader::Stage::Fragment &&
        stage != Shader::Stage::Compute)
        throw std::runtime_error("Metal guest geometry/tessellation emulation is not implemented");
    if (vertex_a &&
        (stage != Shader::Stage::VertexB || vertex_a->ShaderStage() != Shader::Stage::VertexA))
        throw std::invalid_argument("Invalid Metal dual-vertex shader environments");
    Shader::ObjectPool<Shader::IR::Inst> instructions;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    const auto translate = [&](Shader::Environment& environment, bool is_vertex_a) {
        const auto header_size = environment.ShaderStage() == Shader::Stage::Compute
                                     ? 0u
                                     : static_cast<uint32_t>(sizeof(Shader::ProgramHeader));
        if (environment.StartAddress() > std::numeric_limits<uint32_t>::max() - header_size)
            throw std::invalid_argument("Metal guest shader start address overflows");
        Shader::Maxwell::Flow::CFG cfg{environment, flow, environment.StartAddress() + header_size,
                                       is_vertex_a};
        return Shader::Maxwell::TranslateProgram(instructions, blocks, environment, cfg,
                                                 HostTranslateInfo());
    };
    auto program = [&] {
        try {
            if (!vertex_a)
                return translate(env, false);
            auto first = translate(*vertex_a, true);
            auto second = translate(env, false);
            return Shader::Maxwell::MergeDualVertexPrograms(first, second, env);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Maxwell frontend: ") + error.what());
        }
    }();
    Shader::Maxwell::ConvertLegacyToGeneric(program, info);
    auto source = [&] {
        try {
            return EmitGuestShader(program, info, bindings);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Metal shader emission: ") + error.what());
        }
    }();
    return {std::move(source), std::move(program.info)};
}
} // namespace NativeMetal
