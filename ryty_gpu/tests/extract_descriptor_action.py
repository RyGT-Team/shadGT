from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("        struct DescriptorAction {")
end = source.index("\n        static_assert(sizeof(DescriptorAction)==16);", start)
fragment = source[start:end].replace("struct DescriptorAction", "struct NativeDescriptorAction", 1)
Path(sys.argv[2]).write_text(fragment + "\n", encoding="utf-8")
