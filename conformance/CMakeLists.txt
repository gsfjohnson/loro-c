# Phase 0 conformance spike: compile (and run) loro-cpp's own tests against loro-c's new,
# loro-cpp-shaped <loro.hpp> at conformance/include/loro.hpp. Green here proves the API shape
# matches loro-cpp and that the build wiring works, before the full RESHAPE rewrite.
#
# Guarded: active only when LORO_BUILD_CONFORMANCE=ON *and* a loro-cpp checkout is present at
# LORO_CPP_DIR. Without loro-cpp this is a no-op, so normal CI (which has no loro-cpp) is
# unaffected.

set(LORO_CPP_DIR "${CMAKE_SOURCE_DIR}/../loro-cpp" CACHE PATH
    "Path to a loro-cpp checkout providing the conformance tests (tests/test_*.cpp).")

if(NOT EXISTS "${LORO_CPP_DIR}/tests/test_smoke.cpp")
    message(WARNING
        "LORO_BUILD_CONFORMANCE=ON but no loro-cpp tests were found at "
        "'${LORO_CPP_DIR}/tests'. Skipping the conformance spike. "
        "Set -DLORO_CPP_DIR=<path-to-loro-cpp> to enable it.")
    return()
endif()

message(STATUS "loro-c conformance spike: building loro-cpp tests from ${LORO_CPP_DIR}/tests")

# test_smoke is self-contained; test_text additionally includes test_helpers.hpp from
# loro-cpp's tests dir. Both #include <loro.hpp> — resolved to the spike header below.
set(_loro_conformance_tests test_smoke test_text)

foreach(_t IN LISTS _loro_conformance_tests)
    add_executable(conformance_${_t} "${LORO_CPP_DIR}/tests/${_t}.cpp")
    target_link_libraries(conformance_${_t} PRIVATE loro::loro)
    target_include_directories(conformance_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp>
        ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
    add_test(NAME conformance_${_t} COMMAND conformance_${_t})
    # Manual (non-Corrosion) mode: wait for the cargo staticlib, like the main test suite.
    if(TARGET loro_cargo_build)
        add_dependencies(conformance_${_t} loro_cargo_build)
    endif()
endforeach()
