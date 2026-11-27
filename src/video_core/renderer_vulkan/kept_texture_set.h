// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <unordered_set>
#include <variant>
#include <tsl/robin_set.h>
#include "common/fast_hash.h"

namespace Native {
class ImageEntry;
}

namespace Vulkan {

class KeptTextureSet {
    using Key = const Native::ImageEntry*;
    using Stock = std::unordered_set<Key>;
    using Flat = tsl::robin_set<Key, Common::PointerHash>;
    std::variant<Stock, Flat> entries;

public:
    KeptTextureSet() {
        const char* value = std::getenv("SHADGT_FLAT_KEPT_TEXTURES"); // PERF-SGT-225
        if (value && value[0] == '1' && value[1] == '\0') {
            entries.emplace<Flat>();
        }
    }

    bool contains(Key image) const {
        return std::visit([image](const auto& set) { return set.contains(image); }, entries);
    }

    void insert(Key image) {
        std::visit([image](auto& set) { set.insert(image); }, entries);
    }

    std::size_t erase(Key image) {
        return std::visit([image](auto& set) { return set.erase(image); }, entries);
    }

    void clear() {
        std::visit([](auto& set) { set.clear(); }, entries);
    }
};

} // namespace Vulkan
