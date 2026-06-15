# Examples. Each is a standalone program linking the public loro::loro target.

add_executable(basic_text basic_text.cpp)
target_link_libraries(basic_text PRIVATE loro::loro)

# rich_text demonstrates the G1 rich-text surface (marks, expand semantics, deltas).
add_executable(rich_text rich_text.cpp)
target_link_libraries(rich_text PRIVATE loro::loro)

add_executable(sync_two_docs sync_two_docs.cpp)
target_link_libraries(sync_two_docs PRIVATE loro::loro)

# subscribe_events demonstrates the M3 event/subscription surface.
add_executable(subscribe_events subscribe_events.cpp)
target_link_libraries(subscribe_events PRIVATE loro::loro)

# In manual (non-Corrosion) mode, make the examples wait for the cargo staticlib build.
if(TARGET loro_cargo_build)
    add_dependencies(basic_text loro_cargo_build)
    add_dependencies(rich_text loro_cargo_build)
    add_dependencies(sync_two_docs loro_cargo_build)
    add_dependencies(subscribe_events loro_cargo_build)
endif()
