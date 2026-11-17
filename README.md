# FidelityFX SDK — Linux / Vulkan port

A fork of the [AMD FidelityFX™ SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
(upstream **1.1.4**) that builds and runs on **Linux** against **Vulkan**, with no Wine
and no Windows toolchain.

Upstream ships a Vulkan backend, but the build is Windows-only in practice: the CMake
setup forces an MSVC/Visual Studio toolchain, the sources rely on MSVC-specific CRT
functions, and — the real blocker — every shader permutation is compiled by
`FidelityFX_SC.exe`, a Windows binary. This fork replaces that toolchain with a native
one and fixes the portability and correctness problems that surfaced along the way.

The focus is **FSR3 upscaling and frame interpolation**. See [Scope](#scope) for what is and isn't ported.

### Why 1.1.4, and not a 2.x release

Because **1.1.4 is the last upstream release that ships a Vulkan backend.** AMD removed
it in 2.0.0: 1.1.4 carries 270 Vulkan/GLSL source files, 2.0.0 and later carry three
incidental ones (a vendored VMA header, a GLSL callbacks header inside a DX12 sample).

FSR3 itself is still very much present in 2.x — the upscaler headers and
`ffx_fsr3upscaler.cpp` are all there — so the version gap is not about FSR3 being
dropped. But 2.x restructures the SDK around `Kits/FidelityFX/`, its `backend/`
directory contains only `dx12/`, and DX12 is distributed as signed prebuilt DLLs.
There is no Vulkan backend in 2.x to port, so retargeting it would mean writing one
from scratch against a restructured SDK, not rebasing this work.

The 1.1.4 base is therefore deliberate, not stale.

> This is an unofficial, community fork. It is not built, endorsed, or supported by AMD.
> For AMD's original SDK documentation, see [README-upstream.md](README-upstream.md).

---

## Scope

| Component | Status on Linux |
| --- | --- |
| FSR3 Upscaler (`FFX_FSR3UPSCALER`) | **Working** — the primary target of this port |
| Single Pass Downsampler (`FFX_SPD`) | **Working** |
| Vulkan backend (`ffx_backend_vk_x64`) | **Working** — builds as a shared library |
| Optical Flow (`FFX_OF`) / Frame Interpolation (`FFX_FI`) | **Working** — driven through the raw prepare/dispatch API |
| Frame Interpolation **Swapchain** | **Excluded** — depends on Win32 threading primitives |
| DirectX 12 backend | Windows-only, untouched by this fork |
| Cauldron framework & samples | Windows-only, untouched by this fork |

Everything not listed is upstream code that this fork does not attempt to build on Linux.

### Windows builds

This fork is not intended to change the Windows build. Every portability change is
behind a `_WIN32`, `UNIX` or `MSVC` guard, so a Windows configure resolves to upstream's
sources and flags — including the `FrameInterpolationSwapchain` sources, which are
excluded on Linux only. The FSR3 LUT-Lanczos workaround described below is likewise
scoped to non-Windows: it is a glslang toolchain workaround, not an algorithm change.

Two changes are shared with Windows deliberately, both of them fixes:

- `CMakeCompileShaders.txt` now returns the collected permutation headers. Upstream's
  `set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)` propagated the
  variable *name* rather than the headers appended to it.
- `<bit>` is included for `std::popcount`, which the standard requires.

**The Windows build is not verified here.** This fork is developed on Linux, and the
development machine has no MSVC or Windows SDK. What *is* verified is that AMD's
`FidelityFX_SC.exe` runs correctly under Wine and that its output agrees with
`ffx_sc_native.py`: for the same shader and arguments, both produce permutation tables
and reflection data that are identical (same CBV/SRV/UAV counts, same table shape,
read-only storage buffers as SRVs). The SPIR-V blob itself differs, because the bundled
`glslangValidator.exe` is 11.12.0 while the system glslang used by the native path is
16.6.0.

---

## Building

### Prerequisites

A C++20 compiler, CMake, Python 3, the Vulkan SDK/headers, and `glslangValidator`
(from `glslang-tools` / `vulkan-devel`, depending on the distribution).

The versions this fork is verified against:

| Tool | Verified version |
| --- | --- |
| GCC | 16.2.1 |
| CMake | 4.4.4 |
| Python | 3.14.7 |
| glslang | 16.6.0 |
| Vulkan headers | 1.4.363 |

Nothing here is pinned to those versions; they are simply what the port was built and
tested with.

### Configure and build

```sh
cmake -S sdk -B sdk/build_linux \
    -DCMAKE_BUILD_TYPE=Release \
    -DFFX_API_BACKEND=VK_X64 \
    -DFFX_FSR3UPSCALER=ON \
    -DFFX_SPD=ON \
    -DFFX_FI=ON \
    -DFFX_OF=ON \
    -DFFX_AUTO_COMPILE_SHADERS=ON

cmake --build sdk/build_linux -j"$(nproc)"
```

The first build compiles every shader permutation through `ffx_sc_native.py` and takes
a couple of minutes; the configuration above yields 300 deduplicated permutation
headers. Subsequent builds are incremental.

### FidelityFX API upscaler loader

Applications which use the five-entry-point FidelityFX API can build a native Linux
Vulkan loader directly from source. This configuration deliberately includes only the
FSR3 upscaler provider; frame interpolation and its Win32 swapchain provider are not
exposed by this library.

```sh
cmake -S ffx-api -B ffx-api/build_linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFFX_API_BACKEND=VK_X64 \
    -DFFX_AUTO_COMPILE_SHADERS=ON \
    -DFFX_API_BUILD_VK_SMOKE=ON

cmake --build ffx-api/build_linux \
    --target amd_fidelityfx_vk ffx_api_vk_smoke \
    -j"$(nproc)"

ffx-api/bin/ffx_api_vk_smoke \
    ffx-api/build_linux/libamd_fidelityfx_vk.so
```

The smoke test creates a 64x64 native-AA context, records reset and retained-history
FSR3 frames, then records a third retained-history frame with RCAS at 0.3 and reads that
output back. It fails on an unwritten output, a non-finite value, or a mean RGBA channel
more than 0.02 away from the constant linear input `(0.25, 0.5, 0.75, 1.0)`. This catches
zero/white output, accidental colour-domain conversion, and constant-colour sharpening
errors as well as loader or dispatch failure. The `VK_X64` spelling is the SDK's
existing backend selector; on Linux the binaries use the configure host's actual
architecture. Keep the build tree together: the loader's origin-relative runpath
resolves its Vulkan backend under that tree.

### Output

| Artifact | Path |
| --- | --- |
| Vulkan backend (shared) | `sdk/build_linux/src/backends/vk/libffx_backend_vk_x64.so` |
| Component static libs | `sdk/bin/ffx_sdk/*.a` |
| FidelityFX API loader (optional build above) | `ffx-api/build_linux/libamd_fidelityfx_vk.so` |

The backend is built as a **shared library** on Linux, and that is deliberate — see
[Linking against volk](#linking-against-volk) below.

A prebuilt `sdk/bin/ffx_sdk/libffx_backend_vk_x64.so` is committed for convenience.
It is **not** refreshed by a build; if you change backend sources, copy the newly built
`.so` over it yourself:

```sh
cp sdk/build_linux/src/backends/vk/libffx_backend_vk_x64.so sdk/bin/ffx_sdk/
```

---

## What changed vs. upstream 1.1.4

### Native shader compiler

Upstream compiles every shader permutation with `FidelityFX_SC.exe`. This fork adds
**`sdk/tools/binary_store/ffx_sc_native.py`** (~1000 lines), a native Python 3
replacement that produces byte-compatible permutation headers:

- expands the permutation option space and compiles through `glslangValidator`,
  in parallel;
- performs SPIR-V reflection by parsing the binary directly — descriptor bindings,
  sets, counts, storage classes and `NonWritable` block members — rather than
  shelling out to a reflection tool;
- deduplicates identical blobs by MD5 after compilation.

`sdk/CMakeLists.txt` selects it automatically on `UNIX`. A Wine wrapper
(`FidelityFX_SC_linux.sh`) is also present as a fallback path.

### Build system

- `sdk/toolchain.cmake` no longer forces `CMAKE_SYSTEM_NAME WINDOWS` and the MSVC
  generator platform on Unix; it enables position-independent code instead.
- `sdk/CMakeLists.txt` guards the MSVC-only `/Z7 /Od` debug flags behind `if(MSVC)`
  and resolves a platform name for `UNIX + VK_X64`.
- `sdk/src/backends/vk/CMakeLists.txt` excludes `FrameInterpolationSwapchain/` on
  Linux, and compiles the component API sources into the backend library (upstream's
  Windows build emits them as separate static libs).

### Portability

- **`sdk/src/backends/vk/ffx_posix_compat.h`** (new) — a POSIX shim for the MSVC CRT
  functions the SDK relies on: `wcscpy_s`, `sprintf_s`, `_countof`, and friends.
  It is force-included for all Linux targets via `-include`.
- `FFX_API_ENTRY` is guarded for non-Windows; integer literals given a `ULL` suffix.
- `<bit>` is included for `std::popcount` (GCC is strict about this under C++20).
- Case-correct include paths: upstream's `<FidelityFx/...>` (lowercase `x`) resolves
  fine on case-insensitive Windows filesystems but not on Linux.

### Vulkan runtime fixes

- `EffectContext` alignment (`alignas(32)` in the scratch buffer), a dummy image for
  null resource bindings, exclusion of AMD device-coherent memory, heap allocation of
  `VkDeviceContext`, and context size doubling for `wchar_t` width differences.

### Linking against volk

The backend is built as a shared library and linked with `-Wl,-Bsymbolic`. Both are
required when the host application uses [volk](https://github.com/zeux/volk): volk
defines Vulkan entry points as *function-pointer variables*, so ELF symbol
interposition would otherwise bind the backend's Vulkan calls to those data symbols and
call them as code addresses — an immediate segfault. `-Bsymbolic` binds the backend's
Vulkan references to `libvulkan.so` within the library itself.

---

## Two correctness fixes worth reading

These are the substantive findings, not just portability work.

### Most shader permutations were never compiled

`ffx_sc_native.py` originally carried an "early duplicate detection" pass that
text-scanned shader sources for permutation define names, and mapped every value of any
option it did *not* find to the value-0 blob **without compiling it**. The scan did not
traverse the FFX include chain, so most passes collapsed most or all of their
permutation dimensions.

The visible consequences: `prepare_inputs` shipped only an `INVERTED_DEPTH=0`
(standard-Z) variant. On a reverse-Z engine its nearest-depth dilation therefore
selected the *farthest*-depth motion vectors, corrupting reprojection near depth edges —
a quadrant-cross artifact under forward/backward camera motion. `prepare_reactivity`,
`luma_instability` and `shading_change` likewise ran non-HDR standard-Z variants
regardless of runtime flags. Sorting options by scan result also made permutation key
bit order depend on the broken scan.

The fix is to treat every option as referenced and actually compile every permutation.
The post-compile MD5 dedup — which is sound — collapses the genuinely identical blobs.

### LUT Lanczos permutations miscompile under glslang

FSR3 output was persistently soft when static and sharpened under motion. Root cause:
the `USE_LANCZOS_TYPE=LUT` permutations, which FSR3 selects on RDNA wave 32/64, are
miscompiled by the Linux glslang toolchain. The LUT-driven history/upsample kernel
degenerates into a strong low-pass, so every accumulate iteration re-blurs history,
converging to an MTF floor of ~0.36 at Nyquist (~60% edge-energy loss). History
rejection under motion masked it — hence "sharper in motion".

Isolated by bisection: disabling history entirely was sharp, so the blur entered via
history resample; forcing the reference analytic-Lanczos permutations (`useLut=false`)
restored converged static sharpness to 5.84 against a raw reference of 5.80, and dropped
frame-to-frame instability from 32% to 7.9%. Cost rose 0.343 → 0.375 ms at 1080p
Native-AA.

**This is a workaround, not a root cause fix.** `useLut` is forced false on this
toolchain; the underlying glslang codegen bug in the FFX GLSL LUT sampling path is
unresolved and left as a follow-up. Permutation selection, blob semantics, constant
layout, resource sizes and jitter constants were all verified sound in the process.

### Frame interpolation under the Vulkan validation layers

Three defects, each reported by the validation layers on the first frame-interpolation
dispatch:

- **`r_counters` was never bound.** `ffx_sc_native.py` dropped `OpDecorate`
  instructions without a literal operand, `NonWritable` among them, and never read
  `OpMemberDecorate`, so it filed every read-only storage buffer as a UAV. The
  read-only `buffer r_counters` then missed the frame-interpolation UAV name table,
  which holds only `rw_counters`. `patchResourceBindings` returns at the first
  unmatched name and its caller ignores the result, so every binding after it was left
  pointing at the null resource. The reflection now files a storage buffer as an SRV
  when any member of its block is `NonWritable`, the rule SPIRV-Reflect applies inside
  `FidelityFX_SC.exe`. Across the 300 generated blobs that changes one classification,
  `r_counters` in `ffx_frameinterpolation_pass`, which binds through the SRV table.
- **The descriptor pool dropped its storage-buffer entry.** `poolSizeCount` was 5 for
  a six-entry array (the sampler entry appears twice), so the pool held no
  `STORAGE_BUFFER` descriptors.
- **Image views were destroyed while in flight.** Each effect context keeps a ring of
  per-frame dynamic image views and advances it on every `UnregisterResources`.
  Interpolation-only frame interpolation unregisters twice per frame (prepare and
  dispatch), so a ring of `FFX_MAX_QUEUED_FRAMES` (4) slots destroyed the previous
  frame's views while that frame was still executing. The view ring is now
  `FFX_VK_VIEW_QUEUED_FRAMES` = 2 × `FFX_MAX_QUEUED_FRAMES` slots; descriptor-set and
  constant-buffer rings are unchanged.

With these, a frame-interpolation frame is clean under both core and synchronization
validation.

### Diagnostics

Setting `FFX_DIAG_PASSES=1` enables per-pass logging to stderr — dispatch order,
internal resource creation sizes, and `prepare_inputs` blob selection. It is off by
default; upstream logged unconditionally.

---

## Licence and attribution

The FidelityFX SDK is **© 2024 Advanced Micro Devices, Inc.** and is distributed under
the **MIT licence**. That licence is unchanged and applies to this fork in full — see
[`LICENSE.txt`](LICENSE.txt), reproduced verbatim from upstream.

All modifications described above are contributed under the same MIT licence. AMD's
copyright notices in the source files are preserved as-is; this fork claims no ownership
over upstream code.

- Upstream project: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>
- FidelityFX and Radeon are trademarks of Advanced Micro Devices, Inc. Use here is
  descriptive; this fork is not affiliated with or endorsed by AMD.
