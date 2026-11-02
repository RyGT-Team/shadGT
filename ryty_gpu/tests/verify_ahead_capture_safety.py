# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the live renderer's proof and flight ordering and exercise their decisions."""
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text(encoding="utf-8")
proof = s.split("    const auto set_count_proof=[&] {", 1)[1].split("    };", 1)[0]
for term in ("!worker_identity", "!worker_result->descriptor_structure", "!worker_result->counts_valid",
             "!b.groups_ready", "worker_result->counts_push!=pipeline->PushSet()",
             "worker_result->group_data[i]!=b.grouped_writes[i].data()",
             "worker_result->group_sizes[i]!=b.grouped_writes[i].size()"):
    assert term in proof, term

def usable(identity, structure, valid, ready, saved_push, current_push, saved, current):
    return identity and structure and valid and ready and saved_push == current_push and saved == current
base = (True, True, True, True, 2, 2, [(1, 3), (2, 4)], [(1, 3), (2, 4)])
assert usable(*base)
for position, value in ((0, False), (1, False), (2, False), (3, False), (5, 1),
                        (7, [(9, 3), (2, 4)]), (7, [(1, 2), (2, 4)])):
    args = list(base); args[position] = value
    assert not usable(*args)
flight = s.split("            // Retain before PushBindings:", 1)[1].split("            static_assert(Shader::Backend::MaxDescriptorSets<=32);", 1)[0]
retain = flight.index("parallel->ahead_flights[ahead_slot].lease=ahead")
call = flight.index("PushBindings(*pipeline")
cancel = flight.index("flight.lease.reset();flight.release={};--parallel->ahead_live;")
assert retain < call < cancel
assert "} else if(ahead && ahead_slot<parallel->ahead_flights.size())" in flight
# Failure at PushBindings skips the cancellation; success without use cancels it.
def simulate(throws, used):
    retained = True
    try:
        if throws: raise RuntimeError()
        if not used: retained = False
    except RuntimeError: pass
    return retained
assert simulate(True, False) and simulate(True, True)
assert not simulate(False, False) and simulate(False, True)
assert "if(AheadRequested() && ParallelRecordingRequested() && !r.instance.IsIntel())" in s
assert "if(!where->second->ahead_context->closing.load(std::memory_order_acquire))" in s
assert "if(!sidecar->ahead_context->closing.load(std::memory_order_acquire))" in s
print("PASS ahead capture proof, flight ordering and close guards")
