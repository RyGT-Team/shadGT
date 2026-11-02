# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(RyGTEnhancementTests LANGUAGES C CXX)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(SHADGT_ROOT "../.." ABSOLUTE)
set(SHADGT_FMT_INCLUDE "${SHADGT_ROOT}/externals/fmt/include" CACHE PATH "fmt headers")
set(SHADGT_MAGIC_ENUM_INCLUDE "${SHADGT_ROOT}/externals/magic_enum/include" CACHE PATH "magic_enum headers")

foreach(name IN ITEMS parallel_readonly_hint parallel_descriptor_actions parallel_producer_oom parallel_mixed_snapshot shared_pipeline_policy pipeline_retention parallel_hash_fallback parallel_descriptor_snapshot unique_upload_batch descriptor_update_batch allocation_pressure_retry dlssg_options dlssg_state ghost_smoothing texture_gc video_out_source tiling_specialization)
    add_executable(${name}_test ${name}_test.cpp)
    target_compile_features(${name}_test PRIVATE cxx_std_23)
    target_include_directories(${name}_test PRIVATE
        "${SHADGT_ROOT}" "${SHADGT_ROOT}/src"
        "${SHADGT_FMT_INCLUDE}" "${SHADGT_MAGIC_ENUM_INCLUDE}"
        "${CMAKE_CURRENT_BINARY_DIR}")
    if(name STREQUAL "parallel_descriptor_actions" OR name STREQUAL "parallel_descriptor_snapshot" OR name STREQUAL "parallel_mixed_snapshot" OR name STREQUAL "video_out_source" OR name STREQUAL "tiling_specialization")
        target_include_directories(${name}_test PRIVATE
            "${SHADGT_ROOT}/externals/vulkan-headers/include")
    endif()
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /EHsc)
        target_compile_definitions(${name}_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(name STREQUAL "parallel_hash_fallback")
        target_include_directories(${name}_test PRIVATE "${SHADGT_ROOT}/externals/xxhash")
    endif()
    if(name STREQUAL "parallel_readonly_hint" OR name STREQUAL "parallel_descriptor_actions" OR name STREQUAL "parallel_producer_oom" OR name STREQUAL "parallel_mixed_snapshot" OR name STREQUAL "parallel_descriptor_snapshot" OR name STREQUAL "parallel_hash_fallback" OR name STREQUAL "pipeline_retention" OR name STREQUAL "shared_pipeline_policy")
        if(MSVC)
            target_compile_options(${name}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(${name}_test PRIVATE -UNDEBUG)
        endif()
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

target_sources(tiling_specialization_test PRIVATE "${SHADGT_ROOT}/src/video_core/amdgpu/tiling.cpp")
target_compile_definitions(tiling_specialization_test PRIVATE FMT_HEADER_ONLY)

add_test(NAME ghost_smoothing_disabled COMMAND ghost_smoothing_test disabled)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_gc.py"
        "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    DEPENDS extract_gc.py "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
    VERBATIM)
target_sources(texture_gc_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc")

if(WIN32)
    add_executable(parallel_hash_dispatch_test
        parallel_hash_dispatch_test.cpp
        "${SHADGT_ROOT}/ryty_gpu/native/native_xxh512.c"
        "${SHADGT_ROOT}/externals/xxhash/xxhash.c")
    target_compile_features(parallel_hash_dispatch_test PRIVATE cxx_std_23)
    target_include_directories(parallel_hash_dispatch_test PRIVATE
        "${SHADGT_ROOT}" "${SHADGT_ROOT}/externals/xxhash")
    if(MSVC)
        target_compile_options(parallel_hash_dispatch_test PRIVATE /EHsc /UNDEBUG)
    else()
        target_compile_options(parallel_hash_dispatch_test PRIVATE -UNDEBUG)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i.86)$")
        set_source_files_properties("${SHADGT_ROOT}/ryty_gpu/native/native_xxh512.c" PROPERTIES COMPILE_OPTIONS "-march=x86-64")
    endif()
    add_test(NAME parallel_hash_dispatch COMMAND parallel_hash_dispatch_test)
endif()

add_test(NAME parallel_snapshot_source_fragments
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_parallel_snapshot_fragments.py"
            "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/parallel_mixed_snapshot_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/parallel_producer_oom_test.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/parallel_descriptor_actions_test.cpp")

add_test(NAME ahead_capture_safety
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_ahead_capture_safety.py"
            "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp")
