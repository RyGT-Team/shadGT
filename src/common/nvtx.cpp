// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// DIAG-056 / DIAG-GTS-463: see nvtx.h. The only file that includes the NVTX3 headers.

#include "common/nvtx.h"

#if defined(SHADGT_NVTX) && !defined(NDEBUG)

#ifdef _WIN32
#include <windows.h>
#endif
#include <nvtx3/nvToolsExt.h>

namespace Common::Nvtx {
namespace {

nvtxEventAttributes_t ValueAttributes(const char* name, u64 value) {
    nvtxEventAttributes_t attributes{};
    attributes.version = NVTX_VERSION;
    attributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
    attributes.messageType = NVTX_MESSAGE_TYPE_ASCII;
    attributes.message.ascii = name;
    attributes.payloadType = NVTX_PAYLOAD_TYPE_UNSIGNED_INT64;
    attributes.payload.ullValue = value;
    return attributes;
}

} // namespace

void Mark(const char* name) {
    nvtxMarkA(name);
}

void Mark(const char* name, u64 value) {
    const auto attributes = ValueAttributes(name, value);
    nvtxMarkEx(&attributes);
}

u64 Start(const char* name) {
    return nvtxRangeStartA(name);
}

void End(u64 id) {
    nvtxRangeEnd(id);
}

void Push(const char* name) {
    nvtxRangePushA(name);
}

void Push(const char* name, u64 value) {
    const auto attributes = ValueAttributes(name, value);
    nvtxRangePushEx(&attributes);
}

void Pop() {
    nvtxRangePop();
}

void NameThread(const char* name) {
#ifdef _WIN32
    nvtxNameOsThreadA(GetCurrentThreadId(), name);
#endif
}

} // namespace Common::Nvtx

#endif
