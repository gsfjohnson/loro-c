# Examples. Each is a standalone program over the public loro::loro target, using the
# loro-cpp-shaped API (<loro.hpp> + the loro::ext helpers in <loro/loro_ext.hpp>).
#   basic_text, sync_two_docs, subscribe_events  — adopted from loro-cpp/examples.
#   rich_text, cursor_tracking, json_sync         — loro-c demos of the G1/G2/G3 surfaces.

set(_loro_examples
    basic_text sync_two_docs subscribe_events
    rich_text cursor_tracking json_sync)

foreach(_ex IN LISTS _loro_examples)
    add_executable(${_ex} ${_ex}.cpp)
    target_link_libraries(${_ex} PRIVATE loro::loro)
    # In manual (non-Corrosion) mode, wait for the cargo staticlib build.
    if(TARGET loro_cargo_build)
        add_dependencies(${_ex} loro_cargo_build)
    endif()
endforeach()
