cmake_minimum_required(VERSION 3.22)
project(loro-c LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTING "Build the loro-c test suite" ON)
option(LORO_BUILD_EXAMPLES "Build the loro-c examples" OFF)
option(LORO_USE_CORROSION
    "Build the Rust crate with Corrosion (recommended; used by the CI matrix). Disable on \
toolchains where the only available CMake is a POSIX build driving a native-Windows cargo \
(e.g. MSYS2/CLANG64), where Corrosion's absolute-path handling breaks — see \
cmake/BuildRustStaticlib.cmake." ON)

# ---------------------------------------------------------------------------
# Build the Rust loro-c-api crate and expose it as the CMake target `loro_c_api`.
# Default path: Corrosion (fetched, not vendored). Fallback: a thin manual cargo wrapper.
# ---------------------------------------------------------------------------
if(LORO_USE_CORROSION)
    include(FetchContent)
    # v0.6.1: first Corrosion line that recognizes the `*-windows-gnullvm` environment
    # (treated as GNU-style for archive/import-library naming). v0.5.1 aborts with
    # "Unknown windows environment" on gnullvm.
    FetchContent_Declare(
        Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.6.1
    )
    FetchContent_MakeAvailable(Corrosion)

    # On hosts whose default Rust target cannot link (e.g. an MSYS2/CLANG64 box where the
    # msvc target has no link.exe), select a working toolchain/target at configure time:
    #   -DRust_TOOLCHAIN=stable-x86_64-pc-windows-gnullvm
    #   -DRust_CARGO_TARGET=x86_64-pc-windows-gnullvm
    # Both are honoured by Corrosion; neither is hard-coded here so the build stays portable.
    corrosion_import_crate(
        MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/loro-c-api/Cargo.toml
        CRATE_TYPES staticlib
        PROFILE release
    )
else()
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRustStaticlib.cmake)
endif()

# ---------------------------------------------------------------------------
# Header (re)generation — explicit, opt-in, NEVER part of a normal build.
#
# include/loro/loro.h is committed and is the source of truth. Regenerate it with the
# pinned cbindgen CLI only when the Rust API changes:
#     cmake --build build --target regenerate-header
# (Cargo build scripts must not mutate the source tree; regenerating on every build would
# dirty the working tree and make CI non-deterministic.)
# ---------------------------------------------------------------------------
find_program(CBINDGEN_EXECUTABLE cbindgen)
if(CBINDGEN_EXECUTABLE)
    add_custom_target(regenerate-header
        COMMAND ${CBINDGEN_EXECUTABLE}
                --config ${CMAKE_CURRENT_SOURCE_DIR}/loro-c-api/cbindgen.toml
                --crate loro-c-api
                --output ${CMAKE_CURRENT_SOURCE_DIR}/include/loro/loro.h
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/loro-c-api
        COMMENT "Regenerating include/loro/loro.h via cbindgen CLI"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# Public consumable target: loro::loro
# ---------------------------------------------------------------------------
add_library(loro INTERFACE)
add_library(loro::loro ALIAS loro)
target_compile_features(loro INTERFACE cxx_std_20)
target_include_directories(loro INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# loro_c_api is the Corrosion-imported (or manual-fallback) staticlib target (from [lib]
# name in Cargo.toml). Build tree only: in the *install* tree the link to the Rust archive
# is wired by loroConfig.cmake (which recreates `loro_c_api-static`), because naming an
# imported target inside $<INSTALL_INTERFACE> would make install(EXPORT) reject `loro` for
# referencing a target outside its export set. See the install section below.
target_link_libraries(loro INTERFACE $<BUILD_INTERFACE:loro_c_api>)

# ---------------------------------------------------------------------------
# Install / export — makes the library consumable via find_package(loro) and pkg-config.
#
# M5 is static-install-first: the static archive plus the header-only C++ layer are
# installed and a relocatable loroConfig.cmake is generated. The cdylib shared library and
# SOVERSION versioning are deferred (Corrosion-only, Linux/macOS-only, CI-only-verifiable).
#
# The install tree (under CMAKE_INSTALL_PREFIX) looks like:
#   include/loro/{loro.h,loro.hpp}
#   lib/libloro_c_api.a
#   lib/cmake/loro/{loroConfig.cmake,loroConfigVersion.cmake,loro-targets.cmake,<rust-snippet>.cmake}
#   lib/pkgconfig/loro.pc
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(LORO_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/loro)

# System libraries the static Rust archive pulls in. Resolved HERE (at our configure time)
# rather than in the consumer's loroConfig.cmake, because the install is platform-specific
# anyway and consumer-side WIN32/APPLE detection is unreliable on the MSYS2/CLANG64 box
# (CMake's WIN32 is unset there even though the gnullvm target is Windows). Source of truth:
# `cargo rustc --release --lib -- --print native-static-libs`. Both forms are baked into the
# generated package: LORO_SYSTEM_LIBS into loroConfig.cmake, LORO_PC_PRIVATE_LIBS into loro.pc.
if(NOT LORO_USE_CORROSION)
    # MSYS2/CLANG64 gnullvm manual path — matches BuildRustStaticlib.cmake's build-tree list,
    # including libunwind (the crate is built with panic=unwind).
    set(_loro_sys bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp unwind)
    set(LORO_PC_PRIVATE_LIBS "-lbcrypt -ladvapi32 -lkernel32 -lntdll -luserenv -lws2_32 -ldbghelp -lunwind")
elseif(WIN32)
    # MSVC (CI): SEH unwinding, no libunwind.
    set(_loro_sys bcrypt advapi32 kernel32 ntdll userenv ws2_32 dbghelp)
    set(LORO_PC_PRIVATE_LIBS "-lbcrypt -ladvapi32 -lkernel32 -lntdll -luserenv -lws2_32 -ldbghelp")
elseif(APPLE)
    set(_loro_sys "")
    set(LORO_PC_PRIVATE_LIBS "")
else()
    set(_loro_sys m dl pthread gcc_s util rt)
    set(LORO_PC_PRIVATE_LIBS "-lm -ldl -lpthread -lgcc_s -lutil -lrt")
endif()
# Space-separated form for the @LORO_SYSTEM_LIBS@ substitution in loroConfig.cmake.in.
string(JOIN " " LORO_SYSTEM_LIBS ${_loro_sys})

# The build tree links loro -> $<BUILD_INTERFACE:loro_c_api> (line ~84). On the Corrosion
# path the imported archive target does NOT carry these native-static-libs, so attach them
# here or downstream examples/tests fail to link (MSVC: unresolved __imp_BCryptGenRandom
# from getrandom). $<BUILD_INTERFACE:> keeps them out of install(EXPORT) — the install tree
# gets them via loroConfig.cmake (@LORO_SYSTEM_LIBS@). The manual (non-Corrosion) path
# already attaches the same libs in cmake/BuildRustStaticlib.cmake.
if(LORO_USE_CORROSION AND _loro_sys)
    target_link_libraries(loro INTERFACE "$<BUILD_INTERFACE:${_loro_sys}>")
endif()

unset(_loro_sys)

# (A) Install the Rust staticlib + the snippet that recreates it as `loro_c_api-static`.
if(LORO_USE_CORROSION)
    # corrosion_install copies the archive to lib/ (cross-platform naming) and emits
    # ${CMAKE_BINARY_DIR}/corrosion/loro-corrosionCorrosion.cmake, which recreates the
    # archive as a relocatable STATIC IMPORTED target. (Corrosion's own install(EXPORT) is
    # unimplemented in v0.6.1, so we install that generated snippet ourselves.)
    corrosion_install(TARGETS loro_c_api EXPORT loro-corrosion
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES ${CMAKE_BINARY_DIR}/corrosion/loro-corrosionCorrosion.cmake
            DESTINATION ${LORO_INSTALL_CMAKEDIR})
else()
    # Manual path: `_staticlib` is set by BuildRustStaticlib.cmake (included above, same
    # scope). Install the prebuilt archive and a hand-written equivalent snippet.
    install(FILES "${_staticlib}"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RENAME libloro_c_api.a)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loro-manual-targets.cmake
            DESTINATION ${LORO_INSTALL_CMAKEDIR})
endif()

# (B) Export the public INTERFACE target (header-only; no artifact) + the headers.
install(TARGETS loro EXPORT loro-targets)
install(EXPORT loro-targets
        NAMESPACE loro::
        FILE loro-targets.cmake
        DESTINATION ${LORO_INSTALL_CMAKEDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/loro
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")

# (C) Generate + install the package config and its version file.
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/loroConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/loroConfig.cmake
    INSTALL_DESTINATION ${LORO_INSTALL_CMAKEDIR})
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/loroConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/loroConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/loroConfigVersion.cmake
    DESTINATION ${LORO_INSTALL_CMAKEDIR})

# (D) pkg-config. @ONLY so ${prefix}/${libdir} survive into the installed .pc for
# pkg-config (not CMake) to expand at consume time.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/loro.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/loro.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/loro.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(LORO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
