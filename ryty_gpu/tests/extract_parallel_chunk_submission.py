#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract renderer control flow; mocks supply only its external dependencies."""
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")
rotation = Path(sys.argv[2]).read_text(encoding="utf-8")
output = Path(sys.argv[3])
output.mkdir(parents=True, exist_ok=True)

def emit(name, text, start, end):
    first = text.index(start)
    last = text.index(end, first)
    (output / name).write_text(text[first:last], encoding="utf-8")

emit("chunk-closure-types.inc", source, "enum PassCloseReason : u32", "static bool CanInlineContinuation(")
emit("chunk-end-rendering.inc", source, "void NativeRenderer::EndRendering(u32 reason)",
     "bool NativeRenderer::ContinueSmallParallelPass(")
emit("chunk-close-before-draw.inc", source, "        u32 close_reason=0;", "        const bool split_now=")
emit("chunk-rotate-segment.inc", rotation, "template<class ResetState>", "\n}\n}")
# Keep the template's closing brace, excluding the namespace's closing brace.
with (output / "chunk-rotate-segment.inc").open("a", encoding="utf-8") as handle:
    handle.write("\n}\n")
