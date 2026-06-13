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
# loro_c_api is the Corrosion-imported staticlib target (from [lib] name in Cargo.toml).
target_link_libraries(loro INTERFACE loro_c_api)

if(LORO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
