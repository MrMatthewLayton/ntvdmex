cmake_minimum_required(VERSION 3.16)

project(ntvdmex
    VERSION 0.1.0
    DESCRIPTION "New Technology Virtual DOS Manager, Extended"
    LANGUAGES C)

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
