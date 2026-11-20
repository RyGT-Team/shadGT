# Notes for agents working on shadGT

## Ticket IDs: SGT for both games, GTS or GT7 for one

Every change carries a ticket ID of the form `<KIND>-<GAME>-<NNN>` in its commit subject and in
the code comments next to it.

| Part | Values |
|---|---|
| Kind | `FIX`, `PERF`, `DIAG`, `FEAT` (also `GAME`, `NEO` as already in use) |
| Game | `SGT` when the change applies to both games (most renderer work), `GTS` for GT Sport only, `GT7` for GT7 only |
| Number | three digits |

- **SGT has its own count, started at 001 on 2026-11-17** (`FIX-SGT-001`, `FEAT-SGT-002`, ...).
  GTS and GT7 keep the counts they had; numbers are never reused within one tag.
- **Use SGT for anything shared**: pipeline cache, native recorder, regions and mirrors, switches,
  capture tools. Use GTS or GT7 only when the code is gated to one game (a game patch, a hook, a
  per-game default).
- **Before taking a number**, check it is free: `git log --all` subjects, `git grep` of the tree and
  the open and closed PRs' titles (`gh pr list --state all`). Another contributor may have used
  the next number on a branch you have not fetched.
- **Old tickets keep their IDs.** Do not rename `FIX-GTS-…` comments of changes that are already
  pushed; new work on them takes a new SGT number and may name the old one.

## Switch names: SHADGT_, RYGT_ or SHARED_ (FEAT-GTS-415)

From FEAT-GTS-415 on, an environment switch is named after where it takes effect:

| Prefix | Read by | Example |
|---|---|---|
| `SHADGT_` | only the renderer (shadGT: `src/`, `ryty_gpu/`) | `SHADGT_PARALLEL_WORKERS` |
| `RYGT_` | only the runtime (RyGTS: `core/`, `app/`, `plugins/`) | `RYGT_LOG_DIRECTORY` |
| `SHARED_` | both the renderer and the runtime | `SHARED_DIAG` |

- **A new switch** gets the prefix of the code that reads it. Never add a new `RYGT_` switch to the
  renderer or a `SHADGT_` one to the runtime.
- **A switch that starts being read on the other side too** is renamed to `SHARED_` in every
  repository (renderer, runtime, launcher, docs), and added to `SHARED` in the renderer's
  `tools/switch_prefix.py`.
- **The rename is a hard one**: the old names are not read any more. At start the launchers list
  switches saved under an old name in their obsolete-settings question and rename them on Yes
  (`SwitchWalker.FindRenames`), so players keep their settings.
- **A branch from before the rename** brings old names back on merge. Run the tool there after
  merging; a second run changes nothing:

  ```
  python tools/switch_prefix.py --game gts --write-map switch-map.json .   # in shadGT
  python <shadGT>/tools/switch_prefix.py --apply-map switch-map.json <RyGTS>   # the launcher and runtime
  ```

  It renames exact switch names only (not macros such as `RYGT_SWITCH_EXPORT`), in code, comments
  and docs. `CHANGES.MD` and `CHANGELOG.md` keep the names of their time.

## Diagnostics stay out of Release builds (PERF-SGT-025)

Diagnostic code (timers, counters, reports, traces, dumps, extra log lines, the hashes and
containers that feed them) is built only in diagnostics builds, never in Release. Release is what
players run, and a diagnostic left in it costs every draw,
packet or job it sits on even when nobody reads it. PERF-SGT-025 removed about fifteen such costs
from the hot paths: clock reads per dispatch, a lock per draw in Pro mode, a processor-number call
per recorder job, a log line on every capability query.

Two gates exist, and they differ in RelWithDebInfo:

| Gate | Debug | RelWithDebInfo | Release |
|---|---|---|---|
| `SHADGT_DIAGNOSTICS` (CMake's `SHADGT_DIAGNOSTICS=AUTO`) | on | on | off |
| no `NDEBUG` | on | off with CMake's default flags (`/DNDEBUG`); on when the flags leave it out (`-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="/Zi /O2 /Ob1"`, the measurement build) | off |

- **When you add a diagnostic** to find a bug or build a feature, gate it as you write it, with the
  same check its reader uses:
  - `#ifndef NDEBUG` / `#if !defined(NDEBUG)` around the code, or `NATIVE_DIAGNOSTIC(...)` in the
    native renderer (a no-op under `NDEBUG`);
  - `#ifdef SHADGT_DIAGNOSTICS`, or `Common::DiagEnv("SHADGT_DIAG_...")` for its switch, which is
    `nullptr` at compile time in Release; `if constexpr (Common::DiagnosticsBuilt)` in code.
  - A helper that times or counts (a scope object, a `Note...` function) compiles to nothing in
    Release, as `Native::OrderedTiming` and `PartClock` do; don't rely on a run-time switch alone.
- **The reader and the writer are gated together.** A counter, ring or timestamp whose only reader
  is a debug report is dead work in Release; gate the writes, not just the report.
- **A rate-limited or switch-guarded log still costs** its check, and anything built before the
  `LOG_*` call (strings, hashes, locks) runs at every log level. Build it inside the gate.
- **Functional work stays.** If a "diagnostic" also does something the game needs (a stalled
  wait's fence flush, a sampler upkeep), keep that part in every build and make it cheap.
- **When the work is done**, check what is left: build Release and look for the diagnostic's log
  text in `libSceVideoOut.prx` (`grep -ac "<text>"` should find nothing), and say in the commit
  that it is diagnostics-only.
- Mark the switch's description line (below) "diagnostics builds only", under
  `Diagnostics & tracing`.

## Every switch needs a description line

The RyGT launcher's Advanced page lists the renderer's experimental switches by reading
`libSceVideoOut.prx` as a file: every `"SHADGT_..."`, `"SHARED_..."` or `"RYGT_..."` string the renderer passes to `getenv`
(or `Common::DiagEnv`) becomes a row. The hover tooltip, editor and heading come from metadata
strings in the same DLL, kept in **`src/video_core/rygt_switches.cpp`**.

**When you add a switch** (a new `getenv("SHADGT_...")` or `getenv("SHARED_...")`, in `src/` or `ryty_gpu/`), add one line to
`RyGT_SwitchDescriptions` in that file, under the matching group comment:

```cpp
"RYGTSW1|SHADGT_EXAMPLE_SWITCH|flag|Rendering workarounds|off|1 = on, 0 = off|One sentence on what it does.",
```

Fields, separated by `|` (only the description may contain `|`):

| Field | What to write |
|---|---|
| `RYGTSW1` | Format tag; always this. |
| name | The exact variable name. |
| kind | `flag` (0/1 or presence), `int`, `hex`, `float`, `string` or `choice`. Picks the launcher's editor and validation. Use `choice` (FEAT-GTS-416) when the switch takes only a fixed set of values: the launcher shows a dropdown instead of a text box, where anything else would be ignored. |
| group | One of: `Shader objects & caches`, `Shader compiling`, `Native recorder & sync`, `Memory & VRAM`, `Tessellation`, `Frame pacing & present`, `Rendering workarounds`, `AMD`, `NVIDIA`, `Runtime`, `Diagnostics & tracing` (hidden until the user asks; use it for logs, traces, dumps and stats). |
| default | What happens when the variable is not set (`off`, `on`, `300`, ...). |
| values | What the accepted values mean; say "Diagnostics builds only", "Debug builds only" or "any value turns it on" when true. For a `choice`, only the values, separated by `; `: `value` or `value = meaning`, and `A..B` for every whole number from A to B (e.g. `0 = off; 1 = log; 2 = per frame`, `1..8`); put any other note in the description. |
| description | One plain sentence, about 150 characters at most, for a user deciding whether to set it. Say if it slows the game or writes large logs. |

**Switches the RyGTS runtime reads** (`core/` in RyGTS: libkernel, libc, libSceAvPlayer,
libSceUserService, ...) are described in this table too: the launcher reads every `libs\*.prx`, the
runtime modules included, and takes a description from whichever file has it. Add their lines under
`Runtime` (or the group that fits) and say which module reads them.

**When you change or remove a switch**, update or delete its line.

- A switch without a line still shows in the launcher, under "Not described in this renderer build".
- A line whose name the build never reads is not shown.
- Keep the table `extern "C"` and exported (`RYGT_SWITCH_EXPORT`): the renderer never reads it, so
  the export is what stops the linker from dropping the strings.
- Keep the switch name a plain string literal at the `getenv` call. The launcher finds names that way,
  so a name built at run time (concatenated or formatted) will not be found.

The launcher side is `loader/AdvancedPage.cs` (`SwitchWalker`) and `loader/ADVANCED-PAGE.md` in RyGTS
and in ryGT7.
