from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("    template <bool wait_done = false>\n    void SendCommand(")
end = source.index("\n    void ReserveCopyBufferSpace()", start)
Path(sys.argv[2]).write_text(source[start:end], encoding="utf-8")
if len(sys.argv)>4:
    source = Path(sys.argv[3]).read_text(encoding="utf-8")
    start = source.index("void Liverpool::ProcessCommands()")
    end = source.index("\nvoid Liverpool::Process(", start)
    Path(sys.argv[4]).write_text(source[start:end], encoding="utf-8")
