# SPDX-License-Identifier: GPL-2.0-or-later

# Source builds honor the project's deployment target. An installed static
# package is an explicit alternative; its minimum macOS version may be newer.
option(CITROSIS_METAL_USE_SYSTEM_SPIRV_CROSS "Use installed SPIRV-Cross for Metal" OFF)
if (CITROSIS_METAL_USE_SYSTEM_SPIRV_CROSS OR NOT COMMAND CPMAddPackage)
    find_package(spirv_cross_core CONFIG QUIET)
    find_package(spirv_cross_glsl CONFIG QUIET)
    find_package(spirv_cross_msl CONFIG QUIET)
endif()

if (NOT TARGET spirv-cross-msl)
    if (NOT COMMAND CPMAddPackage)
        message(FATAL_ERROR "Metal needs SPIRV-Cross (install spirv-cross or enable CPM)")
    endif()
    CPMAddPackage(
        NAME citrosis_spirv_cross
        GITHUB_REPOSITORY KhronosGroup/SPIRV-Cross
        GIT_TAG d8e3e2b141b8c8a167b2e3984736a6baacff316c
        OPTIONS "SPIRV_CROSS_CLI OFF" "SPIRV_CROSS_ENABLE_TESTS OFF"
                "SPIRV_CROSS_ENABLE_HLSL OFF" "SPIRV_CROSS_ENABLE_CPP OFF"
                "SPIRV_CROSS_ENABLE_REFLECT OFF" "SPIRV_CROSS_ENABLE_C_API OFF"
                "SPIRV_CROSS_ENABLE_GLSL ON" "SPIRV_CROSS_ENABLE_MSL ON"
    )
endif()

foreach(component spirv-cross-core spirv-cross-glsl spirv-cross-msl)
    get_target_property(is_imported ${component} IMPORTED)
    if (NOT is_imported AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # The pinned upstream release has intentional unannotated fall-throughs.
        target_compile_options(${component} PRIVATE -Wno-error=implicit-fallthrough)
    endif()
endforeach()

add_library(citrosis_metal STATIC
    metal_runtime.h metal_runtime.mm
    metal_shader.h metal_shader.cpp
    metal_scheduler.h metal_scheduler.cpp
    metal_kernels.h metal_kernels.cpp
    metal_buffer_cache.h metal_buffer_cache.cpp
    metal_formats.h metal_formats.mm
)
target_include_directories(citrosis_metal PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(citrosis_metal PUBLIC shader_recompiler
    PRIVATE spirv-cross-msl "-framework Metal" "-framework Foundation" "-framework QuartzCore")
set_target_properties(citrosis_metal PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(citrosis_metal PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

add_executable(citrosis-metal-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_smoke.cpp")
target_link_libraries(citrosis-metal-smoke PRIVATE citrosis_metal)
find_program(CITROSIS_METAL_GLSLANG glslangValidator)
if(CITROSIS_METAL_GLSLANG)
    set(metal_fixtures)
    foreach(fixture fullscreen.vert attributes.vert sampled-array.frag storage.comp)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${fixture}.spv")
        add_custom_command(OUTPUT "${output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/fixtures"
            COMMAND "${CITROSIS_METAL_GLSLANG}" -V --target-env vulkan1.1
                "${PROJECT_SOURCE_DIR}/tools/macos/metal-fixtures/${fixture}" -o "${output}"
            DEPENDS "${PROJECT_SOURCE_DIR}/tools/macos/metal-fixtures/${fixture}"
            VERBATIM)
        list(APPEND metal_fixtures "${output}")
    endforeach()
    add_custom_target(citrosis-metal-fixtures DEPENDS ${metal_fixtures})
    add_dependencies(citrosis-metal-smoke citrosis-metal-fixtures)
else()
    message(STATUS "glslangValidator unavailable: Metal fixture generation disabled")
endif()

add_executable(citrosis-metal-present-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_present_smoke.mm")
target_link_libraries(citrosis-metal-present-smoke PRIVATE citrosis_metal "-framework AppKit")
set_target_properties(citrosis-metal-present-smoke PROPERTIES OBJCXX_STANDARD 20)
target_compile_options(citrosis-metal-present-smoke PRIVATE -fobjc-arc)

add_executable(citrosis-metal-guest-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_guest_smoke.cpp")
target_link_libraries(citrosis-metal-guest-smoke PRIVATE citrosis_metal video_core
    Vulkan::Headers Vulkan::UtilityHeaders GPUOpen::VulkanMemoryAllocator)

add_executable(citrosis-metal-buffer-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_buffer_cache_smoke.cpp")
target_link_libraries(citrosis-metal-buffer-smoke PRIVATE citrosis_metal)

add_executable(citrosis-metal-image-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_image_smoke.cpp")
target_link_libraries(citrosis-metal-image-smoke PRIVATE citrosis_metal video_core)

add_executable(citrosis-metal-texture-cache-smoke EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_texture_cache_smoke.cpp")
target_link_libraries(citrosis-metal-texture-cache-smoke PRIVATE citrosis_metal video_core)

add_executable(citrosis-metal-rasterizer-link-check EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/tools/macos/metal_rasterizer_link_check.cpp")
target_link_libraries(citrosis-metal-rasterizer-link-check PRIVATE citrosis_metal video_core)
