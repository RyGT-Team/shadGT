// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef NDEBUG
#include <array>
#include <cstring>
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Vulkan {
// Recorder/lookup-lock only. Bounded history: eviction means an eventual demand can be missed.
class ReadAheadKeyTrace {
    static constexpr std::size_t Ways = 4, KeySlots = 512, StageSlots = 256;
    enum Field : std::size_t { Vertex, Patch, Attachments, Color, Blend, Masks, ShaderMask,
        Logic, Samples, Mrt, Depth, Alpha, Raster, OtherBytes, Equal, NoSameStages, Count };
    using Programs = std::array<u64, MaxShaderStages>;
    struct KeyRecord { GraphicsPipelineKey key; bool valid{}, demanded{}, selected{}; };
    struct StageRecord { GraphicsPipelineKey key; bool valid{}; };
    struct ProgramRecord { Programs programs{}; GraphicsPipelineKey key; bool valid{}; };
public:
    struct Example { GraphicsPipelineKey ahead, actual; Programs ahead_programs{}, actual_programs{}; std::size_t field{}; bool same_programs{}; };
    struct Summary {
        u64 ahead{}, actual_observations{}, exact_later{}, evicted_undemanded{}, evicted_demanded{};
        u64 evicted_unselected{}, evicted_selected{};
        u64 recorder_selected{}, selected_first{}, same_program_different_stages{}, same_program_same_stages{}, no_program_match{};
        std::array<u64, 3> ahead_params{}, selected_params{}; // direct, indirect, tessellated quads
        std::array<u64, Count> fields{};
        std::array<u64, MaxShaderStages> stage_mismatch{};
        std::array<Example, 4> examples{};
        std::size_t example_count{};
    };
    static constexpr std::array<const char*, Count> FieldNames{
        "vertex", "patch", "attachments", "color", "blend", "masks", "shader_mask",
        "logic", "samples", "mrt", "depth", "alpha_to_coverage", "raster", "other_bytes", "equal", "no_same_stages"};

    void Ahead(const GraphicsPipelineKey& key, const Programs& programs = {}, bool indirect = false, bool tess_quads = false) {
        ++summary.ahead;
        ++summary.ahead_params[indirect ? 1 : 0];
        summary.ahead_params[2] += tess_quads;
        const auto pb = ProgramBucket(programs);
        const ProgramRecord* same_program = nullptr;
        for (std::size_t i = pb; i < pb + Ways; ++i)
            if (recorder_programs[i].valid && recorder_programs[i].programs == programs) { same_program = &recorder_programs[i]; break; }
        if (!same_program) ++summary.no_program_match;
        else if (same_program->key.stage_hashes == key.stage_hashes) ++summary.same_program_same_stages;
        else ++summary.same_program_different_stages;
        const auto stage_begin = StageBucket(key);
        const StageRecord* same = nullptr;
        for (std::size_t i = stage_begin; i < stage_begin + Ways; ++i)
            if (actual_stages[i].valid && actual_stages[i].key.stage_hashes == key.stage_hashes) {
                same = &actual_stages[i]; break;
            }
        const auto field = same ? FirstDifference(same->key, key) : NoSameStages;
        ++summary.fields[field];
        if (!same && latest_valid)
            for (std::size_t i = 0; i < key.stage_hashes.size(); ++i)
                summary.stage_mismatch[i] += latest_actual.stage_hashes[i] != key.stage_hashes[i];
        if (summary.example_count < summary.examples.size()) {
            const auto* other = same_program ? same_program : (latest_recorder.valid ? &latest_recorder : nullptr);
            if (other && (!same_program || other->key != key)) {
                const auto example_field = other->key.stage_hashes == key.stage_hashes
                    ? FirstDifference(other->key, key) : NoSameStages;
                summary.examples[summary.example_count++] = {key, other->key, programs,
                    other->programs, example_field, same_program != nullptr};
            }
        }
        const auto begin = KeyBucket(key);
        std::size_t slot = KeySlots;
        for (std::size_t i = begin; i < begin + Ways; ++i) {
            if (keys[i].valid && keys[i].key == key) return;
            if (!keys[i].valid) slot = i;
        }
        if (slot == KeySlots) {
            slot = begin + key_next[begin / Ways]++ % Ways;
            ++(keys[slot].demanded ? summary.evicted_demanded : summary.evicted_undemanded);
            ++(keys[slot].selected ? summary.evicted_selected : summary.evicted_unselected);
        }
        keys[slot] = {key, true, false, false};
    }
    void RecorderSelected(const GraphicsPipelineKey& key, const Programs& programs,
                          bool indirect, bool tess_quads) {
        ++summary.recorder_selected;
        ++summary.selected_params[indirect ? 1 : 0];
        summary.selected_params[2] += tess_quads;
        const auto begin = KeyBucket(key);
        for (std::size_t i = begin; i < begin + Ways; ++i)
            if (keys[i].valid && !keys[i].selected && keys[i].key == key) {
                keys[i].selected = true; ++summary.selected_first; break;
            }
        const auto pb = ProgramBucket(programs);
        std::size_t slot = StageSlots;
        for (std::size_t i = pb; i < pb + Ways; ++i) {
            if (recorder_programs[i].valid && recorder_programs[i].programs == programs) { slot = i; break; }
            if (!recorder_programs[i].valid) slot = i;
        }
        if (slot == StageSlots) slot = pb + program_next[pb / Ways]++ % Ways;
        recorder_programs[slot] = {programs, key, true};
        latest_recorder = {programs, key, true};
    }
    void Actual(const GraphicsPipelineKey& key) {
        ++summary.actual_observations;
        const auto begin = KeyBucket(key);
        for (std::size_t i = begin; i < begin + Ways; ++i) {
            if (keys[i].valid && !keys[i].demanded && keys[i].key == key) {
                keys[i].demanded = true; ++summary.exact_later; break;
            }
        }
        const auto sb = StageBucket(key);
        std::size_t slot = StageSlots;
        for (std::size_t i = sb; i < sb + Ways; ++i) {
            if (actual_stages[i].valid && actual_stages[i].key.stage_hashes == key.stage_hashes) { slot = i; break; }
            if (!actual_stages[i].valid) slot = i;
        }
        if (slot == StageSlots) slot = sb + stage_next[sb / Ways]++ % Ways;
        actual_stages[slot] = {key, true};
        latest_actual = key; latest_valid = true;
    }
    Summary Take() { auto result = summary; summary = {}; return result; }
private:
    static std::size_t KeyBucket(const GraphicsPipelineKey& k) { return (std::hash<GraphicsPipelineKey>{}(k) % (KeySlots / Ways)) * Ways; }
    static std::size_t StageBucket(const GraphicsPipelineKey& k) {
        return (XXH3_64bits(k.stage_hashes.data(), sizeof(k.stage_hashes)) % (StageSlots / Ways)) * Ways;
    }
    static std::size_t ProgramBucket(const Programs& p) {
        return (XXH3_64bits(p.data(), sizeof(p)) % (StageSlots / Ways)) * Ways;
    }
    static std::size_t FirstDifference(const GraphicsPipelineKey& a, const GraphicsPipelineKey& b) {
        const auto differs = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) != 0; };
        if (differs(a.vertex_buffer_formats, b.vertex_buffer_formats)) return Vertex;
        if (a.patch_control_points != b.patch_control_points) return Patch;
        if (a.num_color_attachments != b.num_color_attachments) return Attachments;
        if (differs(a.color_buffers, b.color_buffers)) return Color;
        if (differs(a.blend_controls, b.blend_controls)) return Blend;
        if (differs(a.write_masks, b.write_masks)) return Masks;
        if (differs(a.cb_shader_mask, b.cb_shader_mask)) return ShaderMask;
        if (a.logic_op != b.logic_op) return Logic;
        if (a.num_samples != b.num_samples || a.depth_samples != b.depth_samples || differs(a.color_samples, b.color_samples)) return Samples;
        if (a.mrt_mask != b.mrt_mask) return Mrt;
        if (a.z_format != b.z_format || a.stencil_format != b.stencil_format || a.depth_clamp_enable != b.depth_clamp_enable) return Depth;
        if (a.alpha_to_coverage != b.alpha_to_coverage) return Alpha;
        if (a.prim_type != b.prim_type || a.polygon_mode != b.polygon_mode || a.clip_space != b.clip_space ||
            a.provoking_vtx_last != b.provoking_vtx_last || a.depth_clip_enable != b.depth_clip_enable) return Raster;
        return a == b ? Equal : OtherBytes;
    }
    std::array<KeyRecord, KeySlots> keys{};
    std::array<StageRecord, StageSlots> actual_stages{};
    std::array<ProgramRecord, StageSlots> recorder_programs{};
    ProgramRecord latest_recorder{};
    std::array<unsigned char, KeySlots / Ways> key_next{};
    std::array<unsigned char, StageSlots / Ways> stage_next{};
    std::array<unsigned char, StageSlots / Ways> program_next{};
    GraphicsPipelineKey latest_actual{};
    bool latest_valid{};
    Summary summary{};
};
} // namespace Vulkan
#endif
