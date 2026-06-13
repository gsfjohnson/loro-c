# M1 examples. Each is a standalone program linking the public loro::loro target.
# subscribe_events.cpp lands with the event/subscription API in M3 (see pm/CBINDGEN_PLAN.md).

add_executable(basic_text basic_text.cpp)
target_link_libraries(basic_text PRIVATE loro::loro)

add_executable(sync_two_docs sync_two_docs.cpp)
target_link_libraries(sync_two_docs PRIVATE loro::loro)

# In manual (non-Corrosion) mode, make the examples wait for the cargo staticlib build.
if(TARGET loro_cargo_build)
    add_dependencies(basic_text loro_cargo_build)
    add_dependencies(sync_two_docs loro_cargo_build)
endif()
