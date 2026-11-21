// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include "common/recoverable.h"

namespace Shader {

/// FIX-SGT-094: the SRT walker storage has no room for a walker of `bytes`. A RecoverableFailure
/// like the storage's other errors; its callers keep the size and translate again only once
/// SrtWalkerMayFit says a walker that large could fit (srt.h).
class SrtStorageFull : public Common::RecoverableFailure {
public:
    explicit SrtStorageFull(std::size_t bytes_)
        : Common::RecoverableFailure{"SRT executable storage exhausted"}, bytes{bytes_} {}
    std::size_t bytes;
};

} // namespace Shader
