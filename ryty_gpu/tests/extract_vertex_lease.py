import sys
from pathlib import Path
source=Path(sys.argv[1]).read_text()
header=Path(sys.argv[2]).read_text()
start=source.index("    // Once published, workers or completion pins")
end=source.index("    VertexIndexState& v = *p.vertex_index;",start)
producer=source[start:end].replace("std::make_shared<VertexIndexState>()", "MakeVertex()")
start=source.index("            if(leaseable_vi) {")
end=source.index("            if(ParallelGateDiagEnabled())",start)
capture=source[start:end].replace('ASSERT_MSG(parallel->sync_pins.Pin(parallel->pin_slot,captured.pipeline_owner),"parallel pipeline owner pin failed");', 'assert(test_pin(captured.pipeline_owner));')
start=header.index("inline bool MinimumPassBytes(")
end=header.index("// Reconstruct the worker-local",start)
Path(sys.argv[3]).write_text(producer)
Path(sys.argv[4]).write_text(capture)
Path(sys.argv[5]).write_text(header[start:end])

start=source.index("    template<class Rows> void Stage(")
end=source.index("    void Flush(",start)
Path(sys.argv[6]).write_text(source[start:end])

recording=Path(sys.argv[7]).read_text()
start=recording.index("bool ValidatePass(")
end=recording.index("bool RecordPass(",start)
Path(sys.argv[8]).write_text(recording[start:end])

start=source.index("    template<bool Proven=false,class Groups>")
end=source.index("    Admission CheckCounts(",start)
Path(sys.argv[9]).write_text(source[start:end])

coordinator=Path(sys.argv[10]).read_text()
start=coordinator.index("    job->loan=loan.index;")
end=coordinator.index("    loan.index=DrawPool::Invalid;++transfer_success_",start)
Path(sys.argv[11]).write_text(coordinator[start:end])

start=source.index("        const bool empty_vi = vi->attributes.empty()")
end=source.index("        if(eligible)",start)
Path(sys.argv[12]).write_text(source[start:end])
