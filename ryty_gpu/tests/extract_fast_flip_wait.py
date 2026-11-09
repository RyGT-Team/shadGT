from pathlib import Path
import sys
s=Path(sys.argv[1]).read_text(encoding='utf-8')
a=s.index('        auto now = Clock::now();',s.index('void VideoOutDriver::PresentThread'))
b=s.index('\n\n        if (DebugState.IsGuestThreadsPaused())',a)
Path(sys.argv[2]).write_text(s[a:b]+'\n')

a=s.index('    const auto early_allowed = [&] {',s.index('void VideoOutDriver::PresentThread'))
b=s.index('\n    };',a)+7
Path(sys.argv[3]).write_text(s[a:b]+'\n')
