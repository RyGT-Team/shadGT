// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace Storage {

// Readers must see either the previous complete blob or the replacement, even if the
// filesystem reports a delayed error when the final buffered bytes are flushed at close.
template <typename Writer>
std::error_code PublishShaderBlob(const std::filesystem::path& target, Writer write) {
    static std::atomic<std::uint64_t> sequence{};
    std::filesystem::path temporary;
    std::FILE* file;
    do {
        temporary = target;
        temporary += ".tmp-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
        file = _wfopen(temporary.c_str(), L"wbx");
#else
        file = std::fopen(temporary.c_str(), "wbx");
#endif
    } while (!file && errno == EEXIST);
    if (!file) {
        return {errno, std::generic_category()};
    }

    std::error_code error;
    errno = 0;
    if (!write(file)) {
        error = {errno ? errno : EIO, std::generic_category()};
    }
    if (std::fclose(file) != 0 && !error) {
        error = {errno ? errno : EIO, std::generic_category()};
    }
    if (!error) {
        std::filesystem::rename(temporary, target, error);
    }
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return error;
}

} // namespace Storage
