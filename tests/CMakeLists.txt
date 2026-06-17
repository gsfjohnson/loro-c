# loro-c test suite — the loro-cpp-shaped permanent suite (RESHAPE Phase 7).
#
# Each test is a standalone executable returning 0 on success (1 = assertion failure,
# 2 = exception), registered with CTest. The bulk are loro-cpp's own tests/*.cpp, adopted
# here verbatim so loro-c's public <loro.hpp> is held to loro-cpp's behaviour. They include
# "test_helpers.hpp" (co-located) and <loro.hpp> / <loro/loro_ext.hpp> (from loro::loro).
#
# test_value_fidelity and test_cursor are loro-c-authored: they cover blind spots loro-cpp's
# own tests miss (kDouble{2.0} + kBinary typed-value fidelity; the fleshed-out Cursor surface).

set(_loro_tests
    # Adopted from loro-cpp/tests (one per UDL interface).
    test_smoke test_doc test_text test_map test_list test_movable_list test_tree test_counter
    test_awareness test_ephemeral_store test_undo test_fractional_index test_version_vector
    test_jsonpath test_subscriptions test_ext
    # loro-c-authored, covering loro-cpp's blind spots.
    test_value_fidelity test_cursor test_text_utf8)

foreach(_t IN LISTS _loro_tests)
    add_executable(loro_${_t} ${_t}.cpp)
    target_link_libraries(loro_${_t} PRIVATE loro::loro)
    # "test_helpers.hpp" lives alongside the tests; make the include path explicit.
    target_include_directories(loro_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${_t} COMMAND loro_${_t})
    # In manual (non-Corrosion) mode, wait for the cargo staticlib build.
    if(TARGET loro_cargo_build)
        add_dependencies(loro_${_t} loro_cargo_build)
    endif()
endforeach()

# Proves the C ABI is usable from plain C (compiled as C, not C++).
add_executable(loro_test_c_only test_c_only.c)
target_link_libraries(loro_test_c_only PRIVATE loro::loro)
add_test(NAME test_c_only COMMAND loro_test_c_only)
if(TARGET loro_cargo_build)
    add_dependencies(loro_test_c_only loro_cargo_build)
endif()
