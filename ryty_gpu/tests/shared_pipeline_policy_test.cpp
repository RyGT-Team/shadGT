#include "video_core/renderer_vulkan/shared_pipeline_policy.h"
#include <cassert>
#include <initializer_list>
int main() {
    using Vulkan::SharedPipelinePolicy;
    for (bool supported : {false, true}) for (bool tess : {false, true})
        for (bool whole_guard : {false, true}) for (bool shared_tess : {false, true})
            for (bool optimize : {false, true}) {
                const bool force_whole = tess && (whole_guard || !shared_tess);
                const auto policy = SharedPipelinePolicy::Select(supported, force_whole, tess, optimize);
                assert(policy.libraries == (supported && !force_whole));
                assert(policy.background_optimization == (policy.libraries && !tess && optimize));
                if (tess && whole_guard) assert(!policy.libraries);
            }
}
