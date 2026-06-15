// G4: diff / patch — compute the diff between two versions, replay it onto another document,
// and revert a document to an earlier version. A diff crosses the API as an opaque DiffBatch
// (losslessly carrying live nested containers); to_json() is for inspection only.

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

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// diff(F0, F1) captured on doc A, applied onto a clone frozen at F0, reproduces A's F1 state.
static void test_diff_apply_across_docs() {
    loro::Doc a;
    loro::Text ta = a.get_text("t");
    ta.insert(0, "hello");
    a.commit();
    loro::Frontiers f0 = a.state_frontiers();

    ta.insert(5, " world");
    a.commit();
    loro::Frontiers f1 = a.state_frontiers();

    loro::DiffBatch batch = a.diff(f0, f1);
    const std::string dj = batch.to_json();
    CHECK(contains(dj, "insert"));
    CHECK(contains(dj, "world"));

    // A document frozen at F0 plus the diff equals A at F1.
    loro::Doc b;
    b.import(a.export_snapshot_at(f0));
    loro::Text tb = b.get_text("t");
    CHECK(tb.to_string() == "hello");
    b.apply_diff(batch);
    CHECK(tb.to_string() == "hello world");

    // apply_diff does not consume the batch: it can be applied again (here onto a fresh fork).
    loro::Doc c;
    c.import(a.export_snapshot_at(f0));
    c.apply_diff(batch);
    CHECK(c.get_text("t").to_string() == "hello world");
}

// revert_to rewinds the document, recording the inverse as new ops (the doc stays attached).
static void test_revert_to() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "hello");
    doc.commit();
    loro::Frontiers f0 = doc.state_frontiers();

    t.insert(5, " world");
    doc.commit();
    CHECK(t.to_string() == "hello world");

    doc.revert_to(f0);
    CHECK(t.to_string() == "hello");
    CHECK(!doc.is_detached());  // unlike checkout(), revert stays attached
}

// A diff that creates a nested container round-trips losslessly through the opaque batch — the
// case a JSON-only patch surface could not represent.
static void test_diff_with_nested_container() {
    loro::Doc a;
    loro::Map m = a.get_map("m");
    a.commit();
    loro::Frontiers f0 = a.state_frontiers();

    loro::Container inner = m.insert_container("child", loro::Container::map());
    inner.as_map().insert("k", "\"v\"");
    a.commit();
    loro::Frontiers f1 = a.state_frontiers();

    loro::DiffBatch batch = a.diff(f0, f1);

    loro::Doc b;
    b.import(a.export_snapshot_at(f0));
    b.apply_diff(batch);
    // The nested map and its entry survived the apply.
    const std::string bj = b.to_json();
    CHECK(contains(bj, "\"k\""));
    CHECK(contains(bj, "\"v\""));
}

int main() {
    test_diff_apply_across_docs();
    test_revert_to();
    test_diff_with_nested_container();

    if (failures == 0) {
        std::puts("test_diff: OK");
        return 0;
    }
    std::fprintf(stderr, "test_diff: %d failure(s)\n", failures);
    return 1;
}
