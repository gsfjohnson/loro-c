// G1: rich text — marks/unmarks, the richtext/delta views, apply_delta round-trip,
// expand semantics via the style config, splice/slice/char_at, UTF-16 ops, convert_pos,
// push_str, and concurrent-mark convergence across two peers.

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

// Counts non-overlapping occurrences of `needle` in `hay`. Used to count delta runs.
static std::size_t count_occurrences(const std::string& hay, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Mark a range bold (a key in the default rich-text config), then read it back through
// both richtext views.
static void test_mark_and_views() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "Hello world!");
    t.mark(0, 5, "bold", "true");
    doc.commit();

    const std::string rv = t.get_richtext_value();
    CHECK(rv.find("Hello") != std::string::npos);
    CHECK(rv.find("bold") != std::string::npos);
    CHECK(rv.find("world") != std::string::npos);

    const std::string delta = t.to_delta();
    CHECK(delta.find("Hello") != std::string::npos);
    CHECK(delta.find("bold") != std::string::npos);

    // Unmarking the tail of the bold range splits it: "Hel"(bold) + "lo world!".
    t.unmark(3, 5, "bold");
    doc.commit();
    const std::string rv2 = t.get_richtext_value();
    CHECK(rv2.find("Hel") != std::string::npos);
}

// to_delta from one doc, apply_delta into a fresh doc, states + formatting match.
static void test_apply_delta_round_trip() {
    loro::Doc a;
    loro::Text ta = a.get_text("t");
    ta.insert(0, "Hello world!");
    ta.mark(0, 5, "bold", "true");
    a.commit();
    const std::string delta = ta.to_delta();

    loro::Doc b;
    loro::Text tb = b.get_text("t");
    tb.apply_delta(delta);
    b.commit();

    CHECK(tb.to_string() == "Hello world!");
    const std::string delta_b = tb.to_delta();
    CHECK(delta_b.find("bold") != std::string::npos);
    CHECK(delta_b.find("Hello") != std::string::npos);
}

// Expand semantics: with After-expand, text inserted right after the mark inherits it
// (one merged run); with None-expand, it does not (two runs). Uses a custom key configured
// per-doc so the two behaviours are isolated.
static void test_expand_semantics() {
    // After: "!" inserted at the boundary inherits the mark -> single bold run.
    {
        loro::Doc doc;
        loro::StyleConfigMap styles;
        styles.insert("fmt", LORO_EXPAND_AFTER);
        doc.config_text_style(styles);

        loro::Text t = doc.get_text("t");
        t.insert(0, "Hello");
        t.mark(0, 5, "fmt", "true");
        doc.commit();
        t.insert(5, "!");
        doc.commit();

        const std::string delta = t.to_delta();
        CHECK(t.to_string() == "Hello!");
        CHECK(count_occurrences(delta, "\"insert\"") == 1);  // merged into one run
        CHECK(delta.find("fmt") != std::string::npos);
    }
    // None: "!" does not inherit the mark -> two runs.
    {
        loro::Doc doc;
        loro::StyleConfigMap styles;
        styles.insert("fmt", LORO_EXPAND_NONE);
        doc.config_text_style(styles);

        loro::Text t = doc.get_text("t");
        t.insert(0, "Hello");
        t.mark(0, 5, "fmt", "true");
        doc.commit();
        t.insert(5, "!");
        doc.commit();

        const std::string delta = t.to_delta();
        CHECK(t.to_string() == "Hello!");
        CHECK(count_occurrences(delta, "\"insert\"") == 2);  // split into two runs
    }
}

// config_default_text_style makes an otherwise-unconfigured key markable.
static void test_default_text_style() {
    loro::Doc doc;
    doc.config_default_text_style(LORO_EXPAND_NONE);
    loro::Text t = doc.get_text("t");
    t.insert(0, "abcdef");
    t.mark(0, 3, "customkey", "true");  // only works because a default style is set
    doc.commit();
    CHECK(t.get_richtext_value().find("customkey") != std::string::npos);
}

// splice returns the removed slice; slice/char_at read substrings.
static void test_splice_slice_char_at() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "hello world");
    doc.commit();

    const std::string removed = t.splice(0, 6, "");  // drop "hello "
    CHECK(removed == "hello ");
    CHECK(t.to_string() == "world");

    CHECK(t.slice(0, 3) == "wor");
    CHECK(t.char_at(0) == "w");
}

// UTF-16 length/insert/delete and convert_pos across coordinate systems, exercised with a
// supplementary-plane character (😀 = 1 codepoint, 4 UTF-8 bytes, 2 UTF-16 code units).
static void test_utf16_and_convert_pos() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "a\xF0\x9F\x98\x80""b");  // "a😀b"
    doc.commit();

    CHECK(t.len_unicode() == 3);
    CHECK(t.len_utf8() == 6);
    CHECK(t.len_utf16() == 4);

    // 'b' is at unicode index 2 -> UTF-16 unit 3, UTF-8 byte 5.
    CHECK(t.convert_pos(2, LORO_POS_UNICODE, LORO_POS_UTF16) == 3);
    CHECK(t.convert_pos(2, LORO_POS_UNICODE, LORO_POS_BYTES) == 5);

    t.insert_utf16(3, "X");  // before 'b'
    doc.commit();
    CHECK(t.to_string() == "a\xF0\x9F\x98\x80""Xb");

    t.remove_utf16(3, 1);  // remove the 'X'
    doc.commit();
    CHECK(t.to_string() == "a\xF0\x9F\x98\x80""b");
}

// push_str appends; update replaces the whole content via a diff.
static void test_push_str_and_update() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "foo");
    t.push_str("bar");
    doc.commit();
    CHECK(t.to_string() == "foobar");

    t.update("Hello World");
    doc.commit();
    CHECK(t.to_string() == "Hello World");
}

// Concurrent marks on two peers merge to a converged, formatted state.
static void test_concurrent_marks() {
    loro::Doc a;
    a.set_peer_id(1);
    loro::Text ta = a.get_text("t");
    ta.insert(0, "Hello world");
    a.commit();

    loro::Doc b;
    b.set_peer_id(2);
    b.import(a.export_snapshot());
    loro::Text tb = b.get_text("t");

    ta.mark(0, 5, "bold", "true");  // "Hello"
    a.commit();
    tb.mark(6, 11, "italic", "true");  // "world"
    b.commit();

    a.import(b.export_updates());
    b.import(a.export_updates());

    const std::string da = ta.to_delta();
    const std::string db = tb.to_delta();
    CHECK(da == db);  // converged
    CHECK(da.find("bold") != std::string::npos);
    CHECK(da.find("italic") != std::string::npos);
}

int main() {
    test_mark_and_views();
    test_apply_delta_round_trip();
    test_expand_semantics();
    test_default_text_style();
    test_splice_slice_char_at();
    test_utf16_and_convert_pos();
    test_push_str_and_update();
    test_concurrent_marks();

    if (failures == 0) {
        std::puts("test_richtext: OK");
        return 0;
    }
    std::fprintf(stderr, "test_richtext: %d failure(s)\n", failures);
    return 1;
}
