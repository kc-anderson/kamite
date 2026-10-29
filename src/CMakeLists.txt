# Game layer (kamite.exe) and the recompiled game code. Adapted from the Urban Reign port's src/CMakeLists.txt.

# KAMITE_ARCH: instruction set the compiler may use in the generated code (game code + VU1 AOT, the bulk of the build).
# SSE41 (default): one exe for every CPU with SSE4.1 (MSVC has no /arch:SSE4.2, so its own code stays SSE2 and the
# runtime's explicit SSE4.1 intrinsics still apply). AVX2: only to compare speed; the exe then needs an AVX2 CPU.
set(KAMITE_ARCH "SSE41" CACHE STRING "Instruction set of the generated code: SSE41 (default) or AVX2 (comparison only)")
set_property(CACHE KAMITE_ARCH PROPERTY STRINGS SSE41 AVX2)
if(KAMITE_ARCH STREQUAL "AVX2")
    set(KAMITE_ARCH_FLAG /arch:AVX2)
else()
    set(KAMITE_ARCH_FLAG "")
endif()

set(KAMITE_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")
set(KAMITE_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")

if(NOT EXISTS "${KAMITE_GENERATED_DIR}/register_functions.cpp")
    if(KAMITE_BUILD_RECOMP)
        # Configure for building ps2_recomp only (build.py generate on a fresh tree): no kamite target yet.
        message(WARNING
            "Recompiled game code not found in ${KAMITE_GENERATED_DIR}; configuring without the kamite target.\n"
            "Build ps2_recomp, then run: python build.py generate")
        return()
    endif()
    message(FATAL_ERROR
        "Recompiled game code not found in ${KAMITE_GENERATED_DIR}.\n"
        "Put your own disc image (USA, SLUS-21503) in game/ and run: python build.py generate")
endif()

file(GLOB KAMITE_GENERATED_SOURCES CONFIGURE_DEPENDS "${KAMITE_GENERATED_DIR}/*.cpp")

set(KAMITE_HOST_SOURCES
    main.cpp
    kamite/button_prompt_sheets.cpp
    kamite/button_prompts.cpp
    kamite/crate_prompts.cpp
    kamite/call_profile.cpp
    kamite/guest_symbols.cpp
    kamite/call_trace.cpp
    kamite/movie_watchdog.cpp
    kamite/frame_patches.cpp
    kamite/game_options.cpp
    kamite/character_mods.cpp
    kamite/disc_files.cpp
    kamite/hd_textures.cpp
    kamite/prompt_hd.cpp
    kamite/style_presets.cpp
    kamite/texture_slots.cpp
    kamite/jukebox.cpp
    kamite/widescreen.cpp
    kamite/camera.cpp
    kamite/npc_poses.cpp
    kamite/debug_tools.cpp
    kamite/spawn_tools.cpp
    kamite/hitbox_overlay.cpp
    kamite/menu_buttons.cpp
    kamite/modern_controls.cpp
    kamite/near_geometry.cpp
    kamite/crash_history.cpp
    kamite/hud_layout.cpp
    kamite/hotkeys.cpp
    kamite/dev_menu.cpp
    kamite/roulette_bar.cpp
    kamite/radar_glow.cpp
    kamite/ini_file.cpp
    kamite/counter_finder.cpp
    kamite/test_mode.cpp
    kamite/message_guard.cpp
    kamite/bone_tweaks.cpp
    kamite/pc_sampler.cpp
    kamite/function_hooks.cpp
    kamite/game_disc.cpp
    kamite/hang_dump.cpp
    kamite/hle_bindings.cpp
    kamite/host_window.cpp
    kamite/log.cpp
    kamite/prompt_art.cpp
    kamite/sha256.cpp
    kamite/thread_policy.cpp
)

# kamite.ini (top level) is built into kamite.exe as its default settings: kamite_generated/kamite/default_ini.h.
# The header is rewritten only when the text changes, so editing kamite.ini rebuilds main.cpp alone.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/kamite.ini")
file(READ "${CMAKE_SOURCE_DIR}/kamite.ini" KAMITE_DEFAULT_INI)
string(LENGTH "${KAMITE_DEFAULT_INI}" KAMITE_DEFAULT_INI_LENGTH)
if(KAMITE_DEFAULT_INI_LENGTH GREATER 16000)
    message(FATAL_ERROR "kamite.ini is ${KAMITE_DEFAULT_INI_LENGTH} bytes; MSVC string literals stop at 16380 (split it)")
endif()
if(KAMITE_DEFAULT_INI MATCHES "\\)KMINI\"")
    message(FATAL_ERROR "kamite.ini must not contain )KMINI\" (the raw string delimiter)")
endif()
set(KAMITE_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/default_ini.h")
file(WRITE "${KAMITE_INI_HEADER}.tmp"
    "// Generated from kamite.ini by src/CMakeLists.txt. Do not edit.\n"
    "#pragma once\n"
    "namespace kamite\n"
    "{\n"
    "    inline constexpr const char *kDefaultIni = R\"KMINI(${KAMITE_DEFAULT_INI})KMINI\";\n"
    "}\n")
file(COPY_FILE "${KAMITE_INI_HEADER}.tmp" "${KAMITE_INI_HEADER}" ONLY_IF_DIFFERENT)

# Does the runtime have ps2_host_display::setHeadless (LightVelox/PS2Recomp sotc-port 29d26f9)? The Urban Reign runtime
# (2fa53c4) does not; kamite/host_window.cpp then hides the raylib window itself.
file(STRINGS "${KAMITE_RUNTIME_INCLUDE}/runtime/ps2_display_options.h" KAMITE_SET_HEADLESS_DECL REGEX "void[ \t]+setHeadless[ \t]*\\(")
if(KAMITE_SET_HEADLESS_DECL)
    set(KAMITE_RUNTIME_HAS_SET_HEADLESS 1)
else()
    set(KAMITE_RUNTIME_HAS_SET_HEADLESS 0)
endif()
message(STATUS "Kamite: runtime setHeadless available: ${KAMITE_RUNTIME_HAS_SET_HEADLESS}")

# Reuse an already-built game code library when only host sources change (the full EE build is long).
set(KAMITE_GAME_CODE_LIBRARY "" CACHE FILEPATH "Reuse an unchanged generated EE library for host-only builds")
if(KAMITE_GAME_CODE_LIBRARY)
    if(NOT EXISTS "${KAMITE_GAME_CODE_LIBRARY}")
        message(FATAL_ERROR "Cached generated EE library does not exist: ${KAMITE_GAME_CODE_LIBRARY}")
    endif()
    add_library(kamite_game_code STATIC IMPORTED)
    set_target_properties(kamite_game_code PROPERTIES IMPORTED_LOCATION "${KAMITE_GAME_CODE_LIBRARY}")
    target_include_directories(kamite_game_code INTERFACE "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code INTERFACE ps2_runtime)
    # The huge-function sources live in a sibling library built next to it.
    get_filename_component(KAMITE_GAME_CODE_LIBRARY_DIR "${KAMITE_GAME_CODE_LIBRARY}" DIRECTORY)
    target_link_libraries(kamite_game_code INTERFACE "${KAMITE_GAME_CODE_LIBRARY_DIR}/kamite_game_code_huge.lib")
else()
    # /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip. Fine for most files, but
    # one huge function (Urban Reign: ~11k guest instructions, 2 MB of C++) ran over 40 minutes with it. Files above the
    # threshold go into kamite_game_code_huge, which keeps MSVC's default huge-function handling.
    set(KAMITE_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
    set(KAMITE_HUGE_SOURCES "")
    foreach(source IN LISTS KAMITE_GENERATED_SOURCES)
        file(SIZE "${source}" sourceBytes)
        if(sourceBytes GREATER KAMITE_HUGE_SOURCE_BYTES AND NOT source MATCHES "register_functions\\.cpp$")
            list(APPEND KAMITE_HUGE_SOURCES "${source}")
        endif()
    endforeach()
    if(KAMITE_HUGE_SOURCES)
        list(REMOVE_ITEM KAMITE_GENERATED_SOURCES ${KAMITE_HUGE_SOURCES})
    endif()
    list(LENGTH KAMITE_HUGE_SOURCES KAMITE_HUGE_COUNT)
    message(STATUS "Kamite: ${KAMITE_HUGE_COUNT} generated files over ${KAMITE_HUGE_SOURCE_BYTES} bytes go to kamite_game_code_huge")

    # An empty static library is an error in some generators: a stub source keeps it valid when no file is huge.
    set(KAMITE_HUGE_STUB "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/huge_stub.cpp")
    file(WRITE "${KAMITE_HUGE_STUB}.tmp" "// Keeps kamite_game_code_huge non-empty.\nnamespace kamite { int hugeStub() { return 0; } }\n")
    file(COPY_FILE "${KAMITE_HUGE_STUB}.tmp" "${KAMITE_HUGE_STUB}" ONLY_IF_DIFFERENT)
    add_library(kamite_game_code_huge STATIC ${KAMITE_HUGE_SOURCES} "${KAMITE_HUGE_STUB}")
    target_include_directories(kamite_game_code_huge PUBLIC "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code_huge PUBLIC ps2_runtime)
    # Each huge file needs several GB in MSVC's pass 2; compiling them all at once runs out of heap (C1002).
    set(KAMITE_HUGE_JOBS 2 CACHE STRING "Concurrent compiles of the huge generated sources (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS kamite_huge=${KAMITE_HUGE_JOBS})
    set_property(TARGET kamite_game_code_huge PROPERTY JOB_POOL_COMPILE kamite_huge)
    if(MSVC)
        target_compile_options(kamite_game_code_huge PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /W0)
        # Huge files that still compile in reasonable time with /d2OptimizeHugeFunctions (file names, ';'-separated).
        # Urban Reign listed three of its own here; the game's are not measured yet (TODO after the first build).
        set(KAMITE_HUGE_OPTIMIZED "" CACHE STRING "Huge generated files still compiled with /d2OptimizeHugeFunctions")
        foreach(source IN LISTS KAMITE_HUGE_SOURCES)
            get_filename_component(sourceName "${source}" NAME)
            if(sourceName IN_LIST KAMITE_HUGE_OPTIMIZED)
                set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/d2OptimizeHugeFunctions")
            endif()
        endforeach()
    endif()

    add_library(kamite_game_code STATIC ${KAMITE_GENERATED_SOURCES})
    target_link_libraries(kamite_game_code PUBLIC kamite_game_code_huge)
    target_include_directories(kamite_game_code PUBLIC "${KAMITE_GENERATED_DIR}")
    target_link_libraries(kamite_game_code PUBLIC ps2_runtime)
    set_target_properties(kamite_game_code PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(kamite_game_code PRIVATE
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${KAMITE_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${KAMITE_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        # No /GL here, ever: whole-program optimization over ~10k generated files blew up link time and memory (Urban
        # Reign docs/ROADMAP.md A4), and the game's own Release/LTCG link died in "Generating code" (STATUS_2026-10-16.md).
        target_compile_options(kamite_game_code PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
        # One ~100k-line table initializer that runs once at startup: compiles in seconds without PCH/optimizer.
        set_source_files_properties("${KAMITE_GENERATED_DIR}/register_functions.cpp" PROPERTIES
            COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

# Recompiled SNR2 modules (the game's DLLs): 'python -I build.py modules <mod>' writes generated_modules/<mod>/ (the
# module's functions in namespace km_<mod>, its header <mod>_functions.h and <mod>_pack.cpp; never
# register_functions.cpp, which would define the boot table's globals a second time). All modules go into ONE static
# library with ONE runtime-only precompiled header: every room pack is generated for the same slot base (see
# KAMITE_PORT_NOTES.md section 9), so their address-suffixed names repeat and live in per-module namespaces, and a
# per-module PCH (~160 MB each) would cost ~13 GB for 81 modules. The module headers are not in the PCH (each source
# includes its own "<mod>_functions.h" from its folder). kamite_generated/kamite/module_index.cpp lists the packs
# (and keeps the linker from dropping them).
set(KAMITE_MODULES_DIR "${CMAKE_SOURCE_DIR}/generated_modules")
file(GLOB KAMITE_MODULE_PACKS CONFIGURE_DEPENDS "${KAMITE_MODULES_DIR}/*/*_pack.cpp")
set(KAMITE_MODULE_SOURCES "")
set(KAMITE_MODULE_DECLS "")
set(KAMITE_MODULE_REFS "")
set(KAMITE_MODULE_COUNT 0)
foreach(modulePack IN LISTS KAMITE_MODULE_PACKS)
    get_filename_component(moduleDir "${modulePack}" DIRECTORY)
    get_filename_component(moduleName "${moduleDir}" NAME)
    if(NOT moduleName MATCHES "^[A-Za-z_][A-Za-z0-9_]*$" OR NOT modulePack MATCHES "/${moduleName}_pack\\.cpp$")
        message(FATAL_ERROR "generated_modules/${moduleName}: module folder must be a C identifier holding ${moduleName}_pack.cpp")
    endif()
    if(EXISTS "${moduleDir}/register_functions.cpp")
        message(FATAL_ERROR "${moduleDir}/register_functions.cpp would redefine the boot function table (rerun build.py modules)")
    endif()
    if(EXISTS "${moduleDir}/ps2_recompiled_functions.h" OR NOT EXISTS "${moduleDir}/${moduleName}_functions.h")
        message(FATAL_ERROR "${moduleDir} has the old module layout (rerun: python -I build.py modules --force ${moduleName})")
    endif()
    file(GLOB moduleSources CONFIGURE_DEPENDS "${moduleDir}/*.cpp")
    list(APPEND KAMITE_MODULE_SOURCES ${moduleSources})
    string(APPEND KAMITE_MODULE_DECLS "    extern const ModulePack kModulePack_${moduleName};\n")
    string(APPEND KAMITE_MODULE_REFS "&kModulePack_${moduleName}, ")
    math(EXPR KAMITE_MODULE_COUNT "${KAMITE_MODULE_COUNT} + 1")
endforeach()
set(KAMITE_MODULE_LIBRARIES "")
if(KAMITE_MODULE_SOURCES)
    add_library(kamite_modules STATIC ${KAMITE_MODULE_SOURCES})
    target_include_directories(kamite_modules PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(kamite_modules PUBLIC ps2_runtime)
    set_target_properties(kamite_modules PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(kamite_modules PRIVATE
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${KAMITE_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        # Same flags as kamite_game_code (no /GL).
        target_compile_options(kamite_modules PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
    set(KAMITE_MODULE_LIBRARIES kamite_modules)
endif()
message(STATUS "Kamite: ${KAMITE_MODULE_COUNT} recompiled SNR2 module(s) in ${KAMITE_MODULES_DIR}")
set(KAMITE_MODULE_INDEX "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/module_index.cpp")
file(WRITE "${KAMITE_MODULE_INDEX}.tmp"
    "// Generated by src/CMakeLists.txt from generated_modules/*/*_pack.cpp. Do not edit.\n"
    "#include \"kamite/module_registry.h\"\n"
    "namespace kamite\n"
    "{\n"
    "${KAMITE_MODULE_DECLS}"
    "    const ModulePack *const *modulePacks(size_t &count)\n"
    "    {\n"
    "        static const ModulePack *const packs[] = {${KAMITE_MODULE_REFS}nullptr};\n"
    "        count = ${KAMITE_MODULE_COUNT}u;\n"
    "        return packs;\n"
    "    }\n"
    "}\n")
file(COPY_FILE "${KAMITE_MODULE_INDEX}.tmp" "${KAMITE_MODULE_INDEX}" ONLY_IF_DIFFERENT)

add_executable(kamite ${KAMITE_HOST_SOURCES} "${KAMITE_MODULE_INDEX}")
# The checkout's commit for the test cases' meta (kamite_generated/kamite/git_commit.h), refreshed at every build.
set(KAMITE_COMMIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated/kamite/git_commit.h")
execute_process(COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KAMITE_COMMIT_HEADER}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/git_commit.cmake")
add_custom_target(kamite_git_commit
    COMMAND "${CMAKE_COMMAND}" -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KAMITE_COMMIT_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/git_commit.cmake"
    BYPRODUCTS "${KAMITE_COMMIT_HEADER}"
    VERBATIM)
add_dependencies(kamite kamite_git_commit)
if(KAMITE_MODULE_LIBRARIES)
    target_link_libraries(kamite PRIVATE ${KAMITE_MODULE_LIBRARIES})
endif()
# The port's own resources (resources/images/logo.png: the launcher's logo, npc_motions, prompts) next to the exe,
# merged into the resources/ folder pcsx2gs.dll also fills (shaders, OSD font). A target of its own that runs at every
# build of kamite (the build script builds --target kamite): as a POST_BUILD step it ran only when kamite.exe was
# linked again, so a change to resources/ alone never reached the build (Oct 26: the counter idle takes).
add_custom_target(kamite_resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:kamite>/resources"
    COMMENT "Copying the port's resources")
add_dependencies(kamite kamite_resources)
target_include_directories(kamite PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/kamite_generated"
    "${KAMITE_GENERATED_DIR}")
target_link_libraries(kamite PRIVATE kamite_game_code ps2_runtime)
target_compile_definitions(kamite PRIVATE KAMITE_RUNTIME_HAS_SET_HEADLESS=${KAMITE_RUNTIME_HAS_SET_HEADLESS})

if(MSVC)
    target_compile_options(kamite PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(kamite PRIVATE _CRT_SECURE_NO_WARNINGS KAMITE_ARCH_NAME="${KAMITE_ARCH}")
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    # /LTCG in RelWithDebInfo as Urban does: ps2_runtime is compiled with /GL by EnableFastReleaseMode
    # (runtime/ps2xRuntime/cmake/ReleaseMode.cmake, CMakeLists.txt:609-611), so the link does code generation for the
    # runtime objects only. The generated game code never gets /GL (above), so it is not part of that step.
    target_link_options(kamite PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    target_link_libraries(kamite PRIVATE dbghelp winmm)
endif()

if(KAMITE_DEBUG_UI AND TARGET rlImGui)
    target_sources(kamite PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_include_directories(kamite PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
    target_link_libraries(kamite PRIVATE rlImGui imgui)
    target_compile_definitions(kamite PRIVATE PS2X_ENABLE_DEBUG_UI=1)
    # The launcher (src/kamite/launcher.cpp) draws with the same Dear ImGui.
    target_sources(kamite PRIVATE kamite/launcher.cpp kamite/launcher_platform.cpp)
    target_compile_definitions(kamite PRIVATE KAMITE_HAS_LAUNCHER=1)
endif()

# FFmpeg is off by default (KAMITE_FFMPEG); with it on, the runtime stages its DLLs next to the exe.
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(kamite)
endif()

# VU1 microcode compiled ahead of time: capture the code images with PS2X_VU1_CAPTURE, then run vu1_recomp (configure
# with -DKAMITE_BUILD_TOOLS=ON) into generated/vu1. Images not compiled stay interpreted. GENERIC (Urban, SotC).
file(GLOB KAMITE_VU1_SOURCES CONFIGURE_DEPENDS "${KAMITE_GENERATED_DIR}/vu1/*.cpp")
if(KAMITE_VU1_SOURCES)
    add_library(kamite_vu1_code STATIC ${KAMITE_VU1_SOURCES})
    target_include_directories(kamite_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(kamite_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(kamite PRIVATE kamite_vu1_code)
    target_compile_definitions(kamite PRIVATE KAMITE_HAS_VU1_PROGRAMS=1)
    if(MSVC)
        target_compile_options(kamite_vu1_code PRIVATE /bigobj /fp:precise ${KAMITE_ARCH_FLAG} /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

set_target_properties(kamite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
