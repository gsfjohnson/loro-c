# Test suite. Each test is a standalone executable returning 0 on success.

add_executable(loro_test_doc test_doc.cpp)
target_link_libraries(loro_test_doc PRIVATE loro::loro)
add_test(NAME test_doc COMMAND loro_test_doc)

add_executable(loro_test_text test_text.cpp)
target_link_libraries(loro_test_text PRIVATE loro::loro)
add_test(NAME test_text COMMAND loro_test_text)

# M2 containers.
add_executable(loro_test_map_list test_map_list.cpp)
target_link_libraries(loro_test_map_list PRIVATE loro::loro)
add_test(NAME test_map_list COMMAND loro_test_map_list)

add_executable(loro_test_movable_list test_movable_list.cpp)
target_link_libraries(loro_test_movable_list PRIVATE loro::loro)
add_test(NAME test_movable_list COMMAND loro_test_movable_list)

add_executable(loro_test_counter test_counter.cpp)
target_link_libraries(loro_test_counter PRIVATE loro::loro)
add_test(NAME test_counter COMMAND loro_test_counter)

add_executable(loro_test_tree test_tree.cpp)
target_link_libraries(loro_test_tree PRIVATE loro::loro)
add_test(NAME test_tree COMMAND loro_test_tree)

# Proves the C ABI is usable from plain C (compiled as C, not C++).
add_executable(loro_test_c_only test_c_only.c)
target_link_libraries(loro_test_c_only PRIVATE loro::loro)
add_test(NAME test_c_only COMMAND loro_test_c_only)

# In manual (non-Corrosion) mode, make every test wait for the cargo staticlib build.
if(TARGET loro_cargo_build)
    add_dependencies(loro_test_doc loro_cargo_build)
    add_dependencies(loro_test_text loro_cargo_build)
    add_dependencies(loro_test_map_list loro_cargo_build)
    add_dependencies(loro_test_movable_list loro_cargo_build)
    add_dependencies(loro_test_counter loro_cargo_build)
    add_dependencies(loro_test_tree loro_cargo_build)
    add_dependencies(loro_test_c_only loro_cargo_build)
endif()
