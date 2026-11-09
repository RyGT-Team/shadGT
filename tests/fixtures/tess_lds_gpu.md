# NVIDIA tessellation LDS fixture

Build `shadps4_gcn_test` and `shadgt_tess_lds_gpu` in a Debug configuration. Run
`shadps4_gcn_test --gtest_filter=TessLdsShader.*` with `RYGT_TEST_SPV_DIR` set to a
writable directory. This emits the TCS cases using the actual shader backend. Validate
each module with `spirv-val --target-env vulkan1.3`.

Compile `tess_lds_gpu.vert` and `tess_lds_gpu.tese` with
`glslangValidator -V --target-env vulkan1.3`. Run the standalone executable with the
vertex, emitted TCS and evaluation module paths, followed by stride, mode and probe.
It selects an NVIDIA Vulkan 1.3 device and uses its own instance, buffers, command pool
and queue. Run these cases:

| TCS module | stride | mode | probe |
| --- | ---: | ---: | ---: |
| `tess_lds_64_mode1.spv` | 64 | 1 | 0 |
| `tess_lds_72_mode1.spv` | 72 | 1 | 0 |
| `tess_lds_72_mode2.spv` | 72 | 2 | 0 |
| `tess_lds_invalid.spv` | 72 | 1 | 1 |
| `tess_lds_invalid_attr.spv` | 72 | 1 | 2 |
| `tess_lds_invalid_comp.spv` | 72 | 1 | 3 |
| `tess_lds_72_mode2.spv` | 72 | 2 | 4 |
| `tess_lds_own.spv` | 72 | 2 | 5 |

Probe 4 disables scratch only for the reader; probe 5 checks own-patch input. Each run checks
all components of all three patches, both serial and batched writer/reader ordering,
the writer's scratch contents, zeroed partial-group tail, and guard words. Scratch is
poisoned on the CPU; a GPU transfer clears the tail after the writer and before the
writer-to-reader barrier. Canaries detect out-of-bounds writes, not reads. The fixture
does not launch a game or validate every guest shader.
