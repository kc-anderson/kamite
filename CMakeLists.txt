cmake_minimum_required(VERSION 3.21)

# The game (USA, SLUS-21503) native PC port. Adapted from the Urban Reign port's top-level CMakeLists.txt
# (forks/urban-reign-recomp @2fa53c4); KAMITE_PORT_NOTES.md lists what changed.
project(Kamite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# runtime/: the PS2Recomp runtime (EE/IOP/VU/GS emulation), recompiler and offline tools (LightVelox/PS2Recomp
# sotc-port ac9efa0 + the Urban Reign port's changes). thirdparty/: PCSX2's GS and what it needs (optional below).
option(KAMITE_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
# runtime/ps2xTest holds the offline tools: vu1_recomp (VU1 microcode AOT), gs_replay, vu0_audit.
option(KAMITE_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
# runtime/CMakeLists.txt (Urban's) reads these two names.
set(URBAN_BUILD_RECOMP ${KAMITE_BUILD_RECOMP})
set(URBAN_BUILD_TOOLS ${KAMITE_BUILD_TOOLS})

# In-game ImGui "Runtime Debugger" (threads, kernel, IOP/SIF, RPC history, GS, DMA, memory); F1 toggles it.
option(KAMITE_DEBUG_UI "Build the ImGui runtime debugger into kamite.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${KAMITE_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoder for the libmpeg HLE. OFF: ON downloads a prebuilt LGPL FFmpeg at configure time
# (runtime/ps2xRuntime/CMakeLists.txt, ExternalProject ffmpeg_external).
option(KAMITE_FFMPEG "Decode MPEG video with FFmpeg (downloads a prebuilt FFmpeg)" OFF)
set(PS2X_ENABLE_FFMPEG ${KAMITE_FFMPEG} CACHE BOOL "" FORCE)
# Controllers through SDL3 (downloads the SDL3 source at configure time; SDL3.dll goes next to kamite.exe): XInput,
# PlayStation, Switch and Steam controllers, vibration. OFF (or PS2X_SDL_INPUT=0 at run time): GLFW's gamepads.
option(KAMITE_SDL_INPUT "Read controllers through SDL3" ON)
set(PS2X_ENABLE_SDL_INPUT ${KAMITE_SDL_INPUT} CACHE BOOL "" FORCE)
# Runtime logs: the normal ones stay, the very verbose ones (per-call function log tracker) are off.
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory(runtime EXCLUDE_FROM_ALL)
add_subdirectory(src)

# PCSX2's GS (hardware renderer) as pcsx2gs.dll (DX11), loaded at run time when [Video] Renderer=pcsx2 (the default).
# ON by default: its wrapper carries the game's GameIndex fixes and serial (thirdparty/pcsx2-gs/compat/Bridge.cpp,
# HostStubs.cpp). The runtime falls back to its own GS when the DLL is missing (gs_pcsx2_backend.cpp,
# GSPcsx2Backend::Requested), and -DKAMITE_PCSX2_GS=OFF builds without it.
option(KAMITE_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll)" ON)
if(KAMITE_PCSX2_GS AND WIN32)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/pcsx2-gs/CMakeLists.txt")
        message(FATAL_ERROR "KAMITE_PCSX2_GS=ON needs thirdparty/pcsx2-gs (copy it from the Urban Reign port)")
    endif()
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    if(TARGET kamite)
        add_dependencies(kamite pcsx2gs)
        target_link_libraries(kamite PRIVATE pcsx2gs_api) # the F2 menu's UI table (src/kamite/camera.cpp)
    endif()
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
endif()

# KAMITE_LTO: also /GL for ps2_iop and the port sources (linked with /LTCG). ps2_runtime always gets /GL in
# RelWithDebInfo/Release from the runtime's own EnableFastReleaseMode, and kamite.exe links /LTCG in RelWithDebInfo
# (src/CMakeLists.txt). The generated code (~10k files) never gets /GL.
option(KAMITE_LTO "Whole-program optimization of the runtime, the IOP and the port (not the generated code)" OFF)
if(MSVC AND KAMITE_LTO AND TARGET kamite)
    foreach(target ps2_runtime ps2_iop kamite)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/GL>)
    endforeach()
    set_property(TARGET ps2_runtime ps2_iop APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
    target_link_options(kamite PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
endif()
