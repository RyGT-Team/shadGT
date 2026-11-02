from pathlib import Path
import hashlib, sys
s=Path(sys.argv[1]).read_text(encoding='utf-8')
a=s.index('    // The selected dirty pages are already sorted and unique.')
b=s.index('        // The runs of 16-byte pieces the CPU changed',a)
c=s.index('        ++stats.region_partial_pages;',b)
d=s.index('    s.buffers_written = true;',c)
assert hashlib.sha256(s[a:b].encode()).hexdigest()=='77e4dedc20439922e5a7910c0928bdeb67f386b40c1fc9c9d97179d02ba0a3e3', 'snapshot detach, rearm, guarded copy or callback changed'
assert hashlib.sha256(s[c:d].encode()).hexdigest()=='f2d9ad11f9ff6e66552545d387867b99970e251961d722e9aab66f6f897ea6c5', 'post-scan accounting changed'
Path(sys.argv[2]).write_text(s[b:c],encoding='utf-8')
