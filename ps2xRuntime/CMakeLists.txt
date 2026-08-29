cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 8 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)
option(PS2X_G652_CRITICAL_TRACE "Compile the G652 cross-thread critical-path trace" OFF)
option(PS2X_G653_POOLDIAG "Compile the G653 cooperative-dispatch census and spin prototype" OFF)
# G654 P5: the G629-G636 persistent GPU-resident GS domain prototype. CLOSED NEGATIVE ON
# PERFORMANCE and default-OFF at runtime, but 8.52% of ps2_gs_rasterizer.cpp's machine code - and
# rule 12c says compiled-but-never-executed code in a hot TU is not free. OFF compiles the
# constant-folding stubs in g654_stub_gpudomain.inc / g654_stub_g629.inc instead.
option(PS2X_G654_GPUDOMAIN "Compile the G629-G636 persistent GS domain prototype" OFF)
# G654 P16/P17: the exclusive, thread-keyed layer timer. `[G146:perf]`'s four buckets are inclusive,
# nested (GIFsubmit sits INSIDE VIF1) and summed across threads, which is how `fight:rain` produced a
# NEGATIVE 8.71 ms/f residue against its own pole. This build mode adds the non-overlapping,
# per-thread companion. Diagnostic only - never in a timing arm.
option(PS2X_G654_DIAG "Compile the G654 exclusive per-thread layer timer" OFF)
# G655 P13-P16: large, default-off census families in the hottest renderer TU.  Keep each real
# implementation available for evidence/reproduction, but compile a constant-foldable stub in a
# normal build.  Runtime environment guards alone did not remove their machine-code footprint.
option(PS2X_G620_DIAG "Compile the G620 texture-decode census" OFF)
option(PS2X_G343_DIAG "Compile the G343 local-transfer census and closed prototype" OFF)
option(PS2X_G342_DIAG "Compile the G342 texture-alias census and closed prototype" OFF)
option(PS2X_G401_DIAG "Compile the G401 composite surface probe" OFF)
option(PS2X_G605_CYC_DIAG "Compile the G605 replay-leaf cycle census" OFF)
option(PS2X_G655_LEGACY_DIAG "Compile closed legacy G214/G336/G369/G400/G526/G595 diagnostics" OFF)
option(PS2X_G419_DIAG "Compile the G419 within-process A/B parser and reporter" OFF)
option(PS2X_G655_DEPTH_DIAG "Compile the G655 discovered-target depth-coherence trace" OFF)
option(PS2X_G656_DIAG "Compile the G656 upload-edge and bind-loop lap timers" OFF)
option(PS2X_G657_DIAG "Compile the G657 fallback-wall and GPU-coverage census" OFF)
option(PS2X_G657_FOGSIMD "Compile the G657 exact SSE2 fog lerp (built, exact, NOT promoted)" OFF)
option(PS2X_G656_TEXREPL "Compile the HD texture replacement and dumping subsystem" OFF)
option(PS2X_G656_VERIFY "Compile the G255 same-run CPU verify oracle and G628 mask census" OFF)
# G658 P2-P4/P9: mode-6 stage oracle. This changes rendered values deliberately and exists only in
# the diagnostic build used with the G656 same-run CPU/GPU verifier. Keep both hot TUs byte-free in
# normal Release builds (rule 12c).
option(PS2X_G658_DIAG "Compile the G658 mode-6 source/blend/coverage stage oracle" OFF)
# G659 P1/P3-P7/P10: the rewrite-depth / ordered-destination census. Pure instrumentation — it
# changes no rendered value — but it costs a per-pixel counter in the CPU replay's commit point,
# so it is a compile-time build mode with `#if`-excluded call sites (rule 12c).
option(PS2X_G659_DIAG "Compile the G659 mode-6 rewrite-depth and destination census" OFF)
# G660 P2/P3/P4: the `colorzalias` rejection-TERM census. Pure instrumentation — it changes no
# rendered value and no admission — but its call site is inside g178TryFlushGpu in the hottest TU,
# so it is a compile-time build mode with an `#if`-excluded call site (rule 12c).
option(PS2X_G660_DIAG "Compile the G660 colorzalias rejection-term census" OFF)
# G661 P9: the arm counter + reporter for the exact destination-floor blend repair. The LEVER is a
# runtime flag in every build (it selects an execution arm, not a diagnostic); only its Law-10
# counter proof is compile-time, because that counter's call site is in the per-draw GPU submit
# loop of a hot TU and measured +279 code bytes / +1 COMDAT there (rule 12c).
option(PS2X_G661_DIAG "Compile the G661 destination-floor blend arm counter" OFF)
# G662: the phase's COMPLETE instrument set, declared in one place before the first diagnostic
# build (skill/SKILL.md §1.6b rule 1 — G661 relinked three times because it added one census per
# priority). Holds [G662:zauth] (the re-scoped private-depth-mirror writer histogram, at the real
# CPU band-replay choke point instead of inside g261PrepareCpuReplay), [G662:matgran] (kG261MatLocal
# publication granularity per flush edge) and [G662:lever] (arm counters). Every call site is in
# ps2_gs_rasterizer.cpp, the hottest TU, so this is a compile-time build mode (rule 12c).
option(PS2X_G662_DIAG "Compile the G662 depth-authority, materialization-granularity and lever censuses" OFF)
# G663: the bind pre-pass scan census. [G656:bindstage] measured `tail` at 56-61% of the whole bind
# loop (1344 cyc/entry on `dragon`), and the tail's dominant term is a PER-ENTRY scan of all 14
# residency slots (kG248TargetCount). [G663:bindscan] counts the dirty-target population that scan
# is looking for, so the hoist's ceiling is measured before its timing arm is believed. Call sites
# are in ps2_gs_rasterizer.cpp, the hottest TU => compile-time build mode (rule 12c).
option(PS2X_G663_DIAG "Compile the G663 bind-scan population census" OFF)
# G666: the phase's COMPLETE instrument set, declared in one place before the first diagnostic build
# (G661 retrospective rule 1 — G661 rebuilt its diagnostic binary three times because each new
# priority suggested one more census, and every rebuild invalidates the captures already taken).
# Two censuses, both with `#if`-excluded call sites in ps2_gs_rasterizer.cpp (rule 12c) and storage
# in the cold TU ps2_g666_diag.cpp (rule 12b):
#   [G666:discovterm] — which conjunct blocks each DISCOVERED-TARGET batch, separating a
#       PERFORMANCE gate (width, G662's +0.828 ms/f withdrawal) from a POLICY gate (G660's blanket
#       NODEPTH refusal) from a genuinely missing GPU depth authority (foreign ZBP/ZPSM/scope).
#   [G666:zrate] — the depth divergence rate at a MATCHED WRITE DEPTH. The inherited `zbad` is
#       guarded by `s.g570Depth` (so it cannot fire for the class it exonerates) and denominated
#       over the whole depth rect; this re-denominates to the touched population.
option(PS2X_G666_DIAG "Compile the G666 discovered-target term census and matched-depth Z oracle" OFF)
# G669: the EARLY-ISSUE arm for the colour readback, at the two sites that actually carry its cost.
# G668 read `[G455:runway] = 0.000 ms` everywhere and could not use it — `runwayNs` accumulates only
# under `if (arm.early)`, and `early=true` had ONE producer (the local-COPY edge) which credits only
# Local139/Display and was refused by a pending G264 upload mirror on 98.1% of its opportunities;
# kG455Resync139 had no early-arm site at all. That zero is a Rule 38 zero-sample artifact, not a
# measurement. This adds the arm where the FBO content actually becomes final — the G261 wave tail
# and the G276 coalesce tail — so the runway can be measured with a non-zero valid population.
# Behaviour-pure (a timestamp and a row window; no GL call, no decision changed). Both call sites are
# in ps2_gs_rasterizer.cpp, the hottest TU, so they are `#if`-excluded (rule 12c); the storage is
# four small arrays inside the existing G455 part rather than a new TU, because the arm struct they
# write is file-local to that part.
option(PS2X_G669_DIAG "Compile the G669 readback early-issue runway arm" OFF)
# G664 P4: NO OPTION IS DEFINED HERE ON PURPOSE. The phase built `PS2X_G664_LEGACY_VU1ARMS` to
# compile out the VU1 block A/B arms and measured it at -3,584 bytes against a 211,024-byte target
# - a null, because `g490BlockBody` is also instantiated by the G610/G612 native FALLBACK paths and
# not only by the arm selector. Reverted under Rule 31. The re-open condition and the instrument it
# would need are written at the call site, over `g499BlkArmRun()` in
# src/lib/ps2_vu1_parts/vu1_g490_block_run.inc.
# G665: VU1 backend state MIGRATION (GPU -> CPU) plus its round-trip self-test. G566 is a one-way
# door - it seeds CPU state into the GPU and never imports back - which is why a GPU failure is
# fail-closed and why a per-workload backend selector was impossible. The import primitive is ~4.3 KB
# in ps2_vu1.obj, the SECOND HOTTEST TU (layout-guard budget 0), and this phase landed no shipped
# caller for it, so it is compile-time gated under Rule 31. Make it unconditional in the same edit
# that lands a caller. Call site: src/lib/ps2_vu1_parts/vu1_g665_backend_migration.inc.
option(PS2X_G665_DIAG "Compile the G665 VU1 backend state-migration primitives and oracle" OFF)
# G665: the TRANSLATED GPU VU1 backend - whole-microprogram VU1 -> GLSL specialization of G543's
# exact executor, plus its corpus bench graded by G543's own section oracle. Lives in
# ps2_gs_gpu_raster.cpp, which is layout-guarded at budget 0 (Rule 12b), so it is fully
# `#if`-excluded until it has a promoted caller. Call sites:
# src/lib/ps2_gs_gpu_raster_parts/g665_vu_{translate,bench}.inc.
option(PS2X_G665_GPUVU "Compile the G665 translated GPU VU1 backend and its corpus bench" OFF)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)
    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()
        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)
if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
   (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")
    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")
if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )
    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
            "${PS2X_VITA_PREFIX}/include/raylib"
            "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()
    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        find_library(${out_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )
        if(NOT ${out_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()
        set(${out_var} "${${out_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")
    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)
endif()

# G375: FFmpeg-backed MPEG video decode (ported from upstream PS2Recomp).
if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/ps2_gs_gpu.cpp
    src/lib/ps2_gs_gpu_raster.cpp
    src/lib/ps2_gs_rasterizer.cpp
    src/lib/ps2_gs_memory.cpp
    src/lib/ps2_iop.cpp
    src/lib/ps2_iop_audio.cpp
    src/lib/ps2_iop_dbcman.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_pad.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_runtime_cold.cpp
    src/lib/ps2_gif_sequence_cold.cpp
    src/lib/ps2_gif_arbiter_trace_cold.cpp
    src/lib/ps2_stubs.cpp
    src/lib/ps2_syscalls.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/ps2_vu1.cpp
    src/lib/games_database.cpp
    src/lib/ps2_iso_mount.cpp
    src/lib/ps2_frame_dump.cpp
    src/dc2_auto_stubs.cpp
    src/dc2_game_override.cpp
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
)

# G652 P15: the trace is a diagnostic build mode.  In the default build the shared API include
# exposes constexpr-false inline stubs, so no global load or conditional remains in the VU, GS,
# GL, or frame-end hot paths.  Limit the opt-in definition to the four participating TUs.
# G653 P7/P29/P25: the cooperative-dispatch census and the spin-dispatch prototype are a DIAGNOSTIC
# BUILD MODE, not runtime flags. An order-balanced A B B A measured the runtime-flagged version of
# the very same code — default-OFF, behaviour identical — at +0.484 ms/f on `dragon` purely because
# the BYTES were in ps2_gs_rasterizer.cpp (PS2_PROJECT_STATE.md rule 12b). A `const bool` guard
# cannot buy that back; only excluding the code can.
if(PS2X_G653_POOLDIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g653_pool_census.cpp)
    set_source_files_properties(
        src/lib/ps2_gs_rasterizer.cpp
        PROPERTIES COMPILE_DEFINITIONS "PS2X_G653_POOLDIAG=1"
    )
endif()

if(PS2X_G654_GPUDOMAIN)
    # APPEND, not set: PS2X_G653_POOLDIAG above already writes COMPILE_DEFINITIONS on this same
    # source file, and a plain set_source_files_properties would silently drop it.
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp
                 APPEND PROPERTY COMPILE_DEFINITIONS "PS2X_G654_GPUDOMAIN=1")
endif()

if(PS2X_G654_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g654_layerdiag.cpp)
    foreach(_g654src
            src/lib/ps2_gs_gpu.cpp
            src/lib/ps2_gs_rasterizer.cpp
            src/lib/ps2_memory.cpp
            src/lib/ps2_vif1_interpreter.cpp
            src/dc2_game_override.cpp)
        set_property(SOURCE ${_g654src} APPEND PROPERTY COMPILE_DEFINITIONS "PS2X_G654_DIAG=1")
    endforeach()
endif()

foreach(_g655diag G620 G343 G342)
    if(PS2X_${_g655diag}_DIAG)
        set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                     "PS2X_${_g655diag}_DIAG=1")
    endif()
endforeach()

if(PS2X_G401_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G401_DIAG=1")
endif()

if(PS2X_G605_CYC_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G605_CYC_DIAG=1")
endif()

if(PS2X_G655_LEGACY_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G655_LEGACY_DIAG=1")
endif()

if(PS2X_G419_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G419_DIAG=1")
endif()

if(PS2X_G655_DEPTH_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G655_DEPTH_DIAG=1")
endif()

# G656 P6/P9: the upload-edge stage split and the per-entry bind-loop split. Storage and reporters
# live in a cold TU (rule 12b); the hot TU sees empty structs in the default build.
if(PS2X_G656_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g656_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G656_DIAG=1")
endif()

# G657 P3/P4/P7: the fallback-WALL and GPU-coverage census. `[G583:rejcensus]` ranks the same
# population by ENTRIES, and on `lavaboss` the two rankings disagree by 16x (see the header of
# ps2_g657_diag_api.inc). Storage and reporter live in a cold TU (rule 12b).
if(PS2X_G657_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g657_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G657_DIAG=1")
endif()

# G657 P2b: the exact SSE2 fog lerp. BUILT, PROVEN EXACT (bad=0 on 1.10 G per-pixel comparisons),
# and NOT PROMOTED — at matched denominators it reads -0.89% cyc/inside on the dominant leaf, inside
# the scalar arm's own cross-run drift. It is a compile-time mode rather than a runtime flag so the
# default binary carries none of its bytes (rule 12c).
if(PS2X_G657_FOGSIMD)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G657_FOGSIMD=1")
endif()

# G656 P17: the second hot-TU compile-isolation sweep. Each of these is default-OFF at RUN time in
# a normal Release build, so its machine code is a pure code-layout tax on the hottest TU in the
# build (rule 12c / rule 16). Turning the option ON restores the real implementation byte for byte.
if(PS2X_G656_TEXREPL)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G656_TEXREPL=1")
endif()

if(PS2X_G656_VERIFY)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G656_VERIFY=1")
endif()

if(PS2X_G658_DIAG)
    foreach(_g658src
            src/lib/ps2_gs_rasterizer.cpp
            src/lib/ps2_gs_gpu_raster.cpp)
        set_property(SOURCE ${_g658src} APPEND PROPERTY COMPILE_DEFINITIONS "PS2X_G658_DIAG=1")
    endforeach()
endif()

if(PS2X_G659_DIAG)
    foreach(_g659src
            src/lib/ps2_gs_rasterizer.cpp
            src/lib/ps2_gs_gpu_raster.cpp)
        set_property(SOURCE ${_g659src} APPEND PROPERTY COMPILE_DEFINITIONS "PS2X_G659_DIAG=1")
    endforeach()
endif()

# G660 P2/P3/P4: the colorzalias rejection-term census. Storage and reporter live in a cold TU
# (rule 12b); only ps2_gs_rasterizer.cpp carries the `#if`-excluded call site.
if(PS2X_G660_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g660_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G660_DIAG=1")
endif()

# G661 P9: the arm counter and reporter for the exact destination-floor blend repair. Its call site
# is in the per-draw GPU submit loop in ps2_gs_gpu_raster.cpp, and the first build measured it at
# +279 code bytes / +1 COMDAT in that hot object — the class G653 P25 priced at +0.484 ms/f for a
# behaviourally inert addition. Compile-time build mode (rule 12c); the lever itself stays in every
# build behind the runtime flag, because a runtime flag that selects an ARM is not a diagnostic.
if(PS2X_G661_DIAG)
    foreach(_g661src
            src/lib/ps2_gs_gpu_raster.cpp
            src/lib/ps2_gs_rasterizer.cpp)
        set_property(SOURCE ${_g661src} APPEND PROPERTY COMPILE_DEFINITIONS "PS2X_G661_DIAG=1")
    endforeach()
endif()

# G662: storage and reporters live in a cold TU (rule 12b); only ps2_gs_rasterizer.cpp carries the
# `#if`-excluded call sites.
if(PS2X_G662_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g662_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G662_DIAG=1")
endif()

# G663: same shape as G662 - storage and the reporter live in a cold TU (rule 12b), and only
# ps2_gs_rasterizer.cpp carries the `#if`-excluded call sites.
if(PS2X_G663_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g663_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G663_DIAG=1")
endif()

# G666: same shape as G660/G662/G663 — storage and both reporters live in a cold TU (rule 12b), and
# only ps2_gs_rasterizer.cpp carries the `#if`-excluded call sites (rule 12c).
if(PS2X_G666_DIAG)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_g666_diag.cpp)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G666_DIAG=1")
endif()

# G669: no cold TU — the arm struct and the write clocks it reads are file-local statics inside
# rasterizer_g455_readback_runway.inc, so only ps2_gs_rasterizer.cpp can write them (the same
# constraint that kept PS2X_G665_DIAG out of a cold TU). Four uint64 arrays and two leaf functions.
if(PS2X_G669_DIAG)
    set_property(SOURCE src/lib/ps2_gs_rasterizer.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G669_DIAG=1")
endif()

# G665: the VU1 backend state-migration primitives. Unlike G662/G663 these CANNOT live in a cold TU:
# the hidden VU1 pipeline they import is a set of `thread_local` statics in an anonymous namespace
# inside ps2_vu1.cpp, so only that TU can write them. That is precisely why the option exists -
# ps2_vu1.obj is layout-guarded at budget 0, so the code is excluded until it has a shipped caller.
if(PS2X_G665_DIAG)
    set_property(SOURCE src/lib/ps2_vu1.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G665_DIAG=1")
endif()

# G665 translated GPU VU1 backend. Separate from PS2X_G665_DIAG on purpose: that one gates the
# CPU-side state-migration primitives in ps2_vu1.cpp, this one gates the GPU-side translator in the
# raster TU. They are independently useful and independently priced.
if(PS2X_G665_GPUVU)
    set_property(SOURCE src/lib/ps2_gs_gpu_raster.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                 "PS2X_G665_GPUVU=1")
endif()


if(PS2X_G652_CRITICAL_TRACE)
    target_sources(ps2_runtime PRIVATE src/lib/ps2_critical_trace.cpp)
    foreach(_g652src
            src/lib/ps2_runtime.cpp
            src/lib/ps2_gif_arbiter.cpp
            src/lib/ps2_gs_gpu_raster.cpp
            src/dc2_game_override.cpp)
        set_property(SOURCE ${_g652src} APPEND PROPERTY COMPILE_DEFINITIONS
                     "PS2X_G652_CRITICAL_TRACE=1")
    endforeach()
endif()

file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/*.cpp"
)

set(RUNNER_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/runner/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

# dc2_game_override.cpp is compiled once in ps2_runtime (WHOLEARCHIVE keeps its
# AutoRegister side effects); do not also compile it here or the registrations run twice.

add_executable(ps2EntryRunner
    ${RUNNER_SRC_FILES}
)

if(PS2X_ENABLE_RUNNER_UNITY_BUILD)
    set_target_properties(ps2EntryRunner PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS /MP)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg)
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)

# G375: stage the FFmpeg runtime DLLs beside the executable.
ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

    if(PS2X_VITA_CREATE_PACKAGE)
        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)

    if(MSVC AND WIN32)
        target_link_options(ps2EntryRunner PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
        )
    elseif(MINGW AND WIN32)
        target_link_options(ps2EntryRunner PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
        )
    endif()
 
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE" "/WHOLEARCHIVE:ps2_runtime")
endif()

install(TARGETS ps2_runtime
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)
