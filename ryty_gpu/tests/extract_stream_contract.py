import sys
from pathlib import Path

root=Path(sys.argv[1])
out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
recording=(root/"parallel_pass_recording.cpp").read_text(encoding="utf-8")
a=recording.index("struct PassTraversal {")
b=recording.index("static bool RecordPassImpl",a)
(out/"stream-traversal.inc").write_text(recording[a:b], encoding="utf-8")
header=(root/"parallel_pass_coordinator.h").read_text(encoding="utf-8")
a=header.index("    enum class RejectReason");b=header.index("private:",a)
(out/"stream-coordinator-types.inc").write_text(header[a:b], encoding="utf-8")
a=header.index("    template<class Accept> bool Drain(");b=header.index("    struct WorkerStats",a)
(out/"stream-drain-collect.inc").write_text(header[a:b], encoding="utf-8")
source=(root/"parallel_pass_coordinator.cpp").read_text(encoding="utf-8")
parts=[]
for start,end in [
    ("PassCoordinator::~PassCoordinator()", "std::function<void()> PassCoordinator::MakeRelease"),
    ("void PassCoordinator::CloseSmall(", "bool PassCoordinator::TryPass(u64"),
    ("bool PassCoordinator::TryPass(u64", "bool PassCoordinator::TryPassTransfer("),
]:
    a=source.index(start);b=source.index(end,a);parts.append(source[a:b])
(out/"stream-coordinator-methods.inc").write_text("\n".join(parts), encoding="utf-8")
