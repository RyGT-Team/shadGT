// The implementation under test is extracted verbatim from the production TU.
// Collaborators model current recorder memory versus the earlier owned snapshot.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "common/perf_monitor.h"
#ifndef NDEBUG
#error This fixture must exercise the production Release LeanRun/PerfFeatureEnabled definitions.
#endif
static_assert(Common::LeanRun());
static_assert(Common::PerfFeatureEnabled(66));
#define LOG_ERROR(...) ((void)0)
namespace Shader { constexpr unsigned MaxStageTypes = 1; }
namespace Vulkan {
struct SelectedStage {
    std::array<u32, 1> user_data{};
    std::vector<u32> flattened;
};
struct ComputePipeline {
    unsigned id;
    mutable u32 installed_user{};
    mutable std::vector<u32> installed_flat;
};
struct Cache {
    std::mutex mutex;
    const ComputePipeline* current{};
    u32 current_user{2};
    std::unordered_map<u32, std::vector<u32>> memory{{1, {11, 12}}, {2, {21, 22}}};
    unsigned lookups{};
    std::mutex& LookupMutex() { return mutex; }
    const ComputePipeline* GetComputePipeline() { ++lookups; return current; }
};
class Rasterizer {
public:
    struct Selection {
        bool is_compute{true};
        const ComputePipeline* compute{};
        std::array<SelectedStage, Shader::MaxStageTypes> stages{{SelectedStage{{1}, {11, 12}}}};
    } selection;
    Selection* selected_pipeline{&selection};
    Cache pipeline_cache;
    bool in_race{true}, pipelined{true}, read_capture{}, reject_selected{};
    unsigned copies{}, validated_installs{}, current_installs{};
    mutable unsigned policy_queries{};
    struct { unsigned compute_ahead{}, verified{}, verify_failed{}, mismatched{}; } select_stats;
    unsigned select_verify_count{};
    bool SelectsPipelinesAhead() const { ++policy_queries; return in_race && pipelined; }
    void CopyComputeStage(std::array<SelectedStage, Shader::MaxStageTypes>& stages) {
        ++copies;
        stages[0].user_data[0] = pipeline_cache.current_user;
        stages[0].flattened = pipeline_cache.memory.at(pipeline_cache.current_user);
    }
    bool InstallDrawStages(const ComputePipeline* pipeline,
                           std::array<SelectedStage, Shader::MaxStageTypes>& stages, bool validate) {
        if (validate) {
            ++validated_installs;
            if (reject_selected) return false;
        } else ++current_installs;
        pipeline->installed_user = stages[0].user_data[0];
        pipeline->installed_flat = validate ? stages[0].flattened :
            pipeline_cache.memory.at(stages[0].user_data[0]);
        return true;
    }
    const ComputePipeline* AcquireComputePipelineLegacy();
    const ComputePipeline* AcquireComputePipelineCandidate();
};
#include "compute_policy.inc"
} // namespace Vulkan

static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(9); }
}
static bool Current(const Vulkan::ComputePipeline& p) {
    return p.installed_user == 2 && p.installed_flat == std::vector<u32>({21, 22});
}
int main(int argc, char** argv) {
    using namespace Vulkan;
    const std::string_view mode = argc > 1 ? argv[1] : "candidate";
    // Selected while racing, then queued until the consumer has entered menus.
    {
        ComputePipeline old{1}, current{2}; Rasterizer r;
        r.selection.compute = &old; r.pipeline_cache.current = &current; r.in_race = false;
        const bool legacy_negative = mode == "--legacy-negative";
        const auto* got = legacy_negative ? r.AcquireComputePipelineLegacy() : r.AcquireComputePipelineCandidate();
        Check(got == &current && Current(current), "queued race->menu selection must use current pipeline and bytes");
        Check(r.pipeline_cache.lookups == 1 && r.copies == 1 && r.current_installs == 1 &&
              r.validated_installs == 0, "menu fallback must copy current stage and install without trusting snapshot");
    }
    // The same cached pipeline can retain an old per-race shadow: pointer equality
    // alone must not skip the existing recorder-owned stage restoration.
    {
        ComputePipeline same{3}; same.installed_flat = {91}; Rasterizer r;
        r.selection.compute = &same; r.pipeline_cache.current = &same; r.in_race = false;
        const auto* got = mode == "--legacy-negative-same" ? r.AcquireComputePipelineLegacy() : r.AcquireComputePipelineCandidate();
        Check(got == &same && Current(same), "same-pipeline stale shadow must be refreshed");
        Check(r.current_installs == 1 && r.validated_installs == 0, "same-pipeline uses current install");
    }
    // A null result captured earlier must not keep skipping a now-ready menu dispatch.
    {
        ComputePipeline current{4}; Rasterizer r;
        r.selection.compute = nullptr; r.pipeline_cache.current = &current; r.in_race = false;
        const auto* got = mode == "--legacy-negative-null" ? r.AcquireComputePipelineLegacy() : r.AcquireComputePipelineCandidate();
        Check(got == &current && Current(current), "queued null selection must be retried in menus");
    }
    // Existing no-selection and cache-miss fallback behavior remains intact.
    {
        ComputePipeline current{5}; Rasterizer r;
        r.selected_pipeline = nullptr; r.pipeline_cache.current = &current; r.in_race = false;
        Check(r.AcquireComputePipelineCandidate() == &current && Current(current), "no selection fallback");
        r.pipeline_cache.current = nullptr;
        Check(r.AcquireComputePipelineCandidate() == nullptr, "current lookup miss still skips safely");
    }
    // In-race valid snapshots retain the exact zero-lookup fast path.
    {
        ComputePipeline old{6}, current{7}; Rasterizer r;
        r.selection.compute = &old; r.pipeline_cache.current = &current;
        Check(r.AcquireComputePipelineCandidate() == &old && old.installed_flat == std::vector<u32>({11, 12}), "race fast path keeps owned snapshot");
        Check(r.pipeline_cache.lookups == 0 && r.copies == 0 && r.current_installs == 0 && r.validated_installs == 1,
              "race fast path adds no cache lookup or current-stage copy");
        Check(r.select_stats.verified == 0, "Release does not restore sampled probes");
    }
    // Repeated policy transitions, capture mode and disabled pipelining all obey
    // the current consumer policy, without destroying the queued snapshot.
    {
        ComputePipeline old{8}, current{9}; Rasterizer r;
        r.selection.compute = &old; r.pipeline_cache.current = &current; r.read_capture = true;
        for (unsigned i = 0; i < 6; ++i) {
            r.in_race = i % 2 == 0;
            const auto* got = r.AcquireComputePipelineCandidate();
            Check(got == (r.in_race ? &old : &current), "repeated transitions respect policy");
        }
        Check(r.pipeline_cache.lookups == 3 && r.current_installs == 3 && r.validated_installs == 3, "only out-of-race calls look up current state");
        r.in_race = true; r.pipelined = false;
        Check(r.AcquireComputePipelineCandidate() == &current && Current(current), "disabled pipeline mode also restores current state");
    }
    // Existing selected-stage validation failure still uses the ordinary fallback.
    {
        ComputePipeline old{10}, current{11}; Rasterizer r;
        r.selection.compute = &old; r.pipeline_cache.current = &current; r.reject_selected = true;
        Check(r.AcquireComputePipelineCandidate() == &current && Current(current), "in-race validation failure fallback");
        Check(r.select_stats.mismatched == 0, "Release selection metrics remain disabled");
        Check(r.pipeline_cache.lookups == 1 && r.copies == 1 && r.current_installs == 1 &&
              r.validated_installs == 1 && Current(current), "validation failure retains lookup and fresh current-stage installation");
    }
    std::puts("PASS: actual extracted Release consumer; transitions, stale shadows, null selection and race fast path");
}
