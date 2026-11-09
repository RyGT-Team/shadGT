"""Extract the real Release consumer and the pre-fix negative-control body."""
import sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
a = source.index('const ComputePipeline* Rasterizer::AcquireComputePipeline() {')
b = source.index('\nvoid Rasterizer::HashDrawPagesAhead(', a)
body = source[a:b]
new = 'if (SelectsPipelinesAhead() && selected_pipeline && selected_pipeline->is_compute) {'
old = 'if (selected_pipeline && selected_pipeline->is_compute) {'
if body.count(new) != 1:
    raise RuntimeError('Expected the production compute consumer policy condition')
legacy = body.replace(new, old, 1)
Path(sys.argv[2]).write_text(
    legacy.replace('Rasterizer::AcquireComputePipeline()', 'Rasterizer::AcquireComputePipelineLegacy()', 1) + '\n' +
    body.replace('Rasterizer::AcquireComputePipeline()', 'Rasterizer::AcquireComputePipelineCandidate()', 1) + '\n')
