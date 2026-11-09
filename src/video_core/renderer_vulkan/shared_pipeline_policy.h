#pragma once
#include <cstdlib>
namespace Vulkan {
// Port of MR14 PERF-GT7-040: enabled by default, explicitly incompatible
// with retained LTO data for the heavy tessellation path.
inline bool Mr14SharedTessellationEnabled(bool lto) {
    static const bool enabled = [] {
        const char* env=std::getenv("RYGT_GPL");
        return !(env && env[0]=='0');
    }();
    return enabled && !lto;
}
// AMD-320 (from RyGTS's former CI overlay): the same libraries and fast link as above, but
// AMD's compiler keeps its optimizer for tessellated libraries. AMD-322: no link-time data and
// no second, optimized link: on an RX 7600 they made each menu pipeline take 0.4-0.7 s and
// filled pipeline memory (4.4 GB, then trimming and rebuilding pipelines in a loop).
// RYGT_AMD_PIPELINE_POLICY=0 uses the shared policy.
inline bool AmdPipelinePolicyEnabled(unsigned vendor_id) {
    static const bool allowed = [] {
        const char* env=std::getenv("RYGT_AMD_PIPELINE_POLICY");
        return !(env && env[0]=='0');
    }();
    return allowed && vendor_id == 0x1002;
}
inline bool SharedPipelineLibrariesEnabled(bool supported, bool preloading) {
    static const bool no_libraries = [] {const char* p=std::getenv("SHADGT_NO_LIBRARIES");return p&&p[0]=='1';}();
    static const bool whole_prewarm = [] {const char* p=std::getenv("RYGT_PREWARM_WHOLE_PIPELINES");return p&&p[0]=='1';}();
    return supported && !no_libraries && !(preloading && whole_prewarm);
}
// Relax only splash-screen dispatch admission. The driver-call semaphore remains
// mandatory for every tessellated pre-raster library, including direct builds.
constexpr bool QueueWholeTessellation(bool tessellated, bool preloading, bool startup,
                                      bool libraries) {
    return tessellated && !(preloading && startup && libraries);
}
struct SharedPipelinePolicy {
    bool libraries;
    bool disable_optimization;
    bool background_optimization;
    static constexpr SharedPipelinePolicy Select(bool supported, bool force_whole,
                                                 bool tessellated, bool optimize,
                                                 bool emulated_tessellation = false) {
        const bool libraries = supported && !force_whole;
        return {libraries, tessellated,
                libraries && !tessellated && !emulated_tessellation && optimize};
    }
    // AMD-320/322: optimizer kept, and no pipeline keeps link-time data.
    static constexpr SharedPipelinePolicy SelectAmd(bool supported, bool force_whole) {
        return {supported && !force_whole, false, false};
    }
};
}
