import sys
from pathlib import Path
s=Path(sys.argv[1]).read_text();a=s.index("        if(draw.vertex_attribute_count>32");b=s.index("        static_assert(Shader::Backend::MaxDescriptorSets",a);Path(sys.argv[2]).write_text(s[a:b])
