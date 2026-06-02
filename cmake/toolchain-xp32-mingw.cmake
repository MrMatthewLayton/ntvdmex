#
# Cross-compile toolchain: 32-bit Windows XP SP3 target, built from any host
# (developed on macOS) using the mingw-w64 i686 toolchain.
#
# Why this toolchain (see docs/research/build-toolchain.md):
#   - V86 only exists in 32-bit mode, so NTVDMEX is permanently i686/x86 — never x64.
#   - mingw-w64 cross-compiles a standalone PE32 from macOS/Linux, giving a fast
#     dev loop without needing a Windows box just to produce a binary.
#   - The shipping artifact must load on stock XP SP3: we pin the PE subsystem and
#     OS version fields to 5.01 and statically link libgcc so there is no runtime
#     DLL dependency the target machine might lack.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-xp32-mingw.cmake
#   cmake --build build
#

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

# Allow overriding the prefix (e.g. if installed under a different name).
if(NOT DEFINED TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX i686-w64-mingw32)
endif()

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Search for libraries/headers only in the target sysroot, but run host programs.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
