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

# Phase 0: test_smoke (self-contained) + test_text (includes test_helpers.hpp).
# Phase 1 (typed value core): test_map + test_ephemeral_store exercise the typed LoroValue
# bridge through real loro-cpp tests.
# Phase 2 (containers + accessors): the four remaining container types plus the LoroDoc /
# VersionVector / Frontiers surface test_doc needs. All #include <loro.hpp> — resolved to the
# spike header.
# Phase 3 (subscriptions): test_subscriptions exercises the diff/event envelope + commit hooks
# (subscribe / subscribe_root / subscribe_local_update / subscribe_pre_commit /
# subscribe_first_commit_from_peer, ChangeModifier, get_change).
# Phase 4 (undo + commit hooks): test_undo exercises UndoManager (init / record_new_checkpoint /
# undo / redo / can_* / undo_count / set_max_undo_steps).
# Phase 5 (versions / frontiers / cursors / jsonpath / fractional index): test_version_vector
# (VersionVector + Frontiers algebra), test_fractional_index (FractionalIndex + tree positions),
# test_jsonpath (jsonpath / get_by_path / get_by_str_path).
# Phase 6 (awareness): test_awareness exercises the legacy per-peer presence type (Awareness /
# PeerInfo / AwarenessPeerUpdate) over the typed loro_awareness_*_value C ABI.
set(_loro_conformance_tests
    test_smoke test_text test_map test_ephemeral_store
    test_list test_movable_list test_tree test_counter test_doc
    test_subscriptions test_undo
    test_version_vector test_fractional_index test_jsonpath
    test_awareness)

foreach(_t IN LISTS _loro_conformance_tests)
    add_executable(conformance_${_t} "${LORO_CPP_DIR}/tests/${_t}.cpp")
    target_link_libraries(conformance_${_t} PRIVATE loro::loro)
    target_include_directories(conformance_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
        ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
    add_test(NAME conformance_${_t} COMMAND conformance_${_t})
    # Manual (non-Corrosion) mode: wait for the cargo staticlib, like the main test suite.
    if(TARGET loro_cargo_build)
        add_dependencies(conformance_${_t} loro_cargo_build)
    endif()
endforeach()

# Phase 1 fidelity test (loro-c-authored): covers the JSON-bridge blind spots the loro-cpp
# tests miss — kDouble{2.0}, kBinary — plus the loro::ext value helpers (the value-helper
# portion of loro-cpp's test_ext, which can't compile standalone until later phases).
add_executable(conformance_test_value_fidelity
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_value_fidelity.cpp")
target_link_libraries(conformance_test_value_fidelity PRIVATE loro::loro)
target_include_directories(conformance_test_value_fidelity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
    ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
add_test(NAME conformance_test_value_fidelity COMMAND conformance_test_value_fidelity)
if(TARGET loro_cargo_build)
    add_dependencies(conformance_test_value_fidelity loro_cargo_build)
endif()

# Phase 2 ext test (loro-c-authored): the container slice of loro-cpp's test_ext.cpp
# (test_container_id_helpers + test_insert_container_template). The undo slice of test_ext.cpp is
# gated in Phase 4, so the full file cannot compile standalone yet.
add_executable(conformance_test_ext_containers
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_containers.cpp")
target_link_libraries(conformance_test_ext_containers PRIVATE loro::loro)
target_include_directories(conformance_test_ext_containers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
    ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
add_test(NAME conformance_test_ext_containers COMMAND conformance_test_ext_containers)
if(TARGET loro_cargo_build)
    add_dependencies(conformance_test_ext_containers loro_cargo_build)
endif()

# Phase 3 ext test (loro-c-authored): the subscription slice of loro-cpp's test_ext.cpp
# (test_subscribe_lambdas) — exercises the loro::ext lambda adapters + subscribe_* shortcuts.
add_executable(conformance_test_ext_subscriptions
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_subscriptions.cpp")
target_link_libraries(conformance_test_ext_subscriptions PRIVATE loro::loro)
target_include_directories(conformance_test_ext_subscriptions PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
    ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
add_test(NAME conformance_test_ext_subscriptions COMMAND conformance_test_ext_subscriptions)
if(TARGET loro_cargo_build)
    add_dependencies(conformance_test_ext_subscriptions loro_cargo_build)
endif()

# Phase 4 ext test (loro-c-authored): the undo slice of loro-cpp's test_ext.cpp
# (test_undo_lambdas) — exercises the loro::ext on_undo_push/on_undo_pop adapters +
# set_on_push/set_on_pop shortcuts.
add_executable(conformance_test_ext_undo
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_undo.cpp")
target_link_libraries(conformance_test_ext_undo PRIVATE loro::loro)
target_include_directories(conformance_test_ext_undo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
    ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
add_test(NAME conformance_test_ext_undo COMMAND conformance_test_ext_undo)
if(TARGET loro_cargo_build)
    add_dependencies(conformance_test_ext_undo loro_cargo_build)
endif()

# Phase 5 cursor test (loro-c-authored): loro-cpp ships no test_cursor.cpp, so this covers the
# fleshed-out Cursor surface — get_cursor, encode/decode, get_cursor_pos (current + update via
# the new loro_doc_get_cursor_pos_full), and Cursor::init (via the new loro_cursor_new).
add_executable(conformance_test_cursor
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cursor.cpp")
target_link_libraries(conformance_test_cursor PRIVATE loro::loro)
target_include_directories(conformance_test_cursor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include    # the spike's <loro.hpp> / <loro/loro_ext.hpp>
    ${LORO_CPP_DIR}/tests)                 # test_helpers.hpp
add_test(NAME conformance_test_cursor COMMAND conformance_test_cursor)
if(TARGET loro_cargo_build)
    add_dependencies(conformance_test_cursor loro_cargo_build)
endif()
