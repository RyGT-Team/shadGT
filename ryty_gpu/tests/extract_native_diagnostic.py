"""Keep extracted native functions in their actual diagnostic macro context."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
marker = "#if defined(NDEBUG)\n#define NATIVE_DIAGNOSTIC"
assert source.count(marker) == 1
start = source.index(marker)
end = source.index("\n#endif", start) + len("\n#endif")
Path(sys.argv[2]).write_text("#pragma once\n" + source[start:end] + "\n", encoding="utf-8")
