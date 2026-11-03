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
