# The pinned Dynarmic fork puts ARM64 cache mutations inside assert(), so
# Release builds lose the cache entirely. Apply the fix to both dependency paths.
function(citrosis_patch_dynarmic source_dir)
    find_package(Git REQUIRED)
    set(patch "${CMAKE_SOURCE_DIR}/patches/dynarmic-arm64-release-cache.patch")
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${patch}"
        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE can_apply
        OUTPUT_QUIET ERROR_QUIET)
    if(can_apply EQUAL 0)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${patch}"
            WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE applied)
        if(NOT applied EQUAL 0)
            message(FATAL_ERROR "Could not apply the Dynarmic ARM64 Release cache fix")
        endif()
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE already_applied
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT already_applied EQUAL 0)
            message(FATAL_ERROR "Dynarmic does not match the ARM64 Release cache patch")
        endif()
    endif()
endfunction()
