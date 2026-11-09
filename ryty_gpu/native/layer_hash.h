// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include <utility>

namespace Native {
// Hashes correspond to the most recently recorded upload, not the last scan.
class LayerHashState {
public:
    template<class Hash>
    void Capture(std::uint32_t layers, std::uint64_t layer_size, Hash hash, bool whole = true) {
        if (!layers || layer_size < 256) {
            sampled.clear(); uploaded.clear(); cursor = 0;
            return;
        }
        std::vector<std::uint64_t> next;
        std::vector<std::uint64_t> next_samples;
        next.reserve(layers);
        next_samples.reserve(layers);
        for (std::uint32_t i = 0; i < layers; ++i) {
            if (whole) next.push_back(hash(i, 0, layer_size));
            std::uint64_t sample = 0;
            for (std::uint32_t p = 0; p < 4; ++p)
                sample = sample * 1099511628211ull ^ hash(i, layer_size / 4 * p, 64);
            next_samples.push_back(sample);
        }
        uploaded = std::move(next);
        sampled = std::move(next_samples);
        cursor = 0;
    }
    bool Checked(std::uint64_t frame) const { return last_frame == frame; }
    template<class Hash>
    bool Changed(std::uint32_t layers, std::uint64_t layer_size, std::uint64_t frame, Hash hash,
                 bool samples, bool whole) {
        if (!layers || layer_size < 256 || last_frame == frame) return false;
        last_frame = frame;
        bool changed = false;
        if (samples) {
            const bool first = sampled.size() != layers;
            sampled.resize(layers);
            for (std::uint32_t i = 0; i < layers; ++i) {
                std::uint64_t value = 0;
                for (std::uint32_t p = 0; p < 4; ++p)
                    value = value * 1099511628211ull ^ hash(i, layer_size / 4 * p, 64);
                changed |= !first && sampled[i] != value;
                sampled[i] = value;
            }
        }
        if (whole && uploaded.size() == layers) {
            const auto count = std::clamp<std::uint64_t>((4ull << 20) / layer_size, 1, layers);
            for (std::uint64_t n = 0; n < count; ++n) {
                const auto i = cursor;
                cursor = (cursor + 1) % layers;
                changed |= hash(i, 0, layer_size) != uploaded[i];
            }
        }
        return changed;
    }
    void ClearUpload() { uploaded.clear(); cursor = 0; }
private:
    std::vector<std::uint64_t> sampled, uploaded;
    std::uint64_t last_frame = UINT64_MAX;
    std::uint32_t cursor = 0;
};
}
