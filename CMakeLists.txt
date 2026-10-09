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

# --- ntvdmex.exe: the NTVDMEX MANAGER (GH #281, s88) ---------------------------
# One tray icon for every running DOS and Win16 program; each program still runs in
# its own ntvdmhost.exe (NT allows one V86 machine per process). Started on demand by
# the first host, exits when the last one has gone. See src/manager/manager.c.
# (This target used to build the milestone-0 shell preview, src/main.c + console.c,
# which was never shipped and is retired.)
add_executable(ntvdmex WIN32
    src/manager/manager.c
    src/runtime.c
    res/ntvdmex.rc)
target_include_directories(ntvdmex PRIVATE src)
set_source_files_properties(res/ntvdmex.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/MAINICON.ico")
if(NOT NTVDMEX_EMBED_MANIFEST)
    target_compile_definitions(ntvdmex PRIVATE NTVDMEX_NO_MANIFEST)
endif()
ntvdmex_configure_xp_target(ntvdmex)
target_link_libraries(ntvdmex PRIVATE kernel32 user32 shell32 gcc)

# --- wowprobe.exe: Spike-002 harness (WOW registry repoint) --------------------
# A throwaway stub that proves XP launches us as the VDM support process and
# captures the launch command line. Not part of the shipping product.
# Local-only since #322 (not in the public repo): built when its source is present.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/wowprobe/wowprobe.c")
add_executable(wowprobe WIN32
    tools/wowprobe/wowprobe.c
    src/runtime.c)
ntvdmex_configure_xp_target(wowprobe)
target_link_libraries(wowprobe PRIVATE kernel32 user32 gcc)
endif()

# --- ntvdmhost.exe: the clean DOS VDM host -------------------------------------
# The proven DOS core, built from clean src/ modules (src/dos + src/vdm +
# src/host). This replaced the throwaway tools/vdmhost spike, which was retired
# once this host reached parity (MEMTEST PASS in V86); see git history for it.
# Same PE shape as the spike so XP accepts us as the VDM host: ImageBase
# 0x0F000000 (leaves the low ~240MB free for the V86/DOS address space the VDM
# maps), CUI/4.0 subsystem, ASLR/NX cleared. No CRT (src/runtime.c).
add_executable(ntvdmhost
    src/host/main.c
    src/host/log.c
    src/host/host_state.c
    src/host/host_core.c
    src/host/host_irq.c
    src/host/host_io.c
    src/host/host_input.c
    src/host/host_audio.c
    src/host/host_install.c
    src/host/install.c
    src/host/dpmi_svc.c
    src/host/cpuspeed.c
    src/host/pcspeaker.c
    src/host/settings.c
    src/host/sysfont.c
    src/host/x86len.c
    src/host/dpmi_rmcs.c
    src/host/i33_driver.c
    src/host/pif.c
    src/vdd/present_scale.c
    src/vdm/v86.c
    src/vdm/dpmi.c
    src/vdm/dpmi_enter.S
    src/vdm/csrss.c
    src/dos/dos_int21.c
    src/dos/dos_recovery.c
    src/dos/dos_disk.c
    src/dos/dos_err.c
    src/wow/ne.c
    src/wow/wowsched.c
    src/vdd/vdd_bus.c
    src/vdd/vdd_pit.c
    src/vdd/vdd_cmos.c
    src/vdd/vdd_fdc.c
    src/vdd/vdd_ide.c
    src/vdd/vdd_pic.c
    src/vdd/vdd_dma.c
    src/vdd/vdd_opl.c
    src/vdd/vdd_opl_synth.c
    src/vdd/vdd_sb.c
    src/vdd/vdd_gus.c
    src/vdd/vdd_emu8k.c
    src/vdd/vdd_mpu.c
    src/vdd/vdd_comm.c
    src/vdd/vdd_net.c
    src/vdd/vdd_audio.c
    src/vdd/audio_wave.c
    src/vdd/vdd_video.c
    src/vdd/vdd_input.c
    src/vdd/vdd_speaker.c
    src/vdd/vdd_joy.c
    src/vdd/present_ddraw.c
    src/runtime.c
    res/ntvdmhost.rc)
# The .rc names MAINICON.ico but CMake does not scan .rc includes, so a new icon alone
# rebuilt nothing and the old one shipped (s73). Make the dependency explicit.
set_source_files_properties(res/ntvdmhost.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/MAINICON.ico")
target_include_directories(ntvdmhost PRIVATE src/host src/vdm src/dos src/vdd src/wow)
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
target_link_libraries(ntvdmhost PRIVATE kernel32 user32 gdi32 comctl32 comdlg32 advapi32 shell32 gcc)

# --- bin\wowshim\WOW32.DLL + NTVDM.EXE: the host's stand-ins for the two modules a
# 32-bit thunk DLL looks up by NAME (winmm's WOW layer does; GH #5/#278, s90). One
# source, built twice; the .EXE is a DLL image -- only its module name matters.
# Undecorated stdcall exports (--kill-at): callers use GetProcAddress by plain name.
foreach(shim WOW32 NTVDM)
    add_library(shim_${shim} SHARED src/shim/wowshim.c)
    if(shim STREQUAL "NTVDM")
        set_target_properties(shim_${shim} PROPERTIES OUTPUT_NAME NTVDM PREFIX "" SUFFIX ".EXE")
    else()
        set_target_properties(shim_${shim} PROPERTIES OUTPUT_NAME WOW32 PREFIX "" SUFFIX ".DLL")
    endif()
    set_target_properties(shim_${shim} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wowshim"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wowshim")
    target_link_options(shim_${shim} PRIVATE -nostdlib -Wl,--kill-at
        -Wl,--entry,_DllMainCRTStartup@12 -Wl,--enable-stdcall-fixup)
    target_link_libraries(shim_${shim} PRIVATE kernel32)
endforeach()

# --- present_demo.exe: standalone DirectDraw presentation exerciser (M3 slice-3)
# A GUI app that drives src/vdd/present_ddraw.c with an animated test frame, to
# eyeball the windowed + fullscreen blit path on real XP before it is wired into
# the host. ddraw is bound at runtime (GetProcAddress) so there is no ddraw import.
# Local-only since #322 (not in the public repo): built when its source is present.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/present_demo/present_demo.c")
add_executable(present_demo WIN32
    tools/present_demo/present_demo.c
    src/vdd/present_ddraw.c
    src/vdd/present_scale.c
    src/runtime.c)
target_include_directories(present_demo PRIVATE src/vdd)
ntvdmex_configure_xp_target(present_demo)
target_link_libraries(present_demo PRIVATE kernel32 user32 gdi32 gcc)
endif()

# --- DOS probes: tests/probes/dos/*.asm -> build/probes/*.com (nasm) -------------
# The guest-side probes are built, not committed (#322). selftest.com ships in the
# release zip; the others are run on the rig and the oracles. dpmiback.asm is MASM
# syntax and is not assembled here.
find_program(NASM_EXE nasm)
if(NASM_EXE)
    set(PROBE_SRC "${CMAKE_SOURCE_DIR}/tests/probes/dos")
    set(PROBE_OUT "${CMAKE_BINARY_DIR}/probes")
    file(MAKE_DIRECTORY "${PROBE_OUT}")
    file(GLOB PROBE_ASM CONFIGURE_DEPENDS "${PROBE_SRC}/*.asm")
    list(REMOVE_ITEM PROBE_ASM "${PROBE_SRC}/dpmiback.asm")
    file(GLOB PROBE_INC CONFIGURE_DEPENDS "${PROBE_SRC}/*.inc")
    set(PROBE_OUTPUTS)
    foreach(asm IN LISTS PROBE_ASM)
        get_filename_component(n "${asm}" NAME_WE)
        set(deps "${asm}" ${PROBE_INC})
        if(n STREQUAL "p_tsr")                 # incbin "p_tsrc.com"
            list(APPEND deps "${PROBE_OUT}/p_tsrc.com")
        endif()
        add_custom_command(OUTPUT "${PROBE_OUT}/${n}.com"
            COMMAND "${NASM_EXE}" -f bin -I "${PROBE_SRC}/" -I "${PROBE_OUT}/"
                    "${asm}" -o "${PROBE_OUT}/${n}.com"
            DEPENDS ${deps} VERBATIM)
        list(APPEND PROBE_OUTPUTS "${PROBE_OUT}/${n}.com")
    endforeach()
    file(STRINGS "${PROBE_SRC}/variants.txt" PROBE_VARIANTS REGEX "^[^#]")
    foreach(line IN LISTS PROBE_VARIANTS)
        separate_arguments(parts UNIX_COMMAND "${line}")
        list(GET parts 0 out)
        list(GET parts 1 src)
        list(SUBLIST parts 2 -1 flags)
        add_custom_command(OUTPUT "${PROBE_OUT}/${out}"
            COMMAND "${NASM_EXE}" -f bin ${flags} -I "${PROBE_SRC}/"
                    "${PROBE_SRC}/${src}" -o "${PROBE_OUT}/${out}"
            DEPENDS "${PROBE_SRC}/${src}" ${PROBE_INC} VERBATIM)
        list(APPEND PROBE_OUTPUTS "${PROBE_OUT}/${out}")
    endforeach()
    # dosdiff.py reads a probe's .deps/.pre sidecars from BESIDE the .com, so they
    # travel with the binaries and build/probes/ is a self-contained probe folder.
    file(GLOB PROBE_SIDECARS CONFIGURE_DEPENDS "${PROBE_SRC}/*.deps" "${PROBE_SRC}/*.pre")
    foreach(sc IN LISTS PROBE_SIDECARS)
        get_filename_component(scn "${sc}" NAME)
        configure_file("${sc}" "${PROBE_OUT}/${scn}" COPYONLY)
    endforeach()
    add_custom_target(probes ALL DEPENDS ${PROBE_OUTPUTS})
else()
    message(WARNING "nasm not found: the DOS probes (and the release zip's selftest.com) are not built")
endif()
