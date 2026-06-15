# Test suite. Each test is a standalone executable returning 0 on success.

add_executable(loro_test_doc test_doc.cpp)
target_link_libraries(loro_test_doc PRIVATE loro::loro)
add_test(NAME test_doc COMMAND loro_test_doc)

add_executable(loro_test_text test_text.cpp)
target_link_libraries(loro_test_text PRIVATE loro::loro)
add_test(NAME test_text COMMAND loro_test_text)

# G1 rich text: marks, deltas, expand semantics, UTF-16, convert_pos.
add_executable(loro_test_richtext test_richtext.cpp)
target_link_libraries(loro_test_richtext PRIVATE loro::loro)
add_test(NAME test_richtext COMMAND loro_test_richtext)

# G2 cursors: stable positions, side, encode/decode, concurrent-insert shift.
add_executable(loro_test_cursor test_cursor.cpp)
target_link_libraries(loro_test_cursor PRIVATE loro::loro)
add_test(NAME test_cursor COMMAND loro_test_cursor)

# G3 sync surface: JSON-update interchange, export modes, import_batch/with, shallow snapshots.
add_executable(loro_test_sync test_sync.cpp)
target_link_libraries(loro_test_sync PRIVATE loro::loro)
add_test(NAME test_sync COMMAND loro_test_sync)

# G4 diff/patch: diff between versions, apply a diff across docs, revert_to, nested-container diff.
add_executable(loro_test_diff test_diff.cpp)
target_link_libraries(loro_test_diff PRIVATE loro::loro)
add_test(NAME test_diff COMMAND loro_test_diff)

# G5 value navigation: get_by_path / get_by_str_path, live-container handle vs. plain value.
add_executable(loro_test_navigate test_navigate.cpp)
target_link_libraries(loro_test_navigate PRIVATE loro::loro)
add_test(NAME test_navigate COMMAND loro_test_navigate)

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

# M3 events/subscriptions.
add_executable(loro_test_subscriptions test_subscriptions.cpp)
target_link_libraries(loro_test_subscriptions PRIVATE loro::loro)
add_test(NAME test_subscriptions COMMAND loro_test_subscriptions)

# M4 advanced: versions/frontiers, time travel, fractional index, awareness/ephemeral,
# undo manager, jsonpath, commit hooks, change ancestors.
add_executable(loro_test_advanced test_advanced.cpp)
target_link_libraries(loro_test_advanced PRIVATE loro::loro)
add_test(NAME test_advanced COMMAND loro_test_advanced)

# Proves the C ABI is usable from plain C (compiled as C, not C++).
add_executable(loro_test_c_only test_c_only.c)
target_link_libraries(loro_test_c_only PRIVATE loro::loro)
add_test(NAME test_c_only COMMAND loro_test_c_only)

# In manual (non-Corrosion) mode, make every test wait for the cargo staticlib build.
if(TARGET loro_cargo_build)
    add_dependencies(loro_test_doc loro_cargo_build)
    add_dependencies(loro_test_text loro_cargo_build)
    add_dependencies(loro_test_richtext loro_cargo_build)
    add_dependencies(loro_test_cursor loro_cargo_build)
    add_dependencies(loro_test_sync loro_cargo_build)
    add_dependencies(loro_test_diff loro_cargo_build)
    add_dependencies(loro_test_navigate loro_cargo_build)
    add_dependencies(loro_test_map_list loro_cargo_build)
    add_dependencies(loro_test_movable_list loro_cargo_build)
    add_dependencies(loro_test_counter loro_cargo_build)
    add_dependencies(loro_test_tree loro_cargo_build)
    add_dependencies(loro_test_subscriptions loro_cargo_build)
    add_dependencies(loro_test_advanced loro_cargo_build)
    add_dependencies(loro_test_c_only loro_cargo_build)
endif()
