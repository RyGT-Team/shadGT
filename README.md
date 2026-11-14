<!-- SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# shadGT7

# About

The GT7 branch adds native rendering and compatibility fixes, including wave64 translation
that prevents overlapping crowd instance writes, and sparse texture uploads that preserve
resident streamed mip levels. These fixes were verified at Deep Forest. A fix for flickering night shadows at Yas Marina
(cached render targets reused after a memory remap) is included but not yet confirmed in game. Other tracks, updates,
and hardware may still have rendering errors or crashes.

## GT7 ultrawide, HUD editor, frame generation and fast flips

These come from dessierror's [wheel + ultrawide edition](https://gitlab.com/dessierror/rygt-7-online-build)
(its `renderer` branch, cherry-picked). Each is switchable; the switches are environment variables,
which the ryGT7 launcher sets from its options.

- Ultrawide (GT7-UW): with `GPU.aspect_ratio` wider than 16:9 in the game's custom config (or
  `SHADGT_ASPECT`), the scene is stretched over the window and the HUD is kept at 16:9 in the centre.
  A frame's draws into a display buffer after its first (the scene) go to two HUD pictures, one over
  black and one over white, and the post-processing pass lays them over the scene with the coverage
  the game blended them with. `SHADGT_HUD_SPLIT=0` turns the split off; `SHADGT_HUD_STREAM=0` records
  the draws over white in the main list, switching attachments per draw, instead of in a list of
  their own.
- HUD editor: F7 in a race (ultrawide only) shows the HUD pieces as boxes to drag and resize; Save
  writes `user\custom_configs\hud_layout.ini`. Placed pieces apply to race HUDs only (told apart by
  their multisampled track and course maps), not to menus.
- Prepared draws for GT7 (GT7-PREP): hooks on GT7's (CUSA24769 1.00) PGL job begin, end and fork,
  installed only with `SHADGT_NATIVE_PREPARE=1` or `SHADGT_NATIVE_SHADOW=1` and only when the
  executable's bytes match. Ended jobs are walked on `SHADGT_NATIVE_PREPARE_WORKERS` renderer threads
  (default 4; 0 walks on the job's thread, as before).
- DLSS multi-frame generation (GT-MFG): `SHADGT_DLSSG_FRAMES=1..5` generated frames a game frame
  (default 1, as before; more than 1 needs an RTX 50 and falls back to 1 when refused).
- Fast flips (GT-FF): enabled by default with the native renderer when FSR/DLSS frame generation
  is off. Frames can be presented between vblank ticks, after their GPU work finishes and subject
  to the existing flip-rate pacing. The game's vblank clock is unchanged. This reduces uneven
  presentation below the refresh rate; it does not increase rendering throughput.
  `SHADGT_FAST_FLIPS=0` restores presentation on vblank ticks. The explicit `=1` override is still
  available; other rendering modes and frame generation keep their previous default.

## Build

Clone with submodules. In a Visual Studio x64 developer environment with clang-cl, CMake,
Ninja, Python and the Windows SDK:

```
cmake --preset x64-Clang-Release -DSHADGT_RYTY_GPU=ON -DHAVE_ENDIAN_H=OFF
cmake --build Build/x64-Clang-Release --target ryty_gpu --parallel 8
```

This builds libSceVideoOut.prx. The companion runtime and packaging instructions are in
[ryGT7 build-support](https://gitlab.com/rygt-team/rygt7/-/tree/main/build-support).
The standalone shadGT emulator and historical GT Sport documentation remain inherited source;
use the ryGT7 launcher to prepare and run GT7.

The wave64 guard regression test contains 512 synthetic checks and no dumped game shaders.
With tests enabled, its CMake target is shadgt_wave64_guard_regression.

## License and credits

Install the prerequisites in the [Windows build instructions](documents/building-windows.md):
Visual Studio C++ Build Tools, LLVM (`clang-cl`), CMake and Ninja. Use an x64 developer
shell with LLVM on `PATH`, then run from the repository root:

```powershell
git submodule update --init --recursive
cmake --preset x64-Clang-Release
cmake --build Build/x64-Clang-Release --target shadps4 --parallel 6
```

The resulting executable is `Build\x64-Clang-Release\shadGT.exe`.
The Windows preset enables LibreSSL's Windows endian compatibility path, including
the workaround needed by Clang 23. The internal CMake target is still named `shadps4`;
the application it produces is `shadGT.exe`.

To create a portable package, use `scripts/Make-GTSportPortable.ps1`. It also requires
an unpacked Qt launcher and an existing profile configuration; those local files
are not included in Git. See [Build tools setup](Build/TOOLS_SETUP.md) for the
development and capture tools.

`scripts/Run-GTSportPerformance.ps1` uses an isolated test profile and prints a
performance summary after exit. Supply your own `-GamePath`; the script's default
path is machine-specific. It currently also expects the diagnostic Vulkan SDK
directory to exist. `-DisablePerf <ids>` disables selected optimizations for comparisons.

The recorded GT Sport baseline uses Windows red-zone patching, `readbacks_mode` 1,
linear-image readback and pipeline caching. Fresh emulator profiles do not enable
all of these automatically; preserve the tested settings when packaging a build.

# Keyboard shortcuts

> [!NOTE]
> Some keyboards require holding **Fn** to use the function keys.

| Shortcut | Function |
|-------------|-------------|
F10 | FPS Counter
Ctrl+F10 | Video Debug Info
F11 | Fullscreen
F12 | Trigger RenderDoc Capture (or game-only screenshot if RenderDoc is unavailable)
Alt+F12 | Capture screenshot including HUD/dialog overlays


# Firmware files

Place firmware modules dumped from your PS4 in `user\sys_modules` beside the emulator.
GT Sport uses modules such as `libSceNgs2.sprx` for audio, `libSceJpegDec.sprx` for
image decoding, and `libSceJson2.sprx` and `libSceFont.sprx` for other game services.
These examples are not a complete firmware inventory. Missing modules can cause
missing audio, unavailable features or startup failures.

> [!IMPORTANT]
> The firmware modules and the game must be dumped from your own PlayStation 4 console.

# Credits

shadGT is built on [**shadPS4**](https://github.com/shadps4-emu/shadPS4) and would not exist
without the shadPS4 project and
[**all of its contributors**](https://github.com/shadps4-emu/shadPS4/graphs/contributors). The
emulator core, its libraries and almost all of the code here are their work; shadGT's changes are
listed in [GT_SPORT_OPTIMIZATION_LOG.md](GT_SPORT_OPTIMIZATION_LOG.md). The Qt launcher is the
[shadPS4 Qt launcher](https://github.com/shadps4-emu/shadps4-qtlauncher). The GT Sport 1.69 boot
patch is by Kravickas, from the shadPS4 game patch repository.

shadGT is not affiliated with the shadPS4 project, Sony Interactive Entertainment or Polyphony
Digital. Gran Turismo is a trademark of Sony Interactive Entertainment.

For shadGT-specific problems, report them in this repository's
[issue tracker](https://github.com/mlgprorektm8/shadPS4/issues), rather than the upstream
shadPS4 project. Include your build commit, game version, hardware, settings and a
description of how to reproduce the problem. Logs can contain local paths or account
information, so check them before sharing.

# License

Licensed under [GPL-2.0-or-later](LICENSE), like shadPS4.

