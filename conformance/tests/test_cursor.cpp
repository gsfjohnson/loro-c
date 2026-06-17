// Cursor conformance (RESHAPE Phase 5).
//
// loro-cpp ships no test_cursor.cpp, so this loro-c-authored test covers the cursor surface
// fleshed out in Phase 5:
//   * container get_cursor(pos, side),
//   * encode() / decode() round-trip,
//   * get_cursor_pos() — both the resolved `current` AbsolutePosition and the `update` cursor
//     (the latter backed by the new loro_doc_get_cursor_pos_full C ABI primitive), and
//   * Cursor::init(...) constructing a cursor from parts (backed by the new loro_cursor_new
//     C ABI primitive).
//
// loro-c-authored (not adopted from loro-cpp); built only under the conformance spike since it
// includes the spike <loro.hpp>.
#include "test_helpers.hpp"

#include <vector>

using namespace loro_test;

namespace {

// get_cursor on a container, encode/decode round-trip, resolve, and track edits.
bool test_text_cursor() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto text = doc->get_text(root("body"));
    text->insert(0, "hello");
    doc->commit();

    auto cursor = text->get_cursor(2, loro::Side::kLeft);
    if (!cursor) return fail("get_cursor returned null");

    auto encoded = cursor->encode();
    if (encoded.empty()) return fail("cursor encode produced empty bytes");
    auto decoded = loro::Cursor::decode(encoded);
    if (!decoded) return fail("cursor decode returned null");

    auto pos = doc->get_cursor_pos(decoded);
    if (pos.current.pos != 2) return fail("decoded cursor resolved to wrong position");

    // Insert two codepoints entirely before the anchor: the anchored element shifts by +2.
    text->insert(0, "XY");  // "XYhello"
    doc->commit();
    auto pos2 = doc->get_cursor_pos(cursor);
    if (pos2.current.pos != 4) return fail("cursor did not track edits (expected pos 4)");
    return true;
}

// get_cursor_pos must surface the `update` cursor (loro-cpp PosQueryResult.update). loro only
// supplies one when the anchored element was DELETED and it had to re-anchor (a plain shift keeps
// the same id, so update stays null) — so delete the anchored char to force the trace-back path.
bool test_cursor_pos_update() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto text = doc->get_text(root("body"));
    text->insert(0, "hello");
    doc->commit();

    // get_cursor(pos, side) anchors to the element AT index `pos` — here the 'l' at index 2.
    auto cursor = text->get_cursor(2, loro::Side::kLeft);
    if (!cursor) return fail("get_cursor returned null");

    text->delete_(2, 1);  // delete the anchored 'l' -> "helo"
    doc->commit();

    auto pos = doc->get_cursor_pos(cursor);
    if (!pos.update)
        return fail("get_cursor_pos did not yield an update cursor after the anchor was deleted");
    // The updated cursor itself re-resolves against the current doc without error.
    auto pos_again = doc->get_cursor_pos(pos.update);
    if (pos_again.current.pos > text->len_unicode())
        return fail("update cursor re-resolved out of range");
    return true;
}

// Cursor::init builds a cursor from its parts (container id + side + origin pos).
bool test_cursor_init() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto text = doc->get_text(root("body"));
    text->insert(0, "hello");
    doc->commit();

    auto cursor = loro::Cursor::init(std::nullopt, text->id(), loro::Side::kLeft, 0);
    if (!cursor) return fail("Cursor::init returned null");

    // It must encode/decode and resolve against the document without error.
    auto round = loro::Cursor::decode(cursor->encode());
    if (!round) return fail("Cursor::init cursor failed encode/decode round-trip");
    auto pos = doc->get_cursor_pos(cursor);
    if (pos.current.pos > text->len_unicode()) return fail("init cursor resolved out of range");
    return true;
}

bool run() {
    if (!test_text_cursor()) return false;
    if (!test_cursor_pos_update()) return false;
    if (!test_cursor_init()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
