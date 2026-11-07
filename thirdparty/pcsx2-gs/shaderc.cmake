# shaderc_shared.dll: the GLSL -> SPIR-V compiler the Vulkan device loads at run time (VKShaderCache.cpp, dyn_shaderc:
# shaderc_shared.dll next to the exe). Built from source at the versions PCSX2's own dependency build pins
# (.github/workflows/scripts/windows/build-dependencies.bat at PCSX2 695e267, same URLs and hashes), without PCSX2's
# shaderc patch: VKShaderCache.cpp has the stock-API path. Licences: shaderc, SPIRV-Tools, SPIRV-Headers Apache-2.0
# (SPIRV-Headers also MIT); glslang BSD-3-Clause / MIT / Apache-2.0 parts.
include(FetchContent)

# The three dependencies as sources only (SOURCE_SUBDIR names no CMakeLists.txt, so FetchContent_MakeAvailable just
# downloads them); shaderc's third_party/CMakeLists.txt adds them from the SHADERC_*_DIR paths below.
FetchContent_Declare(kamite_glslang
    URL "https://github.com/KhronosGroup/glslang/archive/275822a6261ee689aadb1da5f09a0ec2f058685c.zip"
    URL_HASH SHA256=2b63189efad0348d88d410a5e12ec550a612e0b6ceef64624b8f45491269fb9c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR kamite-sources-only)
FetchContent_Declare(kamite_spirv_headers
    URL "https://github.com/KhronosGroup/SPIRV-Headers/archive/58006c901d1d5c37dece6b6610e9af87fa951375.zip"
    URL_HASH SHA256=d2f071e94c081f5a4606559770ebf1f7d1eac92a1def0c3e10609844aa8b69b2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR kamite-sources-only)
FetchContent_Declare(kamite_spirv_tools
    URL "https://github.com/KhronosGroup/SPIRV-Tools/archive/6337eb62cadd7d124ac6789bf39c0f71148f0a73.zip"
    URL_HASH SHA256=4011be89aa73e3461c9deef73936a62c79a3097590c5135d058041cc9fb99c6f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR kamite-sources-only)
FetchContent_MakeAvailable(kamite_glslang kamite_spirv_headers kamite_spirv_tools)

set(SHADERC_GLSLANG_DIR "${kamite_glslang_SOURCE_DIR}" CACHE STRING "" FORCE)
set(SHADERC_SPIRV_HEADERS_DIR "${kamite_spirv_headers_SOURCE_DIR}" CACHE STRING "" FORCE)
set(SHADERC_SPIRV_TOOLS_DIR "${kamite_spirv_tools_SOURCE_DIR}" CACHE STRING "" FORCE)

# Only the library: no tests, examples, glslc or -Werror; the shared CRT like the rest of the port. Install rules stay
# at their defaults (never run): with SHADERC_SKIP_INSTALL shaderc hands glslang the string "$<NOT:ON>", which reads as
# true, and generation fails on glslang's export set (PCSX2's patch works round the same thing).
foreach(option SHADERC_SKIP_TESTS SHADERC_SKIP_EXAMPLES SHADERC_SKIP_EXECUTABLES SHADERC_SKIP_COPYRIGHT_CHECK
        SHADERC_ENABLE_SHARED_CRT SPIRV_SKIP_TESTS SPIRV_SKIP_EXECUTABLES SPIRV_HEADERS_SKIP_EXAMPLES)
    set(${option} ON CACHE BOOL "" FORCE)
endforeach()
foreach(option SHADERC_ENABLE_WERROR_COMPILE SPIRV_WERROR ENABLE_GLSLANG_BINARIES GLSLANG_TESTS)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(kamite_shaderc
    URL "https://github.com/google/shaderc/archive/refs/tags/v2026.2.zip"
    URL_HASH SHA256=f9401cc5cb36c276cd1e072b6595dbd728148e8dba389e50f7339e2d388dbc08
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(kamite_shaderc)

# The header VKShaderCache.cpp includes (the library itself is loaded at run time, never linked).
set(KAMITE_SHADERC_INCLUDE_DIR "${kamite_shaderc_SOURCE_DIR}/libshaderc/include")
