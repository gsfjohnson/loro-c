// G2: cursors — stable positions that survive concurrent edits. Covers text/list/movable-list
// cursor creation, position tracking through local inserts/deletes, the configured Side (and
// its survival through anchor deletion), encode/decode round-trip, and the headline scenario:
// a remote peer inserts before the cursor and after merge get_cursor_pos reports the shift.

#include <loro/loro.hpp>

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// A text cursor at the end of the document tracks through inserts and deletes (the exact
// sequence from loro's own get_cursor doc-example).
static void test_text_cursor_tracking() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "01234");
    doc.commit();

    auto cur = t.get_cursor(5);  // position 5 == end of "01234"
    CHECK(cur.has_value());
    CHECK(doc.get_cursor_pos(*cur).abs_pos == 5);

    t.insert(0, "01234");  // shift everything right by 5
    doc.commit();
    CHECK(doc.get_cursor_pos(*cur).abs_pos == 10);

    t.remove(0, 10);  // delete everything
    doc.commit();
    CHECK(doc.get_cursor_pos(*cur).abs_pos == 0);

    t.insert(0, "01234");
    doc.commit();
    CHECK(doc.get_cursor_pos(*cur).abs_pos == 5);
}

// The configured Side is reflected by the resolved position, and a Left-anchored cursor keeps
// that side even after its anchor character is deleted.
static void test_side_and_anchor_deletion() {
    // The resolved side mirrors the side the cursor was created with.
    for (loro::Side side : {LORO_SIDE_LEFT, LORO_SIDE_MIDDLE, LORO_SIDE_RIGHT}) {
        loro::Doc doc;
        loro::Text t = doc.get_text("t");
        t.insert(0, "Hello");
        doc.commit();
        auto cur = t.get_cursor(2, side);
        CHECK(cur.has_value());
        const loro::PosQueryResult r = doc.get_cursor_pos(*cur);
        CHECK(r.abs_pos == 2);
        CHECK(r.side == side);
    }

    // Deleting the anchor still resolves; a Left side survives the deletion.
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "Hello");  // indices 0..4
    doc.commit();
    auto cur = t.get_cursor(2, LORO_SIDE_LEFT);  // anchor element at index 2
    CHECK(cur.has_value());

    t.remove(2, 1);  // delete the anchored character -> "Helo"
    doc.commit();
    const loro::PosQueryResult r = doc.get_cursor_pos(*cur);
    CHECK(r.abs_pos == 2);
    CHECK(r.side == LORO_SIDE_LEFT);  // configured side preserved through deletion
}

// encode -> decode reproduces a cursor that resolves to the same position.
static void test_encode_decode_round_trip() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "Hello world");
    doc.commit();

    auto cur = t.get_cursor(6);  // before 'w'
    CHECK(cur.has_value());
    const std::vector<std::uint8_t> bytes = cur->encode();
    CHECK(!bytes.empty());

    loro::Cursor decoded = loro::Cursor::decode(bytes);
    CHECK(doc.get_cursor_pos(decoded).abs_pos == doc.get_cursor_pos(*cur).abs_pos);
    CHECK(doc.get_cursor_pos(decoded).abs_pos == 6);
}

// The headline scenario: a remote peer inserts before the cursor; after merge the resolved
// absolute position shifts accordingly.
static void test_concurrent_remote_insert_shifts_cursor() {
    loro::Doc a;
    a.set_peer_id(1);
    loro::Text ta = a.get_text("t");
    ta.insert(0, "Hello world");
    a.commit();

    auto cur = ta.get_cursor(3);  // anchored within doc a
    CHECK(cur.has_value());
    CHECK(a.get_cursor_pos(*cur).abs_pos == 3);

    // A remote peer starts from a's snapshot and inserts "XYZ" at the front.
    loro::Doc b;
    b.set_peer_id(2);
    b.import(a.export_snapshot());
    loro::Text tb = b.get_text("t");
    tb.insert(0, "XYZ");
    b.commit();

    // Merge the remote change back into a; the cursor now sits 3 positions later.
    a.import(b.export_updates());
    CHECK(ta.to_string() == "XYZHello world");
    CHECK(a.get_cursor_pos(*cur).abs_pos == 6);
}

// List and movable-list cursors track position through inserts.
static void test_list_cursors() {
    {
        loro::Doc doc;
        loro::List l = doc.get_list("l");
        l.insert(0, "0");
        doc.commit();
        auto cur = l.get_cursor(0);
        CHECK(cur.has_value());
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 0);

        l.insert(0, "0");
        doc.commit();
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 1);

        l.insert(0, "0");
        l.insert(0, "0");
        doc.commit();
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 3);

        l.insert(4, "0");  // after the cursor: no shift
        doc.commit();
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 3);
    }
    {
        loro::Doc doc;
        loro::MovableList l = doc.get_movable_list("ml");
        l.push("\"a\"");
        l.push("\"b\"");
        doc.commit();
        auto cur = l.get_cursor(1, LORO_SIDE_LEFT);  // anchored at "b"
        CHECK(cur.has_value());
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 1);

        l.insert(0, "\"x\"");  // insert before -> shift right
        doc.commit();
        CHECK(doc.get_cursor_pos(*cur).abs_pos == 2);
    }
}

int main() {
    test_text_cursor_tracking();
    test_side_and_anchor_deletion();
    test_encode_decode_round_trip();
    test_concurrent_remote_insert_shifts_cursor();
    test_list_cursors();

    if (failures == 0) {
        std::puts("test_cursor: OK");
        return 0;
    }
    std::fprintf(stderr, "test_cursor: %d failure(s)\n", failures);
    return 1;
}
