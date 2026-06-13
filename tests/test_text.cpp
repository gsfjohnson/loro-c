// M1: LoroText edits, the snapshot -> import round trip, and the strong-co-ownership
// free-ordering case (a Text outliving its parent Doc).

#include <loro/loro.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void test_basic_edits() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    CHECK(t.empty());

    t.insert(0, "hello");
    doc.commit();
    CHECK(!t.empty());
    CHECK(t.len_unicode() == 5);
    CHECK(t.len_utf8() == 5);
    CHECK(t.to_string() == "hello");

    // Insert at the end, then delete a range.
    t.insert(5, " world");
    doc.commit();
    CHECK(t.to_string() == "hello world");
    CHECK(t.len_unicode() == 11);

    t.remove(0, 6);  // drop "hello "
    doc.commit();
    CHECK(t.to_string() == "world");

    // Multibyte: a non-ASCII codepoint counts as 1 unicode unit but >1 utf8 byte.
    loro::Text u = doc.get_text("u");
    u.insert(0, "é");  // U+00E9, 2 UTF-8 bytes
    doc.commit();
    CHECK(u.len_unicode() == 1);
    CHECK(u.len_utf8() == 2);
}

// Primary M1 verification: edit -> export_snapshot -> import into a fresh doc -> read back.
static void test_snapshot_round_trip() {
    std::vector<std::uint8_t> snapshot;
    {
        loro::Doc doc;
        loro::Text t = doc.get_text("t");
        t.insert(0, "hello");
        doc.commit();
        snapshot = doc.export_snapshot();
    }
    CHECK(!snapshot.empty());

    loro::Doc fresh;
    fresh.import(snapshot);
    CHECK(fresh.get_text("t").to_string() == "hello");
}

// Free-ordering / strong-co-ownership: destroy the Doc while a Text is still held, then
// use and destroy the Text. Must not use-after-free and must not leak (verified under a
// leak checker in CI). The Text keeps the whole document state alive.
static void test_free_ordering() {
    auto doc = std::make_unique<loro::Doc>();
    loro::Text t = doc->get_text("t");
    t.insert(0, "hello");
    doc->commit();

    doc.reset();  // free the LoroDoc while `t` is still alive

    // `t` remains valid because it co-owns the document state.
    CHECK(t.to_string() == "hello");
    CHECK(t.len_unicode() == 5);
    t.insert(5, "!");  // still mutable
    CHECK(t.to_string() == "hello!");
}  // `t` destroyed here, after the doc — releases the last reference, no leak

int main() {
    test_basic_edits();
    test_snapshot_round_trip();
    test_free_ordering();

    if (failures == 0) {
        std::puts("test_text: OK");
        return 0;
    }
    std::fprintf(stderr, "test_text: %d failure(s)\n", failures);
    return 1;
}
