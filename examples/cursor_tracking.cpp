// cursor_tracking — the G2 cursor flow: place a stable cursor in a text container, then
// watch its absolute position follow the text as a remote peer concurrently edits before it.
// Also shows encode/decode for transporting a cursor between peers.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/cursor_tracking

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    std::printf("loro %s\n", loro::version().c_str());

    // Peer A authors some text and places a cursor just before "world".
    loro::Doc a;
    a.set_peer_id(1);
    loro::Text text = a.get_text("body");
    text.insert(0, "Hello world!");
    a.commit();

    auto cursor = text.get_cursor(6);  // anchored before 'w'
    if (!cursor) {
        std::fprintf(stderr, "failed to create cursor\n");
        return 1;
    }
    std::printf("initial:  \"%s\" cursor at %zu\n", text.to_string().c_str(),
                a.get_cursor_pos(*cursor).abs_pos);

    // The cursor survives transport: encode it, decode it back, same position.
    const std::vector<std::uint8_t> wire = cursor->encode();
    loro::Cursor restored = loro::Cursor::decode(wire);
    std::printf("encoded:  %zu bytes -> resolves to %zu\n", wire.size(),
                a.get_cursor_pos(restored).abs_pos);

    // Peer B starts from A's snapshot and inserts a greeting at the front, concurrently.
    loro::Doc b;
    b.set_peer_id(2);
    b.import(a.export_snapshot());
    loro::Text text_b = b.get_text("body");
    text_b.insert(0, ">> ");
    b.commit();

    // Merge B's change back into A. The numeric index "6" would now be wrong, but the
    // cursor tracks the same logical spot and reports the shifted absolute position.
    a.import(b.export_updates());
    const loro::PosQueryResult pos = a.get_cursor_pos(*cursor);
    std::printf("after merge: \"%s\" cursor at %zu\n", text.to_string().c_str(), pos.abs_pos);

    // The cursor should still sit immediately before "world" (now at index 9).
    return pos.abs_pos == 9 ? 0 : 1;
}
