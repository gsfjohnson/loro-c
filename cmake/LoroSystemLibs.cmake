# LoroSystemLibs.cmake — the native system libraries the Rust static archive pulls in.
#
# Single source of truth for BOTH build paths and BOTH consumers of the list:
#   * cmake/BuildRustStaticlib.cmake attaches it to the build-tree target, and
#   * CMakeLists.txt bakes it into loroConfig.cmake (@LORO_SYSTEM_LIBS@) and
#     loro.pc (@LORO_PC_PRIVATE_LIBS@),
# so the build tree and the install tree cannot drift.
#
# Source of truth for each list:
#   cargo rustc --release --lib -- --print native-static-libs
# for the corresponding target.
include_guard(GLOBAL)

# loro_system_libs(<out-libs-var> <out-pc-var>)
#
# Classifies the *Rust build target*, not the CMake host platform, because host detection
# is unreliable here: on the MSYS2/CLANG64 dev box the only usable CMake is the POSIX
# base-repo build, where WIN32 is FALSE even though cargo targets Windows; and an iOS
# cross build runs on a macOS host. The key is, in order of preference:
#   1. the explicit cargo target triple — LORO_CARGO_TARGET (manual path) or
#      Rust_CARGO_TARGET (Corrosion pass-through, e.g. the iOS cross jobs);
#   2. the pinned rustup toolchain name (LORO_RUST_TOOLCHAIN, manual path): its name
#      embeds the host triple, which is what cargo builds when no --target is given
#      (e.g. stable-x86_64-pc-windows-gnullvm on the dev box);
#   3. the host platform (WIN32/APPLE/else) — only reached when a native CMake drives
#      cargo's default host target, where host detection IS reliable.
function(loro_system_libs out_libs out_pc)
    set(_key "${LORO_CARGO_TARGET}")
    if(NOT _key)
        set(_key "${Rust_CARGO_TARGET}")
    endif()
    if(NOT _key)
        set(_key "${LORO_RUST_TOOLCHAIN}")
    endif()

    if(_key MATCHES "windows-gnullvm")
        # gnullvm: DWARF unwinding via LLVM libunwind (the crate builds panic=unwind).
        set(_libs bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp unwind)
    elseif(_key MATCHES "windows")
        # msvc-style Windows: SEH unwinding, no libunwind. (windows-gnu/mingw-gcc is not
        # a supported target; it would additionally need gcc_eh/pthread.)
        set(_libs bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp)
    elseif(_key MATCHES "apple")
        # macOS and iOS (device + simulator): rustc reports no native-static-libs beyond
        # the libSystem/libc++abi machinery every Apple link carries implicitly, and the
        # crate graph links no Apple frameworks (no security-framework/core-foundation).
        set(_libs "")
    elseif(_key)
        # Any other explicit triple: assume the Linux-ish glibc list.
        set(_libs m dl pthread gcc_s util rt)
    elseif(WIN32)
        # No triple/toolchain hint: native CMake on Windows drives the msvc host target.
        set(_libs bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp)
    elseif(APPLE)
        set(_libs "")
    else()
        set(_libs m dl pthread gcc_s util rt)
    endif()

    # pkg-config Libs.private form ("-lfoo -lbar"); empty list -> empty string.
    set(_pc "")
    if(_libs)
        list(TRANSFORM _libs PREPEND "-l" OUTPUT_VARIABLE _pc)
        list(JOIN _pc " " _pc)
    endif()
    set(${out_libs} "${_libs}" PARENT_SCOPE)
    set(${out_pc} "${_pc}" PARENT_SCOPE)
endfunction()
