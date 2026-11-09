# Experimental parallel recording

The native renderer can record eligible race passes on worker threads. The
experiment is off by default; set `RYGT_PARALLEL_RECORDING=1` before launch to
request it. Draws that cannot be safely captured continue on the ordered
recorder. Resource freshness, feedback, descriptor ownership, and GPU-tick
retirement are checked before work is handed off. The producer descriptor
allocation experiment remains separately opt-in.

This work preserves the project's x86-64-v3 minimum. Long xxHash inputs may use
an isolated AVX-512 implementation when both the CPU and operating system
support it; otherwise they use the existing xxHash path.

A guarded paused-scene comparison of the earlier 056 implementation measured
parallel / serial / parallel medians of approximately 33.0 / 29.0 / 33.0 ms
on one machine. The subsequent mixed-descriptor snapshot change in 057 has
not been measured in a guarded frame-time comparison. No 16.6 ms target or
performance improvement is claimed; measure on the intended system and scene.
The isolated GPU tests exercised real Vulkan rendering and modeled producer
handoff; they were not a complete game integration test.

## Validation

Build the existing `ryty_gpu` target with `SHADGT_RYTY_GPU=ON` and Discord RPC
as configured for the release. Standalone host tests are under `ryty_gpu/tests`:
configure that directory with CMake and run `ctest`, including
`parallel_descriptor_snapshot`, `parallel_hash_fallback`, and, on Windows,
`parallel_hash_dispatch`. The hash-dispatch test checks the real runtime path,
concurrent first use, boundaries, and a protected guard page. Snapshot tests
check mixed rows, no-mutation on size mismatch, null buffer handles, and empty
buffer groups. A real-GPU regression should additionally queue distinct buffer
snapshots before waiting, check independent expected pixels, exercise fallback
and a wrong-valid-buffer negative control, and cover indexed vertex/MRT output.
