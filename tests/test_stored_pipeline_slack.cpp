// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// FIX-SGT-135: a pipeline recipe is written with the bytes nothing reads zeroed, exactly as the
// audit of the shipped shader list cleans one (nettoyer() in audit-liste-v1.py).

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/stored_pipeline_slack.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace {

template <typename T>
using VertexInputs = boost::container::static_vector<T, 32>;

// The key's size on the PC whose recipes the audit read (GT7 1.71, RTX 4070): its tables start at
// 384 = 8 + 376. Its bytes are all read, so the test only needs their place.
struct Key {
    std::array<u8, 376> bytes;
};

// SerializationSupport's stored members, in the order Serialize writes them. Filled and read in
// place (no copies), so padding bytes stay as they were made.
struct Support {
    VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
    VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
    vk::PipelineMultisampleStateCreateInfo multisampling{};
    std::vector<u32> tcs{};
    std::vector<u32> tes{};
};

template <typename T>
void Put(std::vector<u8>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

// A stored graphics pipeline file as RegisterPipelineData writes it: the objects' bytes.
std::vector<u8> StoredFile(const Key& key, const Support& s) {
    std::vector<u8> out;
    Put(out, u32{7}); // PipelineKeyVersion: not read here
    Put(out, u32{0}); // graphics
    Put(out, key);
    Put(out, s.vertex_attributes);
    Put(out, s.vertex_bindings);
    Put(out, s.divisors);
    Put(out, s.multisampling);
    Put(out, s.tcs.size());
    for (const u32 word : s.tcs) Put(out, word);
    Put(out, s.tes.size());
    for (const u32 word : s.tes) Put(out, word);
    return out;
}

// What LoadStoredGraphicsPipeline reads back from it (Deserialize reads the objects' bytes).
void Load(const std::vector<u8>& file, Support& s) {
    size_t at = 2 * sizeof(u32) + sizeof(Key);
    const auto get = [&](auto& object) {
        std::memcpy(static_cast<void*>(&object), file.data() + at, sizeof(object));
        at += sizeof(object);
    };
    get(s.vertex_attributes);
    get(s.vertex_bindings);
    get(s.divisors);
    get(s.multisampling);
}

// nettoyer() of C:\RyGT\outils\audit-liste-v1.py, with its constants: a 1,624-byte recipe; three
// tables (start, bytes, element) ending with their 8-byte count, zeroed past it; the multisample
// state at 1,560, its bytes 4 to 8 zeroed; its two pointers and the helper shaders already zero.
std::vector<u8> Nettoyer(std::vector<u8> x) {
    EXPECT_EQ(x.size(), 1624u);
    EXPECT_TRUE(std::all_of(x.begin() + 1608, x.end(), [](u8 b) { return b == 0; }));
    constexpr std::array<std::array<size_t, 3>, 3> tables{
        {{384, 520, 16}, {904, 392, 12}, {1296, 264, 8}}};
    for (const auto& [start, bytes, element] : tables) {
        u64 n{};
        std::memcpy(&n, x.data() + start + bytes - 8, sizeof(n));
        EXPECT_LE(n * element, bytes - 8);
        std::fill(x.begin() + start + n * element, x.begin() + start + bytes - 8, u8{0});
    }
    constexpr size_t multi = 1560;
    std::fill(x.begin() + multi + 4, x.begin() + multi + 8, u8{0});
    EXPECT_TRUE(std::all_of(x.begin() + multi + 8, x.begin() + multi + 16, [](u8 b) { return b == 0; }));
    EXPECT_TRUE(std::all_of(x.begin() + multi + 32, x.begin() + multi + 40, [](u8 b) { return b == 0; }));
    return x;
}

// A pipeline as a build leaves it: each table filled to its 32 slots with other values first, then
// cut to its count (the slots past it keep them; no byte of them is zero), and the multisample
// state's padding left as the memory was.
void Build(Support& s, u32 attributes, u32 bindings, u32 divisors) {
    for (u32 i = 0; i < 32; ++i) {
        s.vertex_attributes.push_back({.location = 0x5A5A5A5Au + i, .binding = 0x5B5B5B5Bu,
                                       .format = vk::Format(0x5C5C5C5C), .offset = 0x5D5D5D5Du});
        s.vertex_bindings.push_back({.binding = 0x6A6A6A6Au + i, .stride = 0x6B6B6B6Bu,
                                     .inputRate = vk::VertexInputRate(0x6C6C6C6C)});
        s.divisors.push_back({.binding = 0x7A7A7A7Au + i, .divisor = 0x7B7B7B7Bu});
    }
    s.vertex_attributes.resize(attributes);
    s.vertex_bindings.resize(bindings);
    s.divisors.resize(divisors);
    for (u32 i = 0; i < attributes; ++i) {
        s.vertex_attributes[i] = {.location = i, .binding = i / 2,
                                  .format = vk::Format::eR32G32B32A32Sfloat, .offset = 16 * i};
    }
    for (u32 i = 0; i < bindings; ++i) {
        s.vertex_bindings[i] = {.binding = i, .stride = 48, .inputRate = vk::VertexInputRate::eVertex};
    }
    for (u32 i = 0; i < divisors; ++i) {
        s.divisors[i] = {.binding = i, .divisor = 1};
    }
    std::memset(static_cast<void*>(&s.multisampling), 0xD5, sizeof(s.multisampling));
    s.multisampling.sType = vk::StructureType::ePipelineMultisampleStateCreateInfo;
    s.multisampling.pNext = nullptr;
    s.multisampling.flags = {};
    s.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e4;
    s.multisampling.sampleShadingEnable = VK_TRUE;
    s.multisampling.minSampleShading = 0.25f;
    s.multisampling.pSampleMask = nullptr;
    s.multisampling.alphaToCoverageEnable = VK_TRUE;
    s.multisampling.alphaToOneEnable = VK_FALSE;
}

Key SomeKey() {
    Key key{};
    for (size_t i = 0; i < key.bytes.size(); ++i) key.bytes[i] = u8(i * 7 + 1);
    return key;
}

std::vector<u8> RawFile(u32 attributes, u32 bindings, u32 divisors) {
    Support s{};
    Build(s, attributes, bindings, divisors);
    return StoredFile(SomeKey(), s);
}

// The export's step: the file, and what LoadStoredGraphicsPipeline read from it.
bool Clear(std::vector<u8>& file) {
    Support loaded{};
    Load(file, loaded);
    const bool cleared = Vulkan::ClearStoredPipelineSlack<Key>(file, loaded);
    // A count past the slots must not reach the destructor.
    std::memset(static_cast<void*>(&loaded.vertex_attributes), 0, sizeof(loaded.vertex_attributes));
    std::memset(static_cast<void*>(&loaded.vertex_bindings), 0, sizeof(loaded.vertex_bindings));
    std::memset(static_cast<void*>(&loaded.divisors), 0, sizeof(loaded.divisors));
    return cleared;
}

void ExpectWrittenAsCleaned(u32 attributes, u32 bindings, u32 divisors) {
    SCOPED_TRACE(testing::Message() << attributes << " " << bindings << " " << divisors);
    const auto raw = RawFile(attributes, bindings, divisors);
    ASSERT_EQ(raw.size(), 1624u);
    auto written = raw;
    ASSERT_TRUE(Clear(written));
    EXPECT_EQ(written, Nettoyer(raw));
    // There were leftovers to clear: the padding's at least.
    EXPECT_NE(written, raw);
    // Nothing read moved: the same pipeline loads back from it.
    Support before{}, after{};
    Load(raw, before);
    Load(written, after);
    EXPECT_TRUE(std::ranges::equal(before.vertex_attributes, after.vertex_attributes));
    EXPECT_TRUE(std::ranges::equal(before.vertex_bindings, after.vertex_bindings));
    EXPECT_TRUE(std::ranges::equal(before.divisors, after.divisors));
    EXPECT_TRUE(before.multisampling == after.multisampling);
    EXPECT_TRUE(std::equal(raw.begin(), raw.begin() + 384, written.begin()));
    // Written twice, the same bytes.
    auto again = written;
    ASSERT_TRUE(Clear(again));
    EXPECT_EQ(again, written);
}

TEST(StoredPipelineSlack, EmptyTablesAreWrittenAsTheAuditCleansThem) {
    // As GT7's recipes have them (dynamic vertex inputs): 1,152 bytes of slots, and the padding.
    const auto raw = RawFile(0, 0, 0);
    auto written = raw;
    ASSERT_TRUE(Clear(written));
    size_t changed = 0;
    for (size_t i = 0; i < raw.size(); ++i) changed += raw[i] != written[i];
    EXPECT_EQ(changed, 512u + 384u + 256u + 4u);
    ExpectWrittenAsCleaned(0, 0, 0);
}

TEST(StoredPipelineSlack, PartlyFilledTablesAreWrittenAsTheAuditCleansThem) {
    ExpectWrittenAsCleaned(3, 2, 1);
    ExpectWrittenAsCleaned(31, 1, 0);
}

TEST(StoredPipelineSlack, FullTablesKeepEverySlot) {
    ExpectWrittenAsCleaned(32, 32, 32);
}

TEST(StoredPipelineSlack, AFileItCannotReadIsLeftAsItWas) {
    // A count beyond the table's slots.
    {
        auto raw = RawFile(2, 2, 2);
        const u64 count = 33;
        std::memcpy(raw.data() + 384 + 520 - 8, &count, sizeof(count));
        auto file = raw;
        EXPECT_FALSE(Clear(file));
        EXPECT_EQ(file, raw);
    }
    // A pointer in the multisample state.
    {
        Support s{};
        Build(s, 1, 1, 0);
        static const u32 mask = ~0u;
        s.multisampling.pSampleMask = &mask;
        const auto raw = StoredFile(SomeKey(), s);
        auto file = raw;
        EXPECT_FALSE(Clear(file));
        EXPECT_EQ(file, raw);
    }
    // A pipeline with helper shaders (never exported): another length.
    {
        Support s{};
        Build(s, 1, 1, 0);
        s.tcs = {0x07230203u, 1u};
        const auto raw = StoredFile(SomeKey(), s);
        auto file = raw;
        EXPECT_FALSE(Clear(file));
        EXPECT_EQ(file, raw);
    }
}

} // namespace
