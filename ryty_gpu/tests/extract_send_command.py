from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("    template <bool wait_done = false>\n    void SendCommand(")
end = source.index("\n    void ReserveCopyBufferSpace()", start)
Path(sys.argv[2]).write_text(source[start:end], encoding="utf-8")
