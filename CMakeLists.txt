cmake_minimum_required(VERSION 3.16)

project(ntvdmex
    VERSION 0.1.0
    DESCRIPTION "New Technology Virtual DOS Manager, Extended"
    LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# Target platform: Windows XP SP3, 32-bit only. See ADRs 0001/0005.
# Guard against accidentally building with a 64-bit toolchain or a non-Windows
# native compiler -- this project has no other valid target.
# ---------------------------------------------------------------------------
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR
        "NTVDMEX targets Windows only. Configure with the cross toolchain:\n"
        "  cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-xp32-mingw.cmake")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "NTVDMEX is 32-bit only (V86 exists only in 32-bit mode). "
        "Use the i686 mingw-w64 toolchain, not x86_64.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Embedding the SxS manifest gives the Luna theme but is also the most likely
# culprit for an "application configuration is incorrect" load failure on XP.
# Turn OFF to build an un-themed diagnostic binary that has no manifest at all.
option(NTVDMEX_EMBED_MANIFEST "Embed the Common-Controls 6.0 manifest (Luna theme)" ON)

# runtime.c defines memset/memcpy/memmove itself; stop GCC from rewriting those
# loops into a call to the function being defined (infinite recursion).
set_source_files_properties(src/runtime.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-fno-tree-loop-distribute-patterns")

# Common XP-target settings applied to every executable in the project.
#
# We link with NO C runtime: the build toolchain (Homebrew mingw-w64) is
# UCRT-default, and UCRT (api-ms-win-crt-*.dll) is absent on Windows XP, so a
# CRT-linked binary will not load there. -nostdlib drops the CRT and its startup;
# src/runtime.c supplies our own entry point + mem* primitives. The result imports
# only Win32 system DLLs that ship with XP. We also pin the PE subsystem + OS
# version fields to 5.01 so XP's loader accepts the image. (See ADR-0006.)
function(ntvdmex_configure_xp_target tgt)
    target_compile_definitions(${tgt} PRIVATE
        WINVER=0x0501
        _WIN32_WINNT=0x0501)
    # ANSI build (no UNICODE/-municode); -ffreestanding: no hosted C library.
    target_compile_options(${tgt} PRIVATE
        -Wall -Wextra -ffreestanding)
    target_link_options(${tgt} PRIVATE
        -nostdlib -nostartfiles
        -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1
        -Wl,--major-os-version,5        -Wl,--minor-os-version,1)
endfunction()

# --- ntvdmex.exe: the shell preview (and, later, the VDM host itself) ----------
add_executable(ntvdmex WIN32
    src/main.c
    src/console.c
    src/runtime.c
    res/ntvdmex.rc)
target_include_directories(ntvdmex PRIVATE src)
if(NOT NTVDMEX_EMBED_MANIFEST)
    target_compile_definitions(ntvdmex PRIVATE NTVDMEX_NO_MANIFEST)
endif()
ntvdmex_configure_xp_target(ntvdmex)
target_link_libraries(ntvdmex PRIVATE kernel32 user32 gdi32 comctl32 gcc)

# --- wowprobe.exe: Spike-002 harness (WOW registry repoint) --------------------
# A throwaway stub that proves XP launches us as the VDM support process and
# captures the launch command line. Not part of the shipping product.
add_executable(wowprobe WIN32
    tools/wowprobe/wowprobe.c
    src/runtime.c)
ntvdmex_configure_xp_target(wowprobe)
target_link_libraries(wowprobe PRIVATE kernel32 user32 gcc)

# --- ntvdmhost.exe: the clean DOS VDM host -------------------------------------
# The proven DOS core, built from clean src/ modules (src/dos + src/vdm +
# src/host). This replaced the throwaway tools/vdmhost spike, which was retired
# once this host reached parity (MEMTEST PASS in V86); see git history for it.
# Same PE shape as the spike so XP accepts us as the VDM host: ImageBase
# 0x0F000000 (leaves the low ~240MB free for the V86/DOS address space the VDM
# maps), CUI/4.0 subsystem, ASLR/NX cleared. No CRT (src/runtime.c).
add_executable(ntvdmhost
    src/host/main.c
    src/vdm/v86.c
    src/vdm/dpmi.c
    src/vdm/dpmi_enter.S
    src/vdm/csrss.c
    src/dos/dos_int21.c
    src/vdd/vdd_bus.c
    src/vdd/vdd_pit.c
    src/vdd/vdd_pic.c
    src/vdd/vdd_dma.c
    src/vdd/vdd_opl.c
    src/vdd/vdd_opl_synth.c
    src/vdd/vdd_sb.c
    src/vdd/vdd_mpu.c
    src/vdd/vdd_audio.c
    src/vdd/audio_wave.c
    src/vdd/vdd_video.c
    src/vdd/vdd_input.c
    src/vdd/vdd_speaker.c
    src/vdd/present_ddraw.c
    src/runtime.c
    res/ntvdmhost.rc)
target_include_directories(ntvdmhost PRIVATE src/host src/vdm src/dos src/vdd)
if(NOT NTVDMEX_EMBED_MANIFEST)
    target_compile_definitions(ntvdmhost PRIVATE NTVDMEX_NO_MANIFEST)
endif()
ntvdmex_configure_xp_target(ntvdmhost)
# The host deliberately dereferences absolute V86 low-memory addresses (paragraph<<4
# off a NULL base), which -Warray-bounds flags as "outside array bounds". That's a
# false positive for direct hardware-address access -- silence it for this target.
target_compile_options(ntvdmhost PRIVATE -Wno-array-bounds)
target_link_options(ntvdmhost PRIVATE
    -Wl,--image-base,0x0f000000
    -Wl,--subsystem,console -Wl,--entry,_WinMainCRTStartup
    -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0
    -Wl,--disable-nxcompat -Wl,--disable-dynamicbase)
target_link_libraries(ntvdmhost PRIVATE kernel32 user32 gdi32 comctl32 gcc)

# --- present_demo.exe: standalone DirectDraw presentation exerciser (M3 slice-3)
# A GUI app that drives src/vdd/present_ddraw.c with an animated test frame, to
# eyeball the windowed + fullscreen blit path on real XP before it is wired into
# the host. ddraw is bound at runtime (GetProcAddress) so there is no ddraw import.
add_executable(present_demo WIN32
    tools/present_demo/present_demo.c
    src/vdd/present_ddraw.c
    src/runtime.c)
target_include_directories(present_demo PRIVATE src/vdd)
ntvdmex_configure_xp_target(present_demo)
target_link_libraries(present_demo PRIVATE kernel32 user32 gdi32 gcc)
