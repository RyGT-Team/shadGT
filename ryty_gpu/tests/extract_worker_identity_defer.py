"""Extract the actual deferred identity predicate/count proof and reconstruct eager order."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
declaration = source.index('    PreparedDraw::CpuProof* worker_result=nullptr;')
comment = source.index('    // Producer totals', declaration)
declarations = source[declaration:comment]
proof_start = source.index('    const auto set_count_proof=', comment)
proof_end = source.index('\n    };', proof_start) + len('\n    };')
proof = source[proof_start:proof_end]
branch = source.index('        } else {\n        whole_phases.Next(1);', proof_end)
setup_start = source.index('        worker_result = parallel && prepared && from_prepared', branch)
setup_end = source.index('        const auto& db=', setup_start)
setup = source[setup_start:setup_end]
assert 'worker_identity = worker_result' in setup
assert source.index('        const bool native_receipt_valid=', setup_end) > setup_end
assert source.index('        if(fast_reject) {', proof_end) < branch < setup_start
common = '''
    auto* prepared=c.prepared; auto* parallel=c.parallel;
    const bool from_prepared=c.from_prepared;
    const auto* pipeline=&c.pipeline; auto& b=c.bindings;
    BindingCapture parallel_bindings{};
'''
finish = '''
    return {worker_identity,parallel_bindings.counts_proven,
            parallel_bindings.ordinary_writes,parallel_bindings.ordinary_buffers,
            parallel_bindings.ordinary_images,c.pipeline.layout_reads};
}
'''
current = 'Result Current(Case& c,bool fast_reject) {' + common + declarations + proof + '\n    if(!fast_reject) {\n' + setup + '        set_count_proof();\n    }\n' + finish
eager = 'Result Eager(Case& c,bool fast_reject) {' + common + declarations + setup + proof + '\n    if(!fast_reject) set_count_proof();\n' + finish
Path(sys.argv[2]).write_text(eager + '\n' + current)
