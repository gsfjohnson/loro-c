# Manual cargo -> CMake integration (fallback for when LORO_USE_CORROSION is OFF).
#
# Why this exists: Corrosion drives a *native* cargo.exe but passes it absolute paths
# derived from CMake's path variables. On an MSYS2 host the only available CMake is the
# POSIX build, whose path variables look like `/c/Users/...`; native cargo cannot consume
# those, and the round-trip through `cargo metadata` mangles the drive letter. This module
# sidesteps the problem by invoking cargo with WORKING_DIRECTORY set to the crate directory
# and only relative arguments — no absolute path is ever handed to cargo.
#
# It produces an INTERFACE target `loro_c_api` (same name Corrosion would create) that
# links the built staticlib plus the native system libraries the Rust std needs.

if(NOT DEFINED LORO_CARGO)
    find_program(LORO_CARGO cargo REQUIRED)
endif()

set(LORO_CARGO_TARGET "" CACHE STRING
    "Rust target triple to build (empty = cargo's default host target)")
set(LORO_CARGO_PROFILE "release" CACHE STRING "Cargo profile (release|dev)")

# On a box whose default rustup toolchain cannot link (e.g. MSYS2/CLANG64, where the msvc
# default has no link.exe), set this to the toolchain that can — e.g.
#   -DLORO_RUST_TOOLCHAIN=stable-x86_64-pc-windows-gnullvm
# It is applied via the RUSTUP_TOOLCHAIN env var so that BOTH the cargo shim and the rustc
# it spawns resolve to the same toolchain. Invoking a toolchain-specific cargo.exe directly
# does NOT work: that cargo still finds rustc through the PATH shim, which falls back to the
# (wrong) default toolchain and triggers an auto-install / version-mismatch (E0514).
set(LORO_RUST_TOOLCHAIN "" CACHE STRING
    "rustup toolchain to build with (empty = rustup default); applied via RUSTUP_TOOLCHAIN")

set(_crate_dir "${CMAKE_CURRENT_SOURCE_DIR}/loro-c-api")

# Map the cargo profile name to its target/ subdirectory.
if(LORO_CARGO_PROFILE STREQUAL "dev")
    set(_profile_args "")
    set(_profile_subdir "debug")
else()
    set(_profile_args "--release")
    set(_profile_subdir "release")
endif()

# Optional --target and the matching target/ subdirectory.
set(_target_args "")
set(_target_subdir "")
if(LORO_CARGO_TARGET)
    set(_target_args "--target" "${LORO_CARGO_TARGET}")
    set(_target_subdir "${LORO_CARGO_TARGET}/")
endif()

# cargo writes into <crate>/target/ by default; that path is only ever used CMake-side
# (CMake understands POSIX paths), so no conversion is needed here.
set(_staticlib
    "${_crate_dir}/target/${_target_subdir}${_profile_subdir}/libloro_c_api.a")

# Prefix the invocation with `cmake -E env RUSTUP_TOOLCHAIN=...` when a toolchain is pinned,
# so the shim cargo and its rustc agree. Without a pin, run cargo directly.
if(LORO_RUST_TOOLCHAIN)
    set(_cargo_cmd "${CMAKE_COMMAND}" -E env
        "RUSTUP_TOOLCHAIN=${LORO_RUST_TOOLCHAIN}" "${LORO_CARGO}")
else()
    set(_cargo_cmd "${LORO_CARGO}")
endif()

add_custom_command(
    OUTPUT "${_staticlib}"
    COMMAND ${_cargo_cmd} build ${_profile_args} ${_target_args}
    WORKING_DIRECTORY "${_crate_dir}"
    COMMENT "Building loro-c-api Rust staticlib via cargo (${LORO_CARGO_PROFILE})"
    VERBATIM
    USES_TERMINAL
)
# ALL so a plain `cmake --build` builds the staticlib; the link-time dependency below
# guarantees ordering relative to consumers.
add_custom_target(loro_cargo_build ALL DEPENDS "${_staticlib}")

add_library(loro_c_api INTERFACE)
add_dependencies(loro_c_api loro_cargo_build)
# Linking the generated file by full path makes consumers depend on its custom command.
# The trailing system libs are exactly cargo's reported `native-static-libs` for the
# gnullvm/gnu Windows target (query: `cargo rustc --release --lib -- --print
# native-static-libs`). bcrypt+advapi32 back getrandom's BCryptGenRandom path.
target_link_libraries(loro_c_api INTERFACE
    "${_staticlib}"
    bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp unwind
)
