# Will Scarlett source port candidate

## FIX-331: Keep Going menu capture scope

- Keep Going no longer makes every ordinary menu draw a required compilation.
  The verified GTS icon-capture Begin starts a required interval before image
  and mask work is scheduled. It stays required through the actual asynchronous
  save worker and capture End. Missing hooks, interrupted saves, unknown owners
  and mismatched generations retain the conservative wait policy.
- A retained engine shared owner protects the capture across the save wrapper,
  including an early End. Only a matched completed capture releases that pin.
  Unsupported/interrupted cases retain a bounded pin and wait policy until exit.
  Known one-shot, shader memory-write and depth-bake protections remain in place.
- The skipped-output observer is now explicitly opt-in with
  `RYGT_ONE_OFF_LEARN=1`: sampling a temporary skipped intermediate does not prove
  that it is a persistent one-shot image, and address reuse must not silently
  convert normal Keep Going draws into blocking work.
- `RYGT_CAPTURE_SCOPED_MENUS=0` restores the previous blanket menu wait for a
  control run. This is an isolated candidate; cold thumbnail output, cancellation,
  animated menu performance and race/UI transitions require runtime validation.

Target base: `ryty-gpu` at `20b1965a00de9f1b5fdca9b3f861c7a7f6faadee`.
Upstream snapshot: `rygt-team/shadgt7` main at
`3b27dbfee4cd63eaf173826c5447c8cd715a183b`.

The full inventory covers all 186 commits authored by
WillScarlettOhara (`leilumultipass@gmail.com`) reachable from that snapshot,
including 77 commit messages mentioning PERF. Dependencies and later fixes are
reviewed together. This is a draft port, not a claim of runtime validation or a
completed port of every inventory entry.

## Native resource group

| Source | Ported behavior |
| --- | --- |
| `32288f94`, `4bbac02c` | Optional in-place page statistics; address-indexed pending region writes. Existing imported-page coverage supersedes the small range-import memo. |
| `a6dac7da`, `5ad550f4` | Keep recently used, CPU-loaded idle textures within the 2 GiB LRU budget; exclude GPU/foreign writes, target-backed textures and crowded addresses. |
| `25e4c8b4` | Import allocation-relative 256 MiB host-memory pieces on use, up to two pieces for a crossing request. Preserve target coverage, negative-cache generation and deferred cleanup. |
| `c555339f`, `615f732e` | Revalidate disjoint buffer/texture memo ranges through change logs; unmaps and hard invalidation still reject them before dereferencing old backing. |
| `1fce9209`, `a5b40722`, `23282819`, `0f86e14f` | Write small pictures back once per frame; flush overlapping pending pictures before later reads/writes. Preserve immediate GTS photo/thumbnail writebacks above the upstream 64 KiB limit. |
| `f97bc35f` | Batch image downloads, tile in device-local scratch and copy to guest memory. Keep target's authoritative image-to-region copy ordering and reusable standalone scratch. |
| `ed6d5c06`, `42003b21` | Coalesce proven independent compute runs; same-texel mip/layer keys retain unknown-write, descriptor-alias and prior-read safeguards. A functional recording epoch replaces diagnostic counters for Release dependency checks. |

Will's switches/defaults are retained: `RYGT_D1_VIEW_MB=256`,
`RYGT_NATIVE_KEEP_TEXTURES_MB=2048`, buffer/texture revalidation enabled,
`RYGT_NATIVE_WRITEBACK_EACH` unset, writeback batching enabled and same-texel
optimization enabled. Program-specific dispatch shortcuts require the exact
upstream program identity and proofs; other dispatches keep their barriers.

`RYGT_SPLIT_GDS_WRITE_FAULTS` now defaults ON following the requested GTS
runtime check. Explicit `0` restores the full-drain control. The user reported
correct grass placement with the enabled run; no matched OFF recovery comparison
has established the individual cause. Separate PERF-014 drain/readback safeguards
and the general write-fault splitting default are unchanged.

## Validation so far

- Real clang-cl front-end checks of the isolated native renderer, image registry
  and direct-memory imports, using the worktree's headers before dependency paths.
- All nine upstream same-texel GoogleTest cases pass.
- Debug and Release checkpoint `958d7f1e` built and passed matching-symbol,
  paired-runtime loading and all 2,766 import checks. Its Debug RHI, image and
  descriptor GPU regressions passed, including nonzero offsets, guards,
  linear/tiled layouts and mip roundtrips.
- Debug checkpoint `2981d2e4` additionally includes shader objects, imported
  backing lifetime protection, visible-build priority, ready-frame pacing and
  speculative state isolation. The same ABI/symbol and three GPU suites passed.
- Newer groups below require a fresh full assembly build and linked GPU checks.
  No installed game, working runtime or production shader cache has changed.

## Additional integrated groups

- `PERF-245`, `PERF-251`: serialized recorder-idle ownership and queue/decode
  yielding. Register-delta tests retain the target's coalesced-run contract.
- `PERF-247`, `FIX-267`: tessellation LDS writer/reader emulation and deferred
  writer barriers, with partial sample masks excluded from the optimization.
- `FIX-248`, `FIX-252`, `FIX-253`, `FIX-254`: FMask specialization, wave64 lowering,
  floating-point atomic classification, pinned vertex information, asymmetric
  empty-buffer matching and AMD translation behavior.
- `FIX-255`, `FIX-263`, `FIX-264`, `FIX-265`, `FIX-270`: tiny and sixteen-sample
  macrotiles, stable buffer rebinding, and region replacement. Reject failed
  replacements without discarding authoritative GPU bytes or claiming a clear
  that was never written.
- `PERF-250`, `PERF-256`: generation-qualified target and sampler reuse,
  canonical Vulkan samplers, and exact-value descriptor/push-constant command
  reuse. Worker snapshots still receive their full bindings.
- `PERF-259`: guarded compiler CPU placement, source worker priorities and
  recorder-core ownership. External CPU selections are respected; unsupported
  ownership combinations leave placement alone. The heavy driver-build limit
  remains in force.
- `FIX-268`, `FIX-271`, `FIX-277`: lifetime-guarded close-time cache saving and
  non-destructive compatible shader-store selection. Old stores, archives,
  pack stamps and incompatible retranslation marks are retained. An unreadable
  archive falls back to a writable directory store.
- `PERF-269`, `PERF-260`: bounded metadata readers, optional SPIR-V prefetch,
  and compute recipes warmed under the shared heavy-build gate. Recipes use
  explicit translation-policy compatibility, not executable, driver or SCM
  identity. Existing incompatible files are skipped and preserved.
- `FIX-272`, `PERF-262`, `FIX-275`, `PERF-274`: own queued callable values,
  record native command buffers on the scheduler ring, keep replay state on its
  owning thread, and start a list during serialized recorder idle. The ring is
  excluded when recorder workers, Intel direct recording, raw checkpoints or
  unsupported timing paths require direct command handles. Explicit
  `RYGT_NATIVE_RING=0` remains available in Release; periodic A/B diagnostics
  are Debug-only.
- `PERF-273`: early submission when the preceding tick is complete, bounded
  read-only storage mirrors. `FIX-290` follows upstream `83502d8b`, removing
  the display-awake workaround that upstream reverted after identifying the
  Bluetooth HID removal stall.
- `FIX-276`: reclaim GPU-completed upload allocations before an allocation
  retry; never wait for or free the current unsubmitted tick.
- `PERF-261`, `PERF-281`: packed specialization lookup, Screen/Soon build
  priority, bounded catch-up and required-build promotion. Speculative jobs
  remain bounded and cannot displace required work.
- `PERF-278`, `FIX-285`: shader objects with instance-owned stage sharing,
  compatible binary storage, aligned cached binaries, and conventional pipeline
  fallbacks for blended HUD and LDS-emulated draws. The dependency chain includes
  MaxiRazorback's SO-001/SO-002 work (`2ddc0f9c`, `3ef6fe04`) and HUD correction
  (`fd8a1740`), plus Will's later cache/layout fixes. Credit belongs to both
  upstream contributors. GPL and shader objects are distinct Vulkan features.
- `FIX-279`: lease imported buffers, aliases and memory through their actual GPU
  submission lifetime, including removal and failed retirement scheduling.
- `FIX-282`, `FIX-287`, `FIX-288`, `FIX-293`, `FIX-294`, `PERF-295`: semantic
  lane-policy compatibility, recoverable speculative failures, isolated
  read-ahead match state, safe stale-job cancellation, and the optional
  3 ms per 16 ms read-ahead evaluation budget. Required jobs retain priority.
- `PERF-284`, `FIX-286`: ready-frame default/rate pacing and joining the driver
  cache writer before every early return that requires completion.
- `FIX-258`: precise changed-word region writeback, operation-wide preflight,
  imported-part leases, exact image overlap preservation, and a bounded snapshot
  pool. Large or unsupported writebacks invalidate recency instead of dropping
  the guest operation. Fault-ahead remains opt-in (`RYGT_NATIVE_FAULT_AHEAD=1`),
  with image exclusions and guarded snapshots.
- `FIX-289`, `FIX-302`: mip-crop layout checks, superseded-target retirement,
  fill-hash invalidation, packed-order alias isolation, whole-cube view checks,
  source-format resolves and an intermediate fallback. Unsupported resolves stop
  before issuing an invalid Vulkan operation. The broader packed-float conversion
  dependency is not represented by its metadata-only adaptation.
- `FIX-291`, `FIX-292`: overlay gamepad input from event-thread atomics with
  post-reassignment selection and balanced handles; one keyboard-open event
  consistent with the absence of USB keyboard resource IDs.

These groups include source defaults where their lifetime and API contracts can
be retained. GT7-only addresses, game patch offsets and branding require a GTS
adaptation; they are not evidence of equivalent behavior in GT Sport.

## Closing implementation groups

- PERF-297 batches eligible native fences while preserving forced flushes, polling
  progress, readbacks, and the non-pipelined VideoOut path.
- FIX-296 composes cube shader-store policy with existing fragment revisions and
  preserves foreign and sparse occupied indices. FIX-303 admits readable prefixes
  only for qualified read-only bindings; changed worker bindings revalidate serially.
- PERF-298 skips redundant compatibility scans only after a fresh-store proof.
  FIX-306 detects unsampled rewrites of large layered CPU textures.
- FIX-307 remembers watched directly imported fills through actual GPU completion,
  rejects stale CPU receipts, preserves later authoritative draws, and retires
  conflicting aliases. GPU-private fills keep the established path.
- FIX-308 adds packed-float clears/fills and linear/tiled upload/download conversion,
  bounded rounded dispatch, per-mip extents and padding preservation.
- PERF-304 and PERF-309 release the shared lookup lock during qualified required
  compiler or listing waits. Retained jobs/shared futures survive map replacement;
  bounded retries rebuild the complete lookup. Transient modes retain their path.
- PERF-310 avoids repeated placeholder work for pending optional pipelines.
  PERF-311 retries only owned, guarded direct-user-data VS/PS inputs with exact
  job identity; SRT walkers, fetch shaders, tessellation and geometry are excluded.
  Retention is bounded to 512 states and 8 MiB, with expiry and required-work priority.
- PERF-312 selects indirect pipelines on the command thread with exact draw-kind
  and SGPR identity. Indirect data is not falsely declared captured.
- FIX-313 supplies the paired runtime mapping protocol; FIX-299 invalidates stale
  CPU-origin mapping copies and guards delayed readbacks against address reuse.
  The capability freezes before use. Old runtimes keep their original behavior
  without token allocations. The companion patch is a reviewed artifact, not an
  automatically applied CI runtime update.
- PERF-314 uses configurable 1 MiB upload chunks and staging steps that fit them.
  Decoder/recorder priority is explicit opt-in; periodic comparison is Debug-only.
  FIX-318 keeps its standalone policy header compatible with Windows macros.
- FIX-315 preserves every existing GTS writeback and adds qualified high-address
  color outputs with import and GPU-budget preflight. Its ordinary extension is
  bounded to 256 MiB. FIX-322 streams larger byte-exact RGBA8 linear images across
  retained imports without whole-image scratch. FIX-324 permits re-admission after
  overlapping unsubmitted plans are canceled. Unrelated mappings do not cancel.
- FIX-316 clears proven complete prefixes of equal-stride image layers; ambiguous
  layouts or pattern phases use the established fallback.
- FIX-317 expands the push-offset table to 96 bytes without overlapping the shared
  mask/LDS slot at byte 112. The complete push block stays within Vulkan's 128-byte
  minimum. Old metadata stays discoverable, while incompatible SPIR-V and compute
  recipes are skipped without deleting their files. DEBUG-321 isolates its tests.
- FIX-319 uses legal read-only depth layouts or supported feedback-loop layouts,
  and a pre-draw snapshot fallback for unsupported writable feedback. Descriptor
  keys refresh after view/layout substitution; snapshot owners survive both copy
  and draw submission ticks, including intervening command-list splits.
- PERF-320 learns exact potentially harmful skipped-output identities and adds
  both translation and pipeline waits. It never relaxes existing UI/depth guards.
  Bounded persistence runs on a joined background worker.
- PERF-323 periodically trims completed, idle upload chunks, retaining the
  low-water reserve and existing byte budget. Active/tail and retiring chunks
  remain owned. The trim can be disabled explicitly.
- PERF-326 keeps unavailable mapping ranges merged and ordered so repeated
  readback captures do not add another linear allocation-registry scan.

## Deliberate adaptations and validation boundaries

All-author inventory and module ledgers distinguish ported behavior, pre-existing
coverage, source reverts, diagnostics and game-specific code. They do not equate
186 source commits with 186 literal cherry-picks. GT7 addresses, executable patch
offsets, the GT7 headlight fill hash and branding are not installed in GT Sport.
The display-awake workaround follows upstream's revert; the reverted unoptimized
compute policy stays reverted. Destructive worktree/hash-driven store clearing
conflicts with the user's cache-preservation instruction and is not restored.

The learned one-off policy is deliberately conservative: exact observations can
add waits, while weak kind/family matches cannot remove UI or depth safety waits.
First encounters and prepared/aliased observations are not fully covered.
The retry path likewise makes no claim for unowned shader or SRT inputs.

Mapping protection requires the companion kernel protocol. The isolated kernel
was checked against the current Fiber/libc package: all 664 original kernel
exports and all 2,766 package imports remain, and its literal protocol export
returns version 1. This is a historical-object rebuild and loader audit, not a
fresh complete runtime build or map/remap gameplay proof. Full runtime rebuilding
also encountered toolchain failures in unchanged libc; those libraries are not
replaced by this port.

Partial GPU-authoritative remaps retain inherited limitations. Remembered fills
require qualified direct imports. Large tiled/packed/other-format writebacks
outside the bounded path keep their prior behavior. Unsupported writable depth
feedback samples pre-draw contents, not writes occurring within the same draw.

Debug and Release checkpoint abbefb5b both built with matching PDBs and all 2,766
imports, and passed paired-runtime loading. Its linked NVIDIA RHI, ImageRegistry
and descriptor suites pass, including packed linear 1x1/17x3 padding, tiled/mip
roundtrips, large writeback fences, layered rewrites, and changed/untouched layers.
The actual emitted TCS also passes a separate NVIDIA writer/reader fixture with
the new push ABI. The depth fixture passes read-only, forced snapshot and supported
feedback draws. Later assembly validation is recorded beside the final artifacts.

Final acceptance requires both full assembly builds, relevant CPU tests, linked
GPU suites and Release diagnostic gating. Two-race menu/Brand Central checks,
native remap execution and the explicitly requested split-GDS ON trial require
gameplay validation. Component tests establish neither a runtime speedup nor
the 16.6 ms target. No port binary has been installed or merged.

## FIX-335: Wide tessellation LDS scratch

Subsequent local testing installed the port and exposed a repeated HS translation
failure for an LS stride of 72 bytes. The writer, reader and interface already
round that to five vec4s (80 bytes); the prologue's old 64-byte assertion was stale.
The correction retains LDS emulation and validates the generic interface bounds.
Own-patch input loads now occur only in their valid branch. Out-of-group vertex,
attribute and component reads return zero without evaluating an invalid pointer.

Native scratch allocation reserves the complete reachable tail for each mode,
including a partial final patch group, within the existing 32 MiB cap. Unwritten
tails are cleared before the existing writer-to-reader barrier. Deferred readers
clear their tails together, preserving batching and avoiding a new per-draw pass.
Only emulated TCS cache revisions change; unrelated shader binaries remain usable.

Five CPU layout tests, two actual SPIR-V emission tests and eight NVIDIA fixture
cases pass locally. The GPU cases exercise 64/72-byte strides, both modes,
partial-group padding, invalid indices, disabled scratch and own-patch input;
serial and batched runs check all output components, writer bytes, zeroed tails
and canaries. The full Debug/Release renderer assembly and linked component tests
are recorded with their final artifacts, not implied by these standalone checks.

The preceding cold-cache cockpit run lost the Vulkan device and Windows recorded
an NVIDIA driver reset. Its relationship to the translation failure or these
bounds defects is unproven pending a matched cockpit rerun. GDS stays ON. Working
runtime libraries, the UI/capture guards and Keep Going policy are unchanged.
