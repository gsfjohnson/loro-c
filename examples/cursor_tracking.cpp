// cursor_tracking — the G2 cursor flow: place a stable cursor in a text container, then
// watch its absolute position follow the text as a remote peer concurrently edits before it.
// Also shows encode/decode for transporting a cursor between peers.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/cursor_tracking

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <iostream>
#include <vector>

namespace ext = loro::ext;

int main() {
    // Peer A authors some text and anchors a cursor to the 'w' of "world".
    auto a = loro::LoroDoc::init();
    a->set_peer_id(1);
    auto text = a->get_text(ext::root("body"));
    text->insert(0, "Hello world!");
    a->commit();

    auto cursor = text->get_cursor(6, loro::Side::kLeft);  // anchored to 'w' at index 6
    if (!cursor) {
        std::cerr << "failed to create cursor\n";
        return 1;
    }
    std::cout << "initial:     \"" << text->to_string() << "\" cursor at "
              << a->get_cursor_pos(cursor).current.pos << "\n";

    // The cursor survives transport: encode it, decode it back, same position.
    const std::vector<std::uint8_t> wire = cursor->encode();
    auto restored = loro::Cursor::decode(wire);
    std::cout << "encoded:     " << wire.size() << " bytes -> resolves to "
              << a->get_cursor_pos(restored).current.pos << "\n";

    // Peer B starts from A's snapshot and inserts a greeting at the front, concurrently.
    auto b = loro::LoroDoc::init();
    b->set_peer_id(2);
    b->import(a->export_snapshot());
    auto text_b = b->get_text(ext::root("body"));
    text_b->insert(0, ">> ");
    b->commit();

    // Merge B's change back into A. The numeric index "6" would now be wrong, but the cursor
    // tracks the same logical spot and reports the shifted absolute position.
    a->import(b->export_updates(a->state_vv()));
    const auto pos = a->get_cursor_pos(cursor);
    std::cout << "after merge: \"" << text->to_string() << "\" cursor at "
              << pos.current.pos << "\n";

    // The cursor should still sit immediately before "world" (now at index 9).
    return pos.current.pos == 9 ? 0 : 1;
}
