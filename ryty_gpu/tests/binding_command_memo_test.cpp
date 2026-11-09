#include "video_core/renderer_vulkan/binding_command_memo.h"
#include "video_core/renderer_vulkan/pipeline_binding_state.h"
#include <array>
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "binding memo failure at line %d\n", __LINE__); std::exit(1); \
} } while (0)

int main() {
    using Push = std::array<unsigned, 4>;
    using Memo = Vulkan::BindingCommandMemo<std::uint64_t, std::uint64_t, Push, 5>;
    Memo memo;
    Vulkan::PipelineBindingState<unsigned> pipeline;
    unsigned push_calls = 0, set_calls = 0, pushed_calls = 0;
    Push actual_push{};
    std::uint64_t actual_set{};
    std::array<std::uint64_t, 3> actual_pushed{};
    const Push constant_push{1, 2, 3, 4};
    const std::array<std::uint64_t, 3> constant_words{4, 8, 16};
    for (unsigned i = 0; i < 10000; ++i) {
        memo.Prepare(5, 0, 3, pipeline.InvalidationEpoch());
        if (!memo.SamePush(constant_push)) {
            actual_push = constant_push; ++push_calls; memo.NotePush(constant_push);
        }
        if (!memo.SameSet(1, 17)) { actual_set = 17; ++set_calls; memo.NoteSet(1, 17); }
        if (!memo.SamePushed(2, constant_words)) {
            actual_pushed = constant_words; ++pushed_calls; memo.NotePushed(2, constant_words);
        }
        CHECK(actual_push == constant_push && actual_set == 17 && actual_pushed == constant_words);
    }
    CHECK(push_calls == 1 && set_calls == 1 && pushed_calls == 1);
    auto copy = constant_words;
    memo.NotePushed(2, copy);
    copy[0] ^= 99;
    CHECK(memo.SamePushed(2, constant_words) && !memo.SamePushed(2, copy));
    for (const auto changes : {std::array<std::uint64_t, 4>{6, 0, 3, 0},
                               std::array<std::uint64_t, 4>{6, 1, 3, 0},
                               std::array<std::uint64_t, 4>{6, 1, 4, 0},
                               std::array<std::uint64_t, 4>{6, 1, 4, 1}}) {
        memo.Prepare(changes[0], unsigned(changes[1]), changes[2], changes[3]);
        CHECK(!memo.SamePush(constant_push) && !memo.SameSet(1, 17) &&
              !memo.SamePushed(2, constant_words));
        memo.NotePush(constant_push); memo.NoteSet(1, 17); memo.NotePushed(2, constant_words);
    }
    memo.NoteSet(2, 21);
    CHECK(!memo.SamePushed(2, constant_words));
    memo.NotePushed(2, constant_words);
    CHECK(!memo.SameSet(2, 21));
    const auto epoch = pipeline.InvalidationEpoch();
    pipeline.ResetCounts();
    CHECK(pipeline.InvalidationEpoch() == epoch);
    pipeline.Invalidate();
    memo.Prepare(5, 0, 3, pipeline.InvalidationEpoch());
    CHECK(!memo.SamePush(constant_push) && !memo.SameSet(1, 17) &&
          !memo.SamePushed(2, constant_words));

    // Randomized replay against a driver-state model. Foreign helper commands
    // and new lists destroy known state; layout, bind-point and backing changes
    // must force fresh commands. Diagnostic resets must leave them reusable.
    std::uint64_t rng = 0x75bcd15, generation = 3;
    unsigned emitted = 0;
    for (unsigned i = 0; i < 200000; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        if (rng % 11 == 0) {
            pipeline.Invalidate();
            actual_push.fill(99); actual_set = 99; actual_pushed.fill(99);
        }
        if (rng % 31 == 0) ++generation;
        if (rng % 17 == 0) pipeline.ResetCounts();
        const std::uint64_t layout = (rng >> 8) % 7 + 1;
        const unsigned point = rng & 1;
        memo.Prepare(layout, point, generation, pipeline.InvalidationEpoch());
        const Push desired{unsigned(rng % 3), unsigned(rng >> 12), point, i % 7};
        const std::uint64_t set = (rng >> 16) % 13 + 1;
        const std::array<std::uint64_t, 3> words{rng >> 17, rng >> 25, rng & 7};
        if (!memo.SamePush(desired)) { actual_push = desired; ++emitted; memo.NotePush(desired); }
        if (!memo.SameSet(1, set)) { actual_set = set; ++emitted; memo.NoteSet(1, set); }
        if (!memo.SamePushed(2, words)) {
            actual_pushed = words; ++emitted; memo.NotePushed(2, words);
        }
        CHECK(actual_push == desired && actual_set == set && actual_pushed == words);
        // A call that failed before emission must not be recorded as committed.
        memo.ForgetSet(1);
        CHECK(!memo.SameSet(1, set));
    }
    std::printf("PASS: 200000 mixed replay cases; repeated commands 30000 -> 3; emitted=%u\n", emitted);
}
