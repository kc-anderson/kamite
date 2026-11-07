# thirdparty/

| folder | what | from | license |
|---|---|---|---|
| `pcsx2/` | PCSX2's GS (hardware renderer: DX11 and Vulkan; no DX12), `common/` and the libraries they use (fmt, imgui, cpuinfo, Vulkan headers + VMA, xbyak, fast_float, WIL, xxhash) | [PCSX2/pcsx2](https://github.com/PCSX2/pcsx2) commit `2954ebe` (2026-10-11), plus its GS fixes up to `da6bae1` (2026-11-01: texture cache, shuffles, transfers, DS-as-RT feedback; not the FullscreenUI blur), `d1662db` (PSMT4 bounds) and the Vulkan renderer, its shaders and `3rdparty/vulkan` from `695e267` (2026-11-07). shaderc (the Vulkan shader compiler) is built from source at configure time: `pcsx2-gs/shaderc.cmake` | GPL-3.0 (`pcsx2/COPYING.GPLv3`); each library's license is in its folder |
| `pcsx2-gs/` | ours: builds `pcsx2/` into `pcsx2gs.dll` with a C interface (`include/pcsx2gs_api.h`), the host stubs, the shaders/font and the shader cache. See `docs/GS_PCSX2.md` | this project | GPL-3.0 |
| `directx-headers/` | `include/` of DirectX-Headers (`directx/d3d12.h` and friends, needed by PCSX2's DX12 code) | [microsoft/DirectX-Headers](https://github.com/microsoft/DirectX-Headers) commit `461ef51` | MIT (`directx-headers/LICENSE`) |

`pcsx2/` holds only the files the build compiles or includes: the sources listed in `pcsx2-gs/CMakeLists.txt` and
the headers from Ninja's dependency log of a full build (`ninja -t deps`), plus the license files and the whole
`3rdparty/cpuinfo` (it is built with its own CMakeLists). The folder layout is PCSX2's, so a newer PCSX2 can be
copied over the same paths. `pcsx2-gs/tools/make_loaders.py` and `extract_config.py` read a full PCSX2 checkout.
