#ifdef NDEBUG
#undef NDEBUG
#endif
#include "common/geometry_trace.h"
#include <gtest/gtest.h>
#include <fstream>
#include <thread>

TEST(GeometryTrace, RetainsLatestEventsAndSealsAfterTrigger) {
    _putenv_s("RYGT_GEOMETRY_TRACE", "1");
    auto& state = GeometryTrace::Get();
    state.armed = true;
    for (std::uint64_t i = 0; i < state.events.size() + 7; ++i)
        GeometryTrace::Record("before", i);
    EXPECT_EQ(state.dropped, 7);
    EXPECT_EQ(state.events[state.head].id, 7);
    GeometryTrace::Trigger();
    for (int i = 0; i < 1024; ++i) GeometryTrace::Record("after", i);
    EXPECT_TRUE(state.sealed.load());
    for (size_t i = 1; i < state.count; ++i) {
        const auto& previous = state.events[(state.head + i - 1) % state.events.size()];
        const auto& next = state.events[(state.head + i) % state.events.size()];
        EXPECT_LE(previous.ns, next.ns);
    }
    auto dropped = state.dropped;
    GeometryTrace::Record("ignored", 0);
    EXPECT_EQ(state.dropped, dropped);
    EXPECT_FALSE(GeometryTrace::WriteCsv(state, "."));
    EXPECT_TRUE(state.sealed.load());
    const auto directory = std::filesystem::temp_directory_path() /
        ("rygt-geometry-trace-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const auto output = directory / "trace.csv";
    EXPECT_TRUE(GeometryTrace::WriteCsv(state, output.string().c_str()));
    std::ifstream f{output};
    EXPECT_TRUE(f.good());
    f.close();
    std::filesystem::remove(output);
    std::filesystem::remove(directory);
}

TEST(GeometryTrace, ScopeIsThreadLocalAndRestoresPreviousId) {
    GeometryTrace::current = 11;
    std::uint64_t observed{};
    std::thread thread{[&] { GeometryTrace::Scope scope{22}; observed = GeometryTrace::current; }};
    thread.join();
    EXPECT_EQ(observed, 22);
    EXPECT_EQ(GeometryTrace::current, 11);
    { GeometryTrace::Scope scope{33}; EXPECT_EQ(GeometryTrace::current, 33); }
    EXPECT_EQ(GeometryTrace::current, 11);
}
