# Normal-play geometry queue trace

DIAG-334 adds an opt-in Debug-only trace. `NDEBUG` removes its state, environment
lookup and call sites from the renderer. It does not activate F8 or bypass normal
prepared draws, and it does not change GDS settings or add GPU synchronization.

Set `RYGT_GEOMETRY_TRACE=1` before starting a Debug build. Create
`geometry-trace.arm` in the game's working directory immediately before entering
the affected track. A correlated graphics pipeline wait of at least 50 ms seals
the recent history after 1,024 subsequent events. Alternatively, create
`geometry-trace.flush` while the defect is visible. The next decode poll writes
`geometry-trace.csv`. Preserve existing control/output files before each run.

The ring holds the most recent 16,384 events and reports overwritten events in a
`dropped` row. Control files are polled every 1,024 decoded jobs. A scene with no
new jobs cannot notice a request or flush automatically. The first slow wait may
be unrelated to grass. A single trace does not cover the whole loading screen.

CSV fields are `kind,ns,id,a,b,c,d`. Times use the process's steady clock and IDs
correlate a decoded draw/dispatch with its queued recorder call. Important rows:

| Kind | a | b | c | d |
| --- | --- | --- | --- | --- |
| `decode` | compute | direct | index count | instance count |
| `decode_index` | index address | maximum index count | index type | reserved |
| `queue` | decode epoch | selected/staged | prepared | read capture present |
| `queue_flags` | neutral job | compute | direct | staged |
| `selected` | graphics pipeline pointer | compute pipeline pointer | indirect selection | staged |
| `stage_cpu_flat`, `stage_installed`, `stage_rejected` | stage | shader hash | full word count | CPU prefix hash |
| `stage_mode` | stage | validate | trust-selection policy | pipeline pointer |
| `wait_begin`, `wait_end` | pipeline hash | reserved | reserved | reserved |
| `vertex_guest` | vertex shader hash | guest base | descriptor size | binding index in high 32 bits, stride in low 32 bits |
| `vertex_resolved` | binding index | Vulkan buffer handle | buffer offset | buffer generation |
| `vertex_extent` | binding index | resolved size | native frame | scheduler tick |
| `dispatch_pipeline` | compute shader hash | pipeline pointer | indirect (0 for direct) | reserved |
| `compute_no_pipeline` | 1: absent selection; 2: failed recorder lookup | reserved | reserved | reserved |

`recorder_begin`/`recorder_end` and `direct_begin`/`direct_end` delimit execution.
`draw_no_pipeline` identifies a graphics draw that cannot be recorded. A missing
end row can also mean the bounded trace sealed or overwrote it.

CPU hashes cover at most 256 words already owned by the selected/installed shader
info. Equal prefixes do not prove full equality; changed prefixes do not prove
geometry corruption. No guest geometry bytes or GPU-visible buffer contents are
sampled. Vertex binding metadata is emitted only when the native resolver runs,
so cached/prepared paths may lack those rows. This trace cannot alone establish
that a particular draw renders grass or that a producer's GPU writes were visible.

CSV output is synchronous after the trace seals. Recording uses a diagnostic
mutex and CPU hashes. Exclude trace/flush intervals from performance comparisons.
Failed output opens/writes/closes retain sealed evidence for a later retry.
