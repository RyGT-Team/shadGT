// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Gt7CarSign {
// Guest UI recording thread only. The draw packet carries the scope to the GPU thread.
extern thread_local bool recording;
} // namespace Gt7CarSign
