Resource reuse tests are part of this directory's CMake/CTest project.

Run Release tests with assertions enabled. The new targets cover completed-worker
handoff and shutdown, GPU-completion-gated upload chunk reuse, partial upload tails,
independent pipeline bind points, a 1 GiB pipeline headroom boundary, stable range
compaction, bounded shared-library creation, fragment descriptor policy, immutable
upload equality/collisions and complete descriptor keys. Windows additionally tests
guarded upload memory. The draw-preparation test sets both obsolete environment
masks to zero and verifies production fixes remain enabled.
