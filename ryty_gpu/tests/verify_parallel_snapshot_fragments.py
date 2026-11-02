# SPDX-License-Identifier: GPL-2.0-or-later
"""Ensure the portable mixed-snapshot test exercises the renderer fragments."""
from pathlib import Path
import sys

source, test, oom_test, action_test = (Path(path).read_text(encoding="utf-8") for path in sys.argv[1:])
producer = source.split("        const u32 push=pipeline.PushSet();\n        if(candidate->descriptor_structure", 1)[1].split("\n    } catch(const std::bad_alloc&)", 1)[0]
consumer = source.split("                        const auto snapshot_matches=[&] {", 1)[1].split("\n                    }\n                }\n            } catch", 1)[0]
for name, fragment in (("producer", producer), ("consumer", consumer)):
    if fragment not in test:
        raise SystemExit(f"{name} snapshot fragment differs from renderer source")
oom = source.split("    if (ParallelRecordingRequested()) try {", 1)[1].split("    if(auto context=ahead_guard.context;", 1)[0]
if oom not in oom_test:
    raise SystemExit("producer OOM test fragment differs from renderer source")
action_producer = source.split("        candidate->descriptor_structure=true;", 1)[1].split("        if(p->has_vertices && p->vertex_index) {", 1)[0]
action_query = source.split("                            const bool recorded=TimedProofInvoke", 1)[1].split("                            const auto retained_scan=", 1)[0]
for name, fragment in (("action producer", action_producer), ("action query", action_query)):
    if fragment not in action_test:
        raise SystemExit(f"{name} test fragment differs from renderer source")
print("PASS mixed snapshot, action and OOM test fragments match renderer source")
