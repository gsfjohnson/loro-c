# M1 test suite. Each test is a standalone executable returning 0 on success.

add_executable(loro_test_doc test_doc.cpp)
target_link_libraries(loro_test_doc PRIVATE loro::loro)
add_test(NAME test_doc COMMAND loro_test_doc)

add_executable(loro_test_text test_text.cpp)
target_link_libraries(loro_test_text PRIVATE loro::loro)
add_test(NAME test_text COMMAND loro_test_text)

# Proves the C ABI is usable from plain C (compiled as C, not C++).
add_executable(loro_test_c_only test_c_only.c)
target_link_libraries(loro_test_c_only PRIVATE loro::loro)
add_test(NAME test_c_only COMMAND loro_test_c_only)

# In manual (non-Corrosion) mode, make every test wait for the cargo staticlib build.
if(TARGET loro_cargo_build)
    add_dependencies(loro_test_doc loro_cargo_build)
    add_dependencies(loro_test_text loro_cargo_build)
    add_dependencies(loro_test_c_only loro_cargo_build)
endif()
