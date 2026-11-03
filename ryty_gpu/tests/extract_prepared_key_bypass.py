from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text(encoding='utf-8')
a=s.index('    const bool prepared_buffer_keys_unchanged = from_prepared')
b=s.index('\n',a)
c=s.index('        const bool key_changed = prepared_buffer_keys_unchanged ? false')
d=s.index('\n        const auto& key = prepared_key.words;',c)
Path(sys.argv[2]).write_text(s[a:b]+'\n')
Path(sys.argv[3]).write_text(s[c:d]+'\n')

a=s.index('void NativeRenderer::DrawBindings::PrepareDescriptorKeys(')
b=s.index('\nvoid NativeRenderer::DrawBindings::PrepareWriteGroups()',a)
Path(sys.argv[4]).write_text(s[a:b]+'\n')
a=s.index('    if (b.key_layout != pipeline.GetLayout()) b.PrepareDescriptorKeys(pipeline);')
Path(sys.argv[5]).write_text(s[a:s.index('\n',a)]+'\n')
