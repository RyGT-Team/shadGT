# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24)
project(RyGTEnhancementTests LANGUAGES C CXX)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(SHADGT_ROOT "../.." ABSOLUTE)
set(SHADGT_FMT_INCLUDE "${SHADGT_ROOT}/externals/fmt/include" CACHE PATH "fmt headers")
set(SHADGT_MAGIC_ENUM_INCLUDE "${SHADGT_ROOT}/externals/magic_enum/include" CACHE PATH "magic_enum headers")

add_executable(parallel_admission_metadata_test parallel_admission_metadata_test.cpp)
target_compile_features(parallel_admission_metadata_test PRIVATE cxx_std_23)
target_include_directories(parallel_admission_metadata_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_admission_metadata_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_admission_metadata_test PRIVATE -UNDEBUG)
endif()
set(admission_fragments admission-stage-type.inc admission-stage-build.inc admission-stage-select.inc
    admission-worker-identity.inc admission-vertex-build.inc admission-vertex-select.inc)
list(TRANSFORM admission_fragments PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
add_custom_command(OUTPUT ${admission_fragments}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_parallel_admission.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_graphics_pipeline.h"
        "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_parallel_admission.py
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_graphics_pipeline.h" VERBATIM)
target_sources(parallel_admission_metadata_test PRIVATE ${admission_fragments})
add_test(NAME parallel_admission_metadata COMMAND parallel_admission_metadata_test)

foreach(name IN ITEMS parallel_initial_mode parallel_readonly_hint parallel_descriptor_actions parallel_producer_oom parallel_mixed_snapshot shared_pipeline_policy pipeline_retention parallel_hash_fallback parallel_descriptor_snapshot unique_upload_batch descriptor_update_batch allocation_pressure_retry dlssg_options dlssg_state ghost_smoothing texture_gc video_out_source tiling_specialization)
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
    if(name STREQUAL "descriptor_update_batch" OR name STREQUAL "parallel_initial_mode" OR name STREQUAL "parallel_readonly_hint" OR name STREQUAL "parallel_descriptor_actions" OR name STREQUAL "parallel_producer_oom" OR name STREQUAL "parallel_mixed_snapshot" OR name STREQUAL "parallel_descriptor_snapshot" OR name STREQUAL "parallel_hash_fallback" OR name STREQUAL "pipeline_retention" OR name STREQUAL "shared_pipeline_policy")
        if(MSVC)
            target_compile_options(${name}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(${name}_test PRIVATE -UNDEBUG)
        endif()
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()

foreach(name IN ITEMS parallel_sampled_proof_compact parallel_descriptor_action_layout parallel_direct_covers_all)
    add_executable(${name}_test ${name}_test.cpp)
    target_compile_features(${name}_test PRIVATE cxx_std_23)
    target_include_directories(${name}_test PRIVATE
        "${SHADGT_ROOT}" "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}"
        "${SHADGT_ROOT}/externals/vulkan-headers/include")
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /EHsc /UNDEBUG)
    else()
        target_compile_options(${name}_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/descriptor-action-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_descriptor_action.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.h"
        "${CMAKE_CURRENT_BINARY_DIR}/descriptor-action-under-test.inc"
    DEPENDS extract_descriptor_action.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.h"
    VERBATIM)
target_sources(parallel_descriptor_action_layout_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/descriptor-action-under-test.inc")

target_sources(tiling_specialization_test PRIVATE "${SHADGT_ROOT}/src/video_core/amdgpu/tiling.cpp")
target_compile_definitions(tiling_specialization_test PRIVATE FMT_HEADER_ONLY)

add_test(NAME ghost_smoothing_disabled COMMAND ghost_smoothing_test disabled)
add_executable(parallel_vertex_direct_hint_test parallel_vertex_direct_hint_test.cpp)
target_compile_features(parallel_vertex_direct_hint_test PRIVATE cxx_std_23)
target_include_directories(parallel_vertex_direct_hint_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_vertex_direct_hint_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_vertex_direct_hint_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vertex-direct-hint-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_vertex_direct_hint.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/vertex-direct-hint-under-test.inc"
    DEPENDS extract_vertex_direct_hint.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
target_sources(parallel_vertex_direct_hint_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vertex-direct-hint-under-test.inc")
add_test(NAME parallel_vertex_direct_hint COMMAND parallel_vertex_direct_hint_test)
add_executable(parallel_image_batch_test parallel_image_batch_test.cpp)
target_compile_features(parallel_image_batch_test PRIVATE cxx_std_23)
target_include_directories(parallel_image_batch_test PRIVATE "${SHADGT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_image_batch_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_image_batch_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/image-batch-producer.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-start.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-end.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-skip.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-inside.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-factory.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_image_batch.py" "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" "${CMAKE_CURRENT_BINARY_DIR}" "${SHADGT_ROOT}/ryty_gpu/native/descriptor_snapshot.h"
    DEPENDS extract_image_batch.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" "${SHADGT_ROOT}/ryty_gpu/native/descriptor_snapshot.h" VERBATIM)
target_sources(parallel_image_batch_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/image-batch-producer.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-start.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-end.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-skip.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-inside.inc" "${CMAKE_CURRENT_BINARY_DIR}/image-batch-factory.inc")
add_test(NAME parallel_image_batch COMMAND parallel_image_batch_test)
add_executable(parallel_buffer_prefix_test parallel_buffer_prefix_test.cpp)
target_compile_features(parallel_buffer_prefix_test PRIVATE cxx_std_23)
target_include_directories(parallel_buffer_prefix_test PRIVATE "${SHADGT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_buffer_prefix_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_buffer_prefix_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-producer.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-recorder.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-end.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-loop.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-empty-final.inc" "${CMAKE_CURRENT_BINARY_DIR}/native-vi-admission.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_buffer_prefix.py" "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_buffer_prefix.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(parallel_buffer_prefix_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-producer.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-recorder.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-end.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-loop.inc" "${CMAKE_CURRENT_BINARY_DIR}/buffer-prefix-empty-final.inc" "${CMAKE_CURRENT_BINARY_DIR}/native-vi-admission.inc")
add_test(NAME parallel_buffer_prefix COMMAND parallel_buffer_prefix_test)
add_executable(parallel_freshness_certificate_test parallel_freshness_certificate_test.cpp)
target_compile_features(parallel_freshness_certificate_test PRIVATE cxx_std_23)
target_include_directories(parallel_freshness_certificate_test PRIVATE "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_freshness_certificate_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_freshness_certificate_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/freshness-log-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-admission-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-first-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-recheck-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_freshness_certificate.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.h"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_freshness_certificate.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.h"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
target_sources(parallel_freshness_certificate_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/freshness-log-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-admission-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-first-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/freshness-recheck-under-test.inc")
add_test(NAME parallel_freshness_certificate COMMAND parallel_freshness_certificate_test)
add_executable(parallel_vertex_owner_hint_test parallel_vertex_owner_hint_test.cpp)
target_compile_features(parallel_vertex_owner_hint_test PRIVATE cxx_std_23)
target_include_directories(parallel_vertex_owner_hint_test PRIVATE "${SHADGT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_vertex_owner_hint_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_vertex_owner_hint_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vertex-owner-hint-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-native-receipt-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-receipt-select.inc" "${CMAKE_CURRENT_BINARY_DIR}/index-receipt-select.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_vertex_owner_hint.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/vertex-owner-hint-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-native-receipt-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-receipt-select.inc" "${CMAKE_CURRENT_BINARY_DIR}/index-receipt-select.inc"
    DEPENDS extract_vertex_owner_hint.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
target_sources(parallel_vertex_owner_hint_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vertex-owner-hint-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-native-receipt-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-receipt-select.inc" "${CMAKE_CURRENT_BINARY_DIR}/index-receipt-select.inc")
add_test(NAME parallel_vertex_owner_hint COMMAND parallel_vertex_owner_hint_test)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_gc.py"
        "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc"
    DEPENDS extract_gc.py "${SHADGT_ROOT}/src/video_core/texture_cache/texture_cache.cpp"
    VERBATIM)
target_sources(texture_gc_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gc-function-under-test.inc")
add_executable(cpu_stack_registry_test cpu_stack_registry_test.cpp)
target_compile_features(cpu_stack_registry_test PRIVATE cxx_std_23)
target_include_directories(cpu_stack_registry_test PRIVATE "${SHADGT_ROOT}")
add_executable(cpu_stack_paths_test cpu_stack_paths_test.cpp)
target_compile_features(cpu_stack_paths_test PRIVATE cxx_std_23)
target_include_directories(cpu_stack_paths_test PRIVATE "${SHADGT_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(cpu_stack_registry_test PRIVATE /UNDEBUG)
    target_compile_options(cpu_stack_paths_test PRIVATE /UNDEBUG)
else()
    target_compile_options(cpu_stack_registry_test PRIVATE -UNDEBUG)
    target_compile_options(cpu_stack_paths_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-on.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-sort.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-copy.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-drop.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-unwatch.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-release.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_cpu_stack.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-on.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-sort.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-copy.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-drop.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-unwatch.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-release.inc"
    DEPENDS extract_cpu_stack.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
target_sources(cpu_stack_paths_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-on.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-sort.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-copy.inc")
add_test(NAME cpu_stack_registry COMMAND cpu_stack_registry_test)
add_test(NAME cpu_stack_paths COMMAND cpu_stack_paths_test)

add_executable(cpu_stack_retire_test cpu_stack_retire_test.cpp)
target_compile_features(cpu_stack_retire_test PRIVATE cxx_std_23)
target_include_directories(cpu_stack_retire_test PRIVATE "${SHADGT_ROOT}" "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(cpu_stack_retire_test PRIVATE /UNDEBUG)
else()
    target_compile_options(cpu_stack_retire_test PRIVATE -UNDEBUG)
endif()
target_sources(cpu_stack_retire_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-on.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-sort.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-drop.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-unwatch.inc" "${CMAKE_CURRENT_BINARY_DIR}/cpu-stack-release.inc")
add_test(NAME cpu_stack_retire COMMAND cpu_stack_retire_test)

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

add_executable(parallel_direct_try_parts_test parallel_direct_try_parts_test.cpp)
target_compile_features(parallel_direct_try_parts_test PRIVATE cxx_std_23)
target_include_directories(parallel_direct_try_parts_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_direct_try_parts_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_direct_try_parts_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/direct-try-parts-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_direct_try_parts.py"
        "${SHADGT_ROOT}/src/video_core/buffer_cache/direct_memory.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/direct-try-parts-under-test.inc"
    DEPENDS extract_direct_try_parts.py "${SHADGT_ROOT}/src/video_core/buffer_cache/direct_memory.cpp"
    VERBATIM)
target_sources(parallel_direct_try_parts_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/direct-try-parts-under-test.inc")
add_test(NAME parallel_direct_try_parts COMMAND parallel_direct_try_parts_test)

add_executable(sync_owner_pins_test sync_owner_pins_test.cpp)
target_compile_features(sync_owner_pins_test PRIVATE cxx_std_23)
target_include_directories(sync_owner_pins_test PRIVATE "${SHADGT_ROOT}")
if(MSVC)
    target_compile_options(sync_owner_pins_test PRIVATE /UNDEBUG)
else()
    target_compile_options(sync_owner_pins_test PRIVATE -UNDEBUG)
endif()
add_test(NAME sync_owner_pins COMMAND sync_owner_pins_test)

add_executable(frame_queue_trace_test frame_queue_trace_test.cpp)
target_compile_features(frame_queue_trace_test PRIVATE cxx_std_23)
target_include_directories(frame_queue_trace_test PRIVATE "${SHADGT_ROOT}/src")
if(MSVC)
    target_compile_options(frame_queue_trace_test PRIVATE /UNDEBUG)
else()
    target_compile_options(frame_queue_trace_test PRIVATE -UNDEBUG)
endif()
add_test(NAME frame_queue_trace COMMAND frame_queue_trace_test)

add_executable(frame_queue_trace_runtime_test frame_queue_trace_runtime_test.cpp "${SHADGT_ROOT}/src/common/frame_queue_trace.cpp")
target_compile_features(frame_queue_trace_runtime_test PRIVATE cxx_std_23)
target_compile_definitions(frame_queue_trace_runtime_test PRIVATE FRAME_QUEUE_TRACE_TEST)
target_include_directories(frame_queue_trace_runtime_test PRIVATE "${SHADGT_ROOT}/src")
if(MSVC)
    target_compile_options(frame_queue_trace_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(frame_queue_trace_runtime_test PRIVATE -UNDEBUG)
endif()
add_test(NAME frame_queue_trace_disabled COMMAND frame_queue_trace_runtime_test disabled)
add_test(NAME frame_queue_trace_runtime COMMAND frame_queue_trace_runtime_test)

add_executable(parallel_upload_memo_receipt_test parallel_upload_memo_receipt_test.cpp)
target_compile_features(parallel_upload_memo_receipt_test PRIVATE cxx_std_23)
target_include_directories(parallel_upload_memo_receipt_test PRIVATE "${SHADGT_ROOT}/src")
if(MSVC)
    target_compile_options(parallel_upload_memo_receipt_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_upload_memo_receipt_test PRIVATE -UNDEBUG)
endif()
add_test(NAME parallel_upload_memo_receipt COMMAND parallel_upload_memo_receipt_test)

add_executable(parallel_upload_receipt_test parallel_upload_receipt_test.cpp)
target_compile_features(parallel_upload_receipt_test PRIVATE cxx_std_23)
target_include_directories(parallel_upload_receipt_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_upload_receipt_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_upload_receipt_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/upload-backing-proof.inc" "${CMAKE_CURRENT_BINARY_DIR}/upload-incarnation-bump.inc" "${CMAKE_CURRENT_BINARY_DIR}/upload-commit-proof.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_upload_receipt.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_upload_receipt.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(parallel_upload_receipt_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/upload-backing-proof.inc" "${CMAKE_CURRENT_BINARY_DIR}/upload-incarnation-bump.inc" "${CMAKE_CURRENT_BINARY_DIR}/upload-commit-proof.inc")
add_test(NAME parallel_upload_receipt COMMAND parallel_upload_receipt_test)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/descriptor-stage-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_descriptor_stage.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/descriptor-stage-under-test.inc"
    DEPENDS extract_descriptor_stage.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(descriptor_update_batch_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/descriptor-stage-under-test.inc")

add_executable(parallel_pass_closure_test parallel_pass_closure_test.cpp)
target_compile_features(parallel_pass_closure_test PRIVATE cxx_std_23)
target_include_directories(parallel_pass_closure_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_pass_closure_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_pass_closure_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pass-closure-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/emit-pass-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/inline-safe-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_pass_closure.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/pass-closure-under-test.inc"
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.h" "${CMAKE_CURRENT_BINARY_DIR}/emit-pass-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/inline-safe-under-test.inc"
    DEPENDS extract_pass_closure.py "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.h" "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(parallel_pass_closure_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pass-closure-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/emit-pass-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/inline-safe-under-test.inc")
add_test(NAME parallel_pass_closure COMMAND parallel_pass_closure_test)

add_executable(parallel_inline_continuation_test parallel_inline_continuation_test.cpp)
target_compile_features(parallel_inline_continuation_test PRIVATE cxx_std_23)
target_include_directories(parallel_inline_continuation_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_inline_continuation_test PRIVATE /UNDEBUG)
else()
    target_compile_options(parallel_inline_continuation_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inline-continuation-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_inline_continuation.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/inline-continuation-under-test.inc"
    DEPENDS extract_inline_continuation.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(parallel_inline_continuation_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/inline-continuation-under-test.inc")
add_test(NAME parallel_inline_continuation COMMAND parallel_inline_continuation_test)
add_executable(async_mirror_buffer_test async_mirror_buffer_test.cpp)
target_compile_features(async_mirror_buffer_test PRIVATE cxx_std_23)
target_include_directories(async_mirror_buffer_test PRIVATE "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(async_mirror_buffer_test PRIVATE /UNDEBUG)
else()
    target_compile_options(async_mirror_buffer_test PRIVATE -UNDEBUG)
endif()
target_sources(async_mirror_buffer_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/direct-try-parts-under-test.inc")
add_test(NAME async_mirror_buffer COMMAND async_mirror_buffer_test)

add_executable(mirror_probe_test mirror_probe_test.cpp)
target_compile_features(mirror_probe_test PRIVATE cxx_std_23)
target_include_directories(mirror_probe_test PRIVATE "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(mirror_probe_test PRIVATE /UNDEBUG)
else()
    target_compile_options(mirror_probe_test PRIVATE -UNDEBUG)
endif()
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mirror-resume-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-probe-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-merge-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-prepare-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-priority-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-requeue-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_mirror_probe.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-resume-under-test.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-probe-under-test.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-merge-under-test.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-prepare-under-test.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-priority-under-test.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/mirror-requeue-under-test.inc"
    DEPENDS extract_mirror_probe.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
target_sources(mirror_probe_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/direct-try-parts-under-test.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/mirror-resume-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-probe-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-merge-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-prepare-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-priority-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/mirror-requeue-under-test.inc")
add_test(NAME mirror_probe COMMAND mirror_probe_test)

add_executable(gpu_timing_flag_test gpu_timing_flag_test.cpp)
target_compile_features(gpu_timing_flag_test PRIVATE cxx_std_23)
target_include_directories(gpu_timing_flag_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(gpu_timing_flag_test PRIVATE /UNDEBUG)
else()
    target_compile_options(gpu_timing_flag_test PRIVATE -UNDEBUG)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gpu-timing-flag-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_gpu_timing_flag.py"
      "${SHADGT_ROOT}/ryty_gpu/native/rhi/rhi_vulkan.cpp" "${CMAKE_CURRENT_BINARY_DIR}/gpu-timing-flag-under-test.inc"
    DEPENDS extract_gpu_timing_flag.py "${SHADGT_ROOT}/ryty_gpu/native/rhi/rhi_vulkan.cpp" VERBATIM)
target_sources(gpu_timing_flag_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gpu-timing-flag-under-test.inc")
add_test(NAME gpu_timing_flag COMMAND gpu_timing_flag_test)

add_executable(fast_flip_wait_test fast_flip_wait_test.cpp)
target_compile_features(fast_flip_wait_test PRIVATE cxx_std_23)
target_include_directories(fast_flip_wait_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(fast_flip_wait_test PRIVATE /UNDEBUG)
else()
    target_compile_options(fast_flip_wait_test PRIVATE -UNDEBUG)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-wait-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-eligibility-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_fast_flip_wait.py"
      "${SHADGT_ROOT}/src/core/libraries/videoout/driver.cpp" "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-wait-under-test.inc"
      "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-eligibility-under-test.inc"
    DEPENDS extract_fast_flip_wait.py "${SHADGT_ROOT}/src/core/libraries/videoout/driver.cpp" VERBATIM)
target_sources(fast_flip_wait_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-wait-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/fast-flip-eligibility-under-test.inc")
add_test(NAME fast_flip_wait COMMAND fast_flip_wait_test)

add_executable(parallel_vertex_replay_test parallel_vertex_replay_test.cpp)
target_compile_features(parallel_vertex_replay_test PRIVATE cxx_std_23)
target_include_directories(parallel_vertex_replay_test PRIVATE "${SHADGT_ROOT}/src" "${SHADGT_ROOT}/externals/vulkan-headers/include" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_vertex_replay_test PRIVATE /EHsc /UNDEBUG)
else()
    target_compile_options(parallel_vertex_replay_test PRIVATE -UNDEBUG)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vertex-same-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-record-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_vertex_replay.py" "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.cpp" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_vertex_replay.py "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.cpp" VERBATIM)
target_sources(parallel_vertex_replay_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vertex-same-under-test.inc" "${CMAKE_CURRENT_BINARY_DIR}/vertex-record-under-test.inc")
add_test(NAME parallel_vertex_replay COMMAND parallel_vertex_replay_test)

add_executable(parallel_snapshot_replay_test parallel_snapshot_replay_test.cpp)
target_compile_features(parallel_snapshot_replay_test PRIVATE cxx_std_23)
target_include_directories(parallel_snapshot_replay_test PRIVATE "${SHADGT_ROOT}" "${SHADGT_ROOT}/src" "${SHADGT_ROOT}/externals/vulkan-headers/include" "${SHADGT_FMT_INCLUDE}" "${SHADGT_MAGIC_ENUM_INCLUDE}")
if(MSVC)
    target_compile_options(parallel_snapshot_replay_test PRIVATE /EHsc /UNDEBUG)
else()
    target_compile_options(parallel_snapshot_replay_test PRIVATE -UNDEBUG)
endif()
add_test(NAME parallel_snapshot_replay COMMAND parallel_snapshot_replay_test)

add_executable(parallel_vertex_validate_test parallel_vertex_validate_test.cpp)
target_compile_features(parallel_vertex_validate_test PRIVATE cxx_std_23)
target_include_directories(parallel_vertex_validate_test PRIVATE "${SHADGT_ROOT}/externals/vulkan-headers/include" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_vertex_validate_test PRIVATE /EHsc /UNDEBUG)
else()
    target_compile_options(parallel_vertex_validate_test PRIVATE -UNDEBUG)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vertex-validate-under-test.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_vertex_validate.py" "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.cpp" "${CMAKE_CURRENT_BINARY_DIR}/vertex-validate-under-test.inc"
    DEPENDS extract_vertex_validate.py "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.cpp" VERBATIM)
target_sources(parallel_vertex_validate_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vertex-validate-under-test.inc")
add_test(NAME parallel_vertex_validate COMMAND parallel_vertex_validate_test)

# Keep NDEBUG: assertion-enabled fixtures cannot establish Release behavior.
add_executable(frame_queue_release_test frame_queue_release_test.cpp
    "${SHADGT_ROOT}/src/common/frame_queue_trace.cpp")
target_compile_features(frame_queue_release_test PRIVATE cxx_std_23)
target_compile_definitions(frame_queue_release_test PRIVATE NDEBUG)
target_include_directories(frame_queue_release_test PRIVATE "${SHADGT_ROOT}/src")
if(MSVC)
    target_compile_options(frame_queue_release_test PRIVATE /EHsc)
endif()
add_test(NAME frame_queue_release COMMAND frame_queue_release_test
    "${CMAKE_CURRENT_BINARY_DIR}/frame-queue-release-disabled")


# Exercise the actual consumer under production Release preprocessing. The old
# body differs only by the missing policy condition and must fail these cases.
add_executable(compute_menu_policy_test compute_menu_policy_test.cpp)
target_compile_features(compute_menu_policy_test PRIVATE cxx_std_23)
target_compile_definitions(compute_menu_policy_test PRIVATE NDEBUG)
target_include_directories(compute_menu_policy_test PRIVATE "${SHADGT_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(compute_menu_policy_test PRIVATE /EHsc)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/compute_policy.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_compute_policy.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer.cpp" "${CMAKE_CURRENT_BINARY_DIR}/compute_policy.inc"
    DEPENDS extract_compute_policy.py "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer.cpp" VERBATIM)
target_sources(compute_menu_policy_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/compute_policy.inc")
add_test(NAME compute_menu_policy_release COMMAND compute_menu_policy_test)
foreach(case IN ITEMS "--legacy-negative" "--legacy-negative-same" "--legacy-negative-null")
    string(REPLACE "--legacy-negative" "compute_menu_policy_old" test_name "${case}")
    add_test(NAME "${test_name}"
        COMMAND "${Python3_EXECUTABLE}" -c
            "import subprocess,sys; p=subprocess.run(sys.argv[1:]); sys.exit(0 if p.returncode==9 else 1)"
            "$<TARGET_FILE:compute_menu_policy_test>" "${case}")
endforeach()

add_executable(worker_identity_defer_test worker_identity_defer_test.cpp)
target_compile_features(worker_identity_defer_test PRIVATE cxx_std_23)
target_compile_definitions(worker_identity_defer_test PRIVATE NDEBUG)
target_include_directories(worker_identity_defer_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(worker_identity_defer_test PRIVATE /EHsc)
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/worker-identity-defer.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_worker_identity_defer.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/worker-identity-defer.inc"
    DEPENDS extract_worker_identity_defer.py
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp" VERBATIM)
target_sources(worker_identity_defer_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/worker-identity-defer.inc")
add_test(NAME worker_identity_defer_release COMMAND worker_identity_defer_test)
add_test(NAME worker_identity_eager_negative
    COMMAND "${Python3_EXECUTABLE}" -c
        "import subprocess,sys; p=subprocess.run(sys.argv[1:]); sys.exit(0 if p.returncode==9 else 1)"
        "$<TARGET_FILE:worker_identity_defer_test>" "--eager-negative")

# Source-extracted functions use the production diagnostic macro, including its
# NDEBUG branch, rather than a duplicated test-only definition.
set(NATIVE_DIAGNOSTIC_CONTEXT "${CMAKE_CURRENT_BINARY_DIR}/native-diagnostic-context.h")
add_custom_command(OUTPUT "${NATIVE_DIAGNOSTIC_CONTEXT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_native_diagnostic.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${NATIVE_DIAGNOSTIC_CONTEXT}"
    DEPENDS extract_native_diagnostic.py
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
    VERBATIM)
get_directory_property(NATIVE_TEST_TARGETS BUILDSYSTEM_TARGETS)
foreach(NATIVE_TEST_TARGET IN LISTS NATIVE_TEST_TARGETS)
    target_sources(${NATIVE_TEST_TARGET} PRIVATE "${NATIVE_DIAGNOSTIC_CONTEXT}")
    if(MSVC)
        target_compile_options(${NATIVE_TEST_TARGET} PRIVATE "/FI${NATIVE_DIAGNOSTIC_CONTEXT}")
    else()
        target_compile_options(${NATIVE_TEST_TARGET} PRIVATE -include "${NATIVE_DIAGNOSTIC_CONTEXT}")
    endif()
endforeach()

add_executable(ordered_pass_worker_handles_test ordered_pass_worker_handles_test.cpp)
target_compile_features(ordered_pass_worker_handles_test PRIVATE cxx_std_23)
target_include_directories(ordered_pass_worker_handles_test PRIVATE "${SHADGT_ROOT}")
if(MSVC)
    target_compile_options(ordered_pass_worker_handles_test PRIVATE /UNDEBUG)
else()
    target_compile_options(ordered_pass_worker_handles_test PRIVATE -UNDEBUG)
endif()
add_test(NAME ordered_pass_worker_handles COMMAND ordered_pass_worker_handles_test)

add_executable(parallel_chunk_submission_test parallel_chunk_submission_test.cpp)
target_compile_features(parallel_chunk_submission_test PRIVATE cxx_std_23)
target_include_directories(parallel_chunk_submission_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(MSVC)
    target_compile_options(parallel_chunk_submission_test PRIVATE /UNDEBUG /EHsc)
else()
    target_compile_options(parallel_chunk_submission_test PRIVATE -UNDEBUG)
endif()
set(chunk_fragments chunk-closure-types.inc chunk-end-rendering.inc chunk-close-before-draw.inc chunk-rotate-segment.inc)
list(TRANSFORM chunk_fragments PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
add_custom_command(OUTPUT ${chunk_fragments}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_parallel_chunk_submission.py"
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_rotate_segment.h"
        "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_parallel_chunk_submission.py
        "${SHADGT_ROOT}/src/video_core/renderer_vulkan/vk_rasterizer_native.cpp"
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_rotate_segment.h" VERBATIM)
target_sources(parallel_chunk_submission_test PRIVATE ${chunk_fragments})
add_test(NAME parallel_chunk_submission COMMAND parallel_chunk_submission_test)

# CPU publication/traversal and coordinator ownership/order contracts.
set(stream_contract_fragments stream-traversal.inc stream-coordinator-types.inc stream-drain-collect.inc stream-coordinator-methods.inc)
list(TRANSFORM stream_contract_fragments PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
add_custom_command(OUTPUT ${stream_contract_fragments}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract_stream_contract.py"
        "${SHADGT_ROOT}/ryty_gpu/native" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS extract_stream_contract.py
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_recording.cpp"
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_coordinator.h"
        "${SHADGT_ROOT}/ryty_gpu/native/parallel_pass_coordinator.cpp" VERBATIM)
foreach(name IN ITEMS draw_publication stream_coordinator)
    add_executable(${name}_test ${name}_test.cpp)
    target_compile_features(${name}_test PRIVATE cxx_std_23)
    target_include_directories(${name}_test PRIVATE "${SHADGT_ROOT}" "${SHADGT_ROOT}/ryty_gpu/native" "${CMAKE_CURRENT_BINARY_DIR}")
    if(MSVC)
        target_compile_options(${name}_test PRIVATE /EHsc /UNDEBUG)
    else()
        target_compile_options(${name}_test PRIVATE -UNDEBUG)
    endif()
    target_sources(${name}_test PRIVATE ${stream_contract_fragments})
    add_test(NAME ${name} COMMAND ${name}_test)
endforeach()
